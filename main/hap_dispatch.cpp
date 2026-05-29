// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <cstdio>
#include <cstdarg>
#include <cassert>
#include <unistd.h>
#include <sys/time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "task_stacks.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_task_wdt.h"
#include "hap_slave.h"
#include "hap_session.h"
#include "hap_json.h"
#include "metrics/metrics.h"
#include "metrics/metrics_export_prometheus.h"
#include "lua_engine.h"
#include "lua_engine_scripts.h"
#include "event_bus.h"
#include "zigbee_mgr.h"
#include "zhc_adapter.h"
#include "device_shadow.h"
#include "zigbee_pool.h"
#include "zap_store.h"
#include "simple_rules.h"
#include "rule_store.h"
#include "zigbee_diagnostics.h"
#include "esp_timer.h"
// zcl_defs_generated.h removed — legacy pipeline dropped.
#include "mqtt_gw.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_random.h"
#include "p4_ota.h"
#include "device_backend.h"
#include "hap_dispatch.h"

static const char* TAG = "p4_main";

static constexpr int      BATTERY_LOW_THRESHOLD_PCT = 20;
static constexpr uint32_t HAP_RESPONSE_TIMEOUT_MS   = 5000;

void hap_send(HapMsgType type, const uint8_t* payload,
              uint16_t payload_len, uint8_t flags,
              uint16_t ack_seq) {
    HapFrame f{};
    f.type        = type;
    f.seq         = hap_session_next_seq();
    f.ack_seq     = ack_seq;
    f.flags       = flags;
    f.payload     = payload;
    f.payload_len = payload_len;
    hap_session_send(f);
}

// CC-F6: the BULK_STATE_UPDATE batching path (s_bulk_buf, bulk_push,
// flush_bulk) was kept "for a future batched-broadcast path" but never
// activated — bulk_push had zero callers and flush_bulk's 100 ms tick
// always saw count=0. Removed so the type tag is unambiguously the
// per-attr device_update payload emitted from on_zcl_attr_for_hap (see
// HAP-F3 in docs/FINDINGS.md for the type-tag-reuse smell that the
// removal also resolves on the SPA side).

// P-F1 (docs/OPTIMIZATIONS.md): one shared TX scratch for every HAP
// *dispatch* handler. The dispatcher is the hap_session on_frame
// callback, which runs on hap_slave_task; every registered handler runs
// there, one at a time, and `hap_send` copies the payload into the
// session's own slot buffer before returning, so the scratch is free to
// be reused immediately. Handlers must take it via `auto& tx_buf =
// hap_tx_scratch()` (below): that asserts the single-task invariant and
// returns a reference to the array so `sizeof(tx_buf)` still works.
//
// IMPORTANT: code that runs on task_hap (NOT the dispatch task) must NOT
// touch s_hap_tx. hap_slave_task is higher priority than task_hap and
// preempts it, so a task_hap writer sharing s_hap_tx can be interrupted
// mid-encode by a dispatch handler that re-encodes the same buffer; the
// two-stage SPI CRC is computed after the corruption, so the torn frame
// still passes CRC and the S3 receives malformed data (FINDINGS.md
// Finding 4 — the same class as the original F-08 panic). The two
// task_hap writers therefore keep their own buffers: `send_heartbeat`
// uses `hb_buf`, and `emit_attr_update` uses `s_attr_tx`.
//
// Park the 4 KB outbound HAP scratch buffers in PSRAM. Sequential memcpy
// + SPI queue copy only; not touched in ISR or DMA. Frees the same off
// internal RAM (req: CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y).
EXT_RAM_BSS_ATTR static uint8_t s_hap_tx[HAP_MAX_PAYLOAD];

// Dedicated scratch for emit_attr_update — it runs on task_hap, not the
// hap_slave_task dispatcher, so it must never alias s_hap_tx. See note
// above (FINDINGS.md Finding 4).
EXT_RAM_BSS_ATTR static uint8_t s_attr_tx[HAP_MAX_PAYLOAD];

// ── Async HAP-tx queue (sync-SPI fix from docs/OPTIMIZATIONS.md hot-
// path investigation, 2026-04-27) ────────────────────────────────────
// Previously every `EventType::ZCL_ATTR` published from
// `device_shadow_process` ran the JSON encode + `hap_send` (SPI
// master-side ship) **synchronously inside the publishing task** —
// shadow / event-bus / cron — stalling that task for the full SPI
// roundtrip. At 5 attr-events/s the publisher accumulated tens of ms
// of CPU on whichever core it ran on. Now the subscriber just copies
// the 96 B `ZclAttrEvent` into this queue and returns; TaskHAP drains
// the queue as part of its existing 10 ms tick loop and runs the
// encode + send on its own stack, decoupling Zigbee RX from SPI ship.
struct HapTxItem {
    ZclAttrEvent attr;   // 96 B
};
static constexpr size_t HAP_TX_QUEUE_DEPTH = 32;
static QueueHandle_t s_hap_tx_q = nullptr;

// F-08: handles that reuse static-local scratch (raw[64], slots[64],
// src_buf[]) must only run from one task — otherwise the buffers race.
// The actual dispatcher is hap_slave_task (frames arrive via the
// hap_session on_frame callback), not task_hap. Captured lazily on the
// first guarded call so the check works regardless of which task owns
// dispatch.
static TaskHandle_t s_hap_task = nullptr;

static inline void hap_dispatch_assert_single_task() {
    TaskHandle_t cur = xTaskGetCurrentTaskHandle();
    if (s_hap_task == nullptr) { s_hap_task = cur; return; }
    assert(cur == s_hap_task);
}

// Hand a dispatch handler the shared TX scratch, asserting it runs on the
// single dispatch task (hap_slave_task). Returns a reference to the array
// so callers keep using `sizeof(tx_buf)`. task_hap writers
// (emit_attr_update, send_heartbeat) must use their own buffers — see the
// s_hap_tx note above (FINDINGS.md Finding 4).
static auto hap_tx_scratch() -> uint8_t (&)[HAP_MAX_PAYLOAD] {
    hap_dispatch_assert_single_task();
    return s_hap_tx;
}

// Forward decl — `send_alert` is defined below for the
// existing event-bus subscribers, but `emit_attr_update` (above) now
// uses it from the LOW_BATTERY path.
void send_alert(HapAlertCode code, uint64_t ieee, const char* msg);

// Encode + send one queued attr-update — called only from TaskHAP.
static void emit_attr_update(const ZclAttrEvent& ze) {
    if (ze.key[0] == '_') return;  // synthetic internal attr (e.g. "_last_seen")

    const char* str_val = (ze.val_type == VAL_STR) ? ze.str_val : nullptr;
    uint8_t  lqi       = 0;
    uint32_t last_seen = 0;
    if (const ZapDevice* d = pool_find_by_ieee(ze.ieee)) {
        lqi       = d->link_quality;
        last_seen = d->last_seen;
    }
    // Own scratch: emit_attr_update runs on task_hap, NOT the
    // hap_slave_task dispatcher, so it must not share s_hap_tx with the
    // dispatch handlers (FINDINGS.md Finding 4 — torn CRC-valid frames).
    auto& tx_buf = s_attr_tx;
    uint16_t tx_len = 0;
    if (hap_json_encode_device_attr_update(tx_buf, sizeof(tx_buf), &tx_len,
                                           ze.ieee, ze.key,
                                           ze.val_type,
                                           ze.int_val, str_val,
                                           lqi, last_seen)) {
        hap_send(HapMsgType::BULK_STATE_UPDATE, tx_buf, tx_len, HAP_FLAG_NO_ACK);
    }

    // Trigger LOW_BATTERY alert when battery drops below 20% — ZCL
    // cluster 0x0001 attr 0x0021 is scaled to 0–100. Stays in TaskHAP
    // context (send_alert ultimately calls hap_send).
    if (ze.cluster == 0x0001 && ze.attr_id == 0x0021 && ze.val_type == VAL_INT) {
        if (ze.int_val < BATTERY_LOW_THRESHOLD_PCT && ze.int_val >= 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "battery %d%% on device", (int)ze.int_val);
            send_alert(HapAlertCode::LOW_BATTERY, ze.ieee, msg);
        }
    }
}

// Called from event_bus subscription and from main.cpp (rule error callbacks).
// Non-static so main.cpp can reference it via forward declaration.
void send_alert(HapAlertCode code, uint64_t ieee, const char* msg) {
    HapAlert a{};
    a.code = code;
    a.ieee = ieee;
    a.ts   = (uint32_t)(esp_timer_get_time() / 1000000UL);
    strncpy(a.msg, msg, sizeof(a.msg) - 1);
    uint8_t buf[160];
    uint16_t len = 0;
    if (!hap_json_encode_alert(buf, sizeof(buf), &len, a)) return;
    hap_send(HapMsgType::ALERT, buf, len, 0);
}

// Subscriber called from event_bus_publish — runs on whichever task
// fired the publish (typically zcl_attr_task or task_shadow). Must
// stay cheap so the publishing task isn't stalled by SPI ship; the
// real encode + send happens in TaskHAP via emit_attr_update().
static void on_zcl_attr_for_hap(const Event& ev) {
    const auto& ze = *reinterpret_cast<const ZclAttrEvent*>(ev.data);
    if (ze.key[0] == '_') return;  // synthetic internal attr (e.g. "_last_seen")
    if (!s_hap_tx_q) return;       // queue not yet created (boot race)

    HapTxItem item{};
    item.attr = ze;
    if (xQueueSend(s_hap_tx_q, &item, 0) != pdTRUE) {
        // Queue full — drop the oldest and retry once. At 32 slots
        // and TaskHAP draining every 10 ms this only fires under
        // sustained burst (>3200 attrs/s), well past the 5 msg/s
        // workload that motivated this fix.
        HapTxItem stale;
        if (xQueueReceive(s_hap_tx_q, &stale, 0) == pdTRUE) {
            xQueueSend(s_hap_tx_q, &item, 0);
        }
    }
}

// Called from event_bus subscription — notify S3 when a device leaves
static void on_device_leave_for_hap(const Event& ev) {
    uint64_t ieee = 0;
    memcpy(&ieee, ev.data, sizeof(ieee));

    uint8_t buf[48];
    uint16_t len = 0;
    if (!hap_json_encode_device_join(buf, sizeof(buf), &len, ieee)) return;

    hap_send(HapMsgType::DEVICE_LEAVE, buf, len, 0);
}

// Called from event_bus subscription — notify S3 of new device joins
static void on_device_event_for_hap(const Event& ev) {
    uint64_t ieee = 0;
    memcpy(&ieee, ev.data, sizeof(ieee));

    uint8_t buf[48];
    uint16_t len = 0;
    if (!hap_json_encode_device_join(buf, sizeof(buf), &len, ieee)) return;

    hap_send(HapMsgType::DEVICE_JOIN, buf, len, 0);
}

// Per-core CPU sampling lives in zap_common/sys_metrics.h — shared with
// the S3 status handler.
#include "sys_metrics.h"

// ── populate_mem_metrics ──────────────────────────────────────────────────
//
// Fills the memory-diagnostic fields of a HapHeartbeat with values from
// esp_get_*heap_size and heap_caps_get_*. `task_stack_hwm_bytes` is the
// smallest per-task stack high-water-mark across the P4 task table — a
// single number summarising "most-at-risk task." The per-task table is
// still available in the 60 s `Stack HWM (P4)` log; this is the rolled-
// up number surfaced to the UI on every heartbeat.
static void populate_mem_metrics(HapHeartbeat& hb) {
    hb.heap                    = esp_get_free_heap_size();
    hb.heap_min_free           = esp_get_minimum_free_heap_size();
    hb.internal_free           = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    hb.internal_min_free       = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    hb.internal_largest_block  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    hb.psram_free              = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    hb.psram_total             = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    hb.psram_min_free          = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    hb.psram_largest_block     = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    // Min stack HWM across the fixed P4 task list (names match the
    // table in main.cpp / task_stack_mon).
    uint32_t min_hwm = UINT32_MAX;
    for (const auto* e = zhac::stack::kTable; e->name != nullptr; ++e) {
        TaskHandle_t h = xTaskGetHandle(e->name);
        if (!h) continue;
        uint32_t free_b = (uint32_t)uxTaskGetStackHighWaterMark(h) *
                          (uint32_t)sizeof(StackType_t);
        if (free_b < min_hwm) min_hwm = free_b;
    }
    hb.task_stack_hwm_bytes = (min_hwm == UINT32_MAX) ? 0 : min_hwm;
}

// ── Rule/script response helpers ──────────────────────────────────────────
static void send_exec_result(HapMsgType type, const HapRuleExecResult& r) {
    auto& tx_buf = hap_tx_scratch();
    uint16_t tx_len = 0;
    if (!hap_json_encode_rule_exec_result(tx_buf, sizeof(tx_buf), &tx_len, r)) {
        ESP_LOGE(TAG, "encode_exec_result failed"); return;
    }
    hap_send(type, tx_buf, tx_len, HAP_FLAG_NO_ACK);
}

// Resolver thunk used by the hap_json device encoders to look up
// friendly (vendor, model) labels from the ZHC def catalogue. Lives
// in hap_dispatch because hap_json is shared between P4 and S3 and
// cannot depend on zhc_adapter directly.
static void resolve_dev_labels(const ZapDevice* dev,
                                 char* vendor_out, size_t vendor_cap,
                                 char* model_out,  size_t model_cap) {
    if (!dev) return;
    zhac_adapter_resolve_labels(dev->model_id, dev->manufacturer_name,
                                 vendor_out, vendor_cap,
                                 model_out,  model_cap);
}

// ── GET_DEVICES handler ───────────────────────────────────────────────────
static void handle_get_devices(const HapFrame& req) {
    auto& tx_buf = hap_tx_scratch();
    uint16_t len = 0;
    bool ok = hap_json_encode_device_list(
        tx_buf, sizeof(tx_buf), &len,
        pool_all(), pool_count(),
        &resolve_dev_labels);

    if (!ok) {
        ESP_LOGE(TAG, "GET_DEVICES encode failed");
        return;
    }

    hap_send(HapMsgType::DEVICE_LIST, tx_buf, len, HAP_FLAG_NEEDS_ACK, req.seq);
}

// ── GET_DEVICE_BY_ID handler ──────────────────────────────────────────────
// Emit the cached shadow attrs as a JSON object (e.g.
// `{"state":1,"battery":85}`) for the device-info splicer. Returns the
// number of bytes written, or 0 if no attrs survived the `_`-prefix
// filter or the buffer would overflow. Must run on task_hap (the shared
// `sa` scratch is task-serialised).
static size_t emit_attrs_for_dev(const ZapDevice* dev, char* buf, size_t cap) {
    static ShadowAttr sa[32];
    uint8_t nsa = device_shadow_get_attrs(dev->ieee_addr, sa, 32);
    if (nsa == 0 || cap < 2) return 0;
    size_t pos = 0;
    buf[pos++] = '{';
    bool first = true;
    for (uint8_t i = 0; i < nsa; i++) {
        if (sa[i].key[0] == '\0' || sa[i].key[0] == '_') continue;
        char valbuf[sizeof(sa[i].str_val) * 2 + 3]; // worst-case "..."  +NUL
        if (sa[i].val_type == VAL_STR) {
            // str_val originates from a Zigbee device report — a stray
            // `"` or control byte must not break the JSON envelope of
            // every device-details fetch. Escape per RFC 8259.
            char esc[sizeof(sa[i].str_val) * 2 + 1];
            hap_json_escape_str(sa[i].str_val, esc, sizeof(esc));
            snprintf(valbuf, sizeof(valbuf), "\"%s\"", esc);
        } else if (sa[i].val_type == VAL_BOOL) {
            snprintf(valbuf, sizeof(valbuf), "%s", sa[i].int_val ? "true" : "false");
        } else {
            snprintf(valbuf, sizeof(valbuf), "%ld", (long)sa[i].int_val);
        }
        // Keys are produced by ZHC and capped at 20 chars; escape them
        // anyway so a corrupted NVS shadow can't poison the response.
        char key_esc[sizeof(sa[i].key) * 2 + 1];
        hap_json_escape_str(sa[i].key, key_esc, sizeof(key_esc));
        int w = snprintf(buf + pos, cap - pos,
                         "%s\"%s\":%s", first ? "" : ",", key_esc, valbuf);
        if (w <= 0 || (size_t)w >= cap - pos) return 0;
        pos += (size_t)w;
        first = false;
    }
    if (first) return 0;  // every attr was filtered
    if (pos + 1 > cap) return 0;
    buf[pos++] = '}';
    return pos;
}

static size_t emit_exposes_for_dev(const ZapDevice* dev, char* buf, size_t cap) {
    return zhac_adapter_build_exposes_json(dev->ieee_addr,
                                            dev->model_id,
                                            dev->manufacturer_name,
                                            buf, cap);
}

static void handle_get_device_by_id(const HapFrame& req) {
    hap_dispatch_assert_single_task();  // F-08: emit_attrs_for_dev uses a static scratch
    auto& tx_buf = hap_tx_scratch();
    uint16_t len = 0;

    uint64_t ieee = 0;
    if (!hap_json_decode_get_device_req(req.payload, req.payload_len, &ieee)) {
        ESP_LOGE(TAG, "GET_DEVICE_BY_ID decode failed");
        return;
    }

    const ZapDevice* dev = pool_find_by_ieee(ieee);
    bool ok = dev
        ? hap_json_encode_device_info_full(tx_buf, sizeof(tx_buf), &len, dev,
                                             &resolve_dev_labels,
                                             &emit_attrs_for_dev,
                                             &emit_exposes_for_dev)
        : hap_json_encode_device_info_err(tx_buf, sizeof(tx_buf), &len, "not found");

    if (!ok) { ESP_LOGE(TAG, "DEVICE_INFO encode failed"); return; }

    hap_send(HapMsgType::DEVICE_INFO, tx_buf, len, HAP_FLAG_NEEDS_ACK, req.seq);
}

// ── SET_ATTRIBUTE handler ─────────────────────────────────────────────────
static void handle_set_attribute(const HapFrame& req) {
    HapSetAttrReq attr{};
    if (!hap_json_decode_set_attr(req.payload, req.payload_len, attr)) {
        ESP_LOGE(TAG, "SET_ATTR decode failed");
        return;
    }

    // Route command: try protocol-agnostic backend dispatch first, then Zigbee-specific fallback
    bool ok = false;
    bool sent = false;
    auto& tx_buf = hap_tx_scratch();
    uint16_t len = 0;
    {
        const char* key = (attr.key[0] != '\0') ? attr.key : "state";

        // Key-based dispatch through DeviceBackend (protocol-agnostic path)
        if (attr.cluster == 0 && attr.attr == 0) {
            DeviceBackend* b = device_backend_find_by_ieee(attr.ieee);
            if (b && b->write_attr) {
                ok = b->write_attr(attr.ieee, attr.ep, key, attr.val);
            } else {
                ESP_LOGW(TAG, "SET_ATTR no backend for ieee=0x%llx", (unsigned long long)attr.ieee);
            }
        } else {
            // Cluster-based dispatch (Zigbee ZCL path — backward compat)
            ZapDevice* dev = pool_find_by_ieee(attr.ieee);
            uint8_t ep = (attr.ep != 0) ? attr.ep
                         : (dev && dev->endpoint_count > 0 ? dev->endpoints[0] : 1);

            // Try the zhc library's write path first — it covers the
            // generated Tuya/Moes DP devices (663+ as of this writing)
            // which the legacy `zcl_converter_match` does not know.
            // Returns false cleanly when no TzConverter claims `key`,
            // so legacy dispatch still fires.
            if (dev && dev->model_id[0]) {
                bool adapter_sent = false;
                if (strcmp(key, "state") == 0) {
                    adapter_sent = zhac_adapter_send_bool(
                        attr.ieee, dev->model_id, dev->manufacturer_name,
                        dev->nwk_addr, ep, key, attr.val != 0);
                } else {
                    adapter_sent = zhac_adapter_send_uint(
                        attr.ieee, dev->model_id, dev->manufacturer_name,
                        dev->nwk_addr, ep, key,
                        static_cast<uint64_t>(attr.val));
                }
                if (adapter_sent) { ok = true; sent = true; }
            }

            // Legacy zcl_converter IR + tuya_to_rules path retired — ZHC's
            // tz_converters (handled above via zhac_adapter_send_uint) own
            // command encoding now. Fallback here: raw cluster dispatch
            // derived from the attribute key.
            if (dev != nullptr) {
                uint16_t cluster = attr.cluster;
                if (cluster == 0) {
                    if (strcmp(key, "state") == 0)            cluster = 0x0006;
                    else if (strcmp(key, "brightness") == 0)  cluster = 0x0008;
                    else if (strcmp(key, "color_temp") == 0)  cluster = 0x0300;
                    else if (strcmp(key, "hue") == 0 ||
                             strcmp(key, "saturation") == 0)  cluster = 0x0300;
                }
                if (cluster == 0x0006) {
                    uint8_t zcl_cmd = (attr.val != 0) ? 0x01 : 0x00;
                    ok = zigbee_zcl_on_off(dev->nwk_addr, ep, zcl_cmd);
                } else if (cluster == 0x0008) {
                    uint8_t level = (uint8_t)(attr.val & 0xFF);
                    ok = zigbee_zcl_level(dev->nwk_addr, ep, level, 0);
                } else if (cluster == 0x0300) {
                    ok = zigbee_zcl_color_temp(dev->nwk_addr, ep, (uint16_t)(attr.val & 0xFFFF), 0);
                } else {
                    ESP_LOGW(TAG, "SET_ATTR unhandled cluster=0x%04x key=%s ieee=0x%llx",
                             cluster, key, (unsigned long long)attr.ieee);
                }
            } else {
                ESP_LOGW(TAG, "SET_ATTR device not found ieee=0x%llx", (unsigned long long)attr.ieee);
            }
        }
    }

    // Optimistic shadow update. Many devices (especially Tuya LED
    // drivers) don't emit an attribute report after a command-driven
    // state change — only on button/physical triggers. Without this
    // update, the SPA's device view reverts to the last-known shadow
    // value on the next device.get / refresh, making every toggle
    // look like it had no effect. A real attr report from the device
    // will override this optimistic value when it arrives.
    if (ok && attr.key[0] != '\0') {
        const char* k = attr.key;
        uint8_t vt = (strcmp(k, "state") == 0) ? VAL_BOOL : VAL_INT;
        device_shadow_update_optimistic(attr.ieee, k, vt,
                                         static_cast<int32_t>(attr.val));
    }

    if (!sent) {
        if (hap_json_encode_set_ack(tx_buf, sizeof(tx_buf), &len, attr.ieee, ok)) {
            hap_send(HapMsgType::SET_ACK, tx_buf, len, HAP_FLAG_NEEDS_ACK, req.seq);
        }
    }
}

// ── SYNC response ─────────────────────────────────────────────────────────
static void handle_sync(const HapFrame& f) {
    HapSyncInfo info{};
    if (!hap_json_decode_sync(f.payload, f.payload_len, info)) return;
    if (info.is_ack) return;  // P4 doesn't initiate SYNC — ignore ACK

    auto& tx_buf = hap_tx_scratch();
    uint16_t len = 0;
    // Report active (non-tombstoned) count to S3 so the SPA Info page
    // matches the Devices page list (which filters zap_dev_is_removed).
    const uint16_t n_active = pool_count_active();
    if (hap_json_encode_sync_ack(tx_buf, sizeof(tx_buf), &len,
                                  info.session_id, "0.4.0", n_active)) {
        hap_send(HapMsgType::SYNC, tx_buf, len, 0);
        ESP_LOGI(TAG, "SYNC_ACK sent — %d devices", n_active);
    }
}

// ── HAP handler registry ──────────────────────────────────────────────────
// P-F6 (docs/OPTIMIZATIONS.md): plain function pointer instead of
// std::function — `std::function` is 16-32 bytes per slot plus heap if
// the lambda captures, so the table cost ~8 KB BSS for ~25 used
// entries. Every handler today is either a static function or a
// captureless lambda; both convert implicitly to a plain fn pointer.
// 4 B × 256 = 1 KB.
using HapHandler = void(*)(const HapFrame&);
static HapHandler s_hap_handlers[256];

static void handle_heartbeat(const HapFrame& f) {
    HapHeartbeat hb{};
    hap_json_decode_heartbeat(f.payload, f.payload_len, hb);
    // Free-heap snapshot: total free + largest contiguous block in
    // INTERNAL RAM (the pool that fragments first under bursty allocs
    // like rule-driven mqtt_gw_publish). Watch for steady decline →
    // genuine leak, vs flat-but-shrinking-largest → fragmentation.
    const uint32_t free_total = esp_get_free_heap_size();
    const uint32_t free_int   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largest_int =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "HEARTBEAT rx uptime=%" PRIu32
                  " heap_free=%" PRIu32 " int_free=%" PRIu32
                  " int_largest=%" PRIu32,
             hb.uptime, free_total, free_int, largest_int);
}

static void handle_rule_create(const HapFrame& f) {
    char dsl[256] = {};
    char name[24] = {};
    HapRuleExecResult result{};
    if (hap_json_decode_rule_create(f.payload, f.payload_len,
                                     name, sizeof(name),
                                     dsl,  sizeof(dsl))) {
        uint16_t new_id = 0;
        result.ok = simple_rules_add(name, dsl, &new_id);
        result.rule_id = new_id;
        if (!result.ok) {
            const char* detail = dsl_last_error();
            if (detail && detail[0]) {
                strncpy(result.err, detail, sizeof(result.err) - 1);
            } else {
                strncpy(result.err, "parse or store failed",
                        sizeof(result.err) - 1);
            }
        }
    } else {
        result.ok = false;
        strncpy(result.err, "decode failed", sizeof(result.err) - 1);
    }
    send_exec_result(HapMsgType::RULE_EXEC_RESULT, result);
}

static void handle_rule_delete(const HapFrame& f) {
    uint16_t rule_id = 0;
    HapRuleExecResult result{};
    if (hap_json_decode_rule_delete(f.payload, f.payload_len, &rule_id)) {
        result.ok      = simple_rules_delete(rule_id);
        result.rule_id = rule_id;
        if (!result.ok)
            strncpy(result.err, "not found", sizeof(result.err) - 1);
    } else {
        result.ok = false;
        strncpy(result.err, "decode failed", sizeof(result.err) - 1);
    }
    send_exec_result(HapMsgType::RULE_EXEC_RESULT, result);
}

static void handle_rule_update(const HapFrame& f) {
    uint16_t rule_id = 0; bool enabled = false;
    HapRuleExecResult result{};
    if (hap_json_decode_rule_update(f.payload, f.payload_len, &rule_id, &enabled)) {
        result.ok      = simple_rules_enable(rule_id, enabled);
        result.rule_id = rule_id;
        if (!result.ok)
            strncpy(result.err, "not found", sizeof(result.err) - 1);
    } else {
        result.ok = false;
        strncpy(result.err, "decode failed", sizeof(result.err) - 1);
    }
    send_exec_result(HapMsgType::RULE_EXEC_RESULT, result);
}

static void handle_rule_update_dsl(const HapFrame& f) {
    uint16_t rule_id = 0;
    char dsl[256]  = {};
    char name[24]  = {};
    HapRuleExecResult result{};
    if (hap_json_decode_rule_update_dsl(f.payload, f.payload_len, &rule_id,
                                          name, sizeof(name),
                                          dsl,  sizeof(dsl))) {
        result.ok      = simple_rules_update(rule_id, name, dsl);
        result.rule_id = rule_id;
        if (!result.ok) {
            const char* detail = dsl_last_error();
            if (detail && detail[0]) {
                strncpy(result.err, detail, sizeof(result.err) - 1);
            } else {
                strncpy(result.err, "not found or parse failed",
                        sizeof(result.err) - 1);
            }
        }
    } else {
        result.ok = false;
        strncpy(result.err, "decode failed", sizeof(result.err) - 1);
    }
    send_exec_result(HapMsgType::RULE_EXEC_RESULT, result);
}

static void handle_device_set_name(const HapFrame& f) {
    uint64_t ieee = 0;
    char new_name[30] = {};
    auto& tx_buf = hap_tx_scratch();
    uint16_t tx_len = 0;
    bool encode_ok = false;

    if (hap_json_decode_device_set_name(f.payload, f.payload_len,
                                         &ieee, new_name, sizeof(new_name))) {
        ZapDevice* dev = pool_find_by_ieee(ieee);
        if (dev) {
            strncpy(dev->friendly_name, new_name, sizeof(dev->friendly_name) - 1);
            dev->friendly_name[sizeof(dev->friendly_name) - 1] = '\0';
            zap_store_mark_dirty(dev, ZAP_PERSIST_HIGH);
            simple_rules_reload(); // re-resolve friendly names
            // Legacy zcl_converter augmentation retired — the base encoder
            // already carries what the UI needs after a rename.
            encode_ok = hap_json_encode_device_info(tx_buf, sizeof(tx_buf),
                                                     &tx_len, dev,
                                                     &resolve_dev_labels);
        } else {
            encode_ok = hap_json_encode_device_info_err(tx_buf, sizeof(tx_buf),
                                                         &tx_len, "not found");
        }
    } else {
        encode_ok = hap_json_encode_device_info_err(tx_buf, sizeof(tx_buf),
                                                     &tx_len, "decode failed");
    }

    if (encode_ok) {
        hap_send(HapMsgType::DEVICE_INFO, tx_buf, tx_len, HAP_FLAG_NEEDS_ACK, f.seq);
    }
}

static void handle_time_sync(const HapFrame& f) {
    uint32_t ts = 0;
    if (hap_json_decode_time_sync(f.payload, f.payload_len, &ts)) {
        struct timeval tv{ .tv_sec = (time_t)ts, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        ESP_LOGI(TAG, "System time set: ts=%lu", (unsigned long)ts);
    } else {
        ESP_LOGW(TAG, "TIME_SYNC decode failed");
    }
}

static void handle_mqtt_msg_in(const HapFrame& f) {
    HapMqttMsgIn msg{};
    if (hap_json_decode_mqtt_msg_in(f.payload, f.payload_len, msg)) {
        Event ev{};
        ev.type = EventType::MQTT_MSG;
        auto& me = *reinterpret_cast<MqttMsgEvent*>(ev.data);
        // Length-clamped copy avoids both -Wstringop-truncation
        // (strncpy) and -Wformat-truncation (snprintf): the source may
        // legally exceed the destination on this MQTT inbound path,
        // and we just truncate.
        {
            const size_t cap = sizeof(me.topic) - 1;
            const size_t n   = strnlen(msg.topic, cap);
            memcpy(me.topic, msg.topic, n);
            me.topic[n] = '\0';
        }
        {
            const size_t cap = sizeof(me.payload) - 1;
            const size_t n   = strnlen(msg.payload, cap);
            memcpy(me.payload, msg.payload, n);
            me.payload[n] = '\0';
        }
        event_bus_publish(ev);
    } else {
        ESP_LOGW(TAG, "MQTT_MSG_IN decode failed");
    }
}

static void handle_rule_list_req(const HapFrame& f) {
    hap_dispatch_assert_single_task();  // F-08
    static RuleSlot        raw[64];
    static HapRuleSlotInfo slots[64];
    uint16_t count = simple_rules_list(raw, 64);
    for (uint16_t i = 0; i < count; i++) {
        slots[i].rule_id   = raw[i].rule_id;
        slots[i].rule_type = raw[i].rule_type;
        slots[i].enabled   = raw[i].enabled != 0;
        strncpy(slots[i].name, raw[i].name, sizeof(slots[i].name) - 1);
        slots[i].name[sizeof(slots[i].name) - 1] = '\0';
        strncpy(slots[i].src, reinterpret_cast<const char*>(raw[i].src),
                sizeof(slots[i].src) - 1);
        slots[i].src[sizeof(slots[i].src) - 1] = '\0';
    }
    auto& tx_buf = hap_tx_scratch();
    uint16_t tx_len = 0;
    if (hap_json_encode_rule_list_rsp(tx_buf, sizeof(tx_buf), &tx_len, slots, count)) {
        hap_send(HapMsgType::RULE_LIST_RSP, tx_buf, tx_len, HAP_FLAG_NO_ACK, f.seq);
    }
}

static void handle_script_write(const HapFrame& f) {
    hap_dispatch_assert_single_task();  // F-08
    char name_raw[HAP_SCRIPT_NAME_MAX + 8] = {0};
    static char src[HAP_SCRIPT_MAX_SRC + 1];
    HapRuleExecResult result{};
    if (!hap_json_decode_script_write(f.payload, f.payload_len,
                                       name_raw, sizeof(name_raw),
                                       src, sizeof(src))) {
        result.ok = false;
        strncpy(result.err, "decode failed", sizeof(result.err) - 1);
        send_exec_result(HapMsgType::SCRIPT_ACK, result);
        return;
    }
    if (lua_script_cache_write(name_raw, src)) {
        result.ok = true;
        ESP_LOGI(TAG, "SCRIPT_WRITE staged name=%s (%u bytes)",
                 name_raw, (unsigned)strlen(src));
    } else {
        result.ok = false;
        strncpy(result.err, "invalid name or stage failed",
                sizeof(result.err) - 1);
    }
    send_exec_result(HapMsgType::SCRIPT_ACK, result);
}

static void handle_script_delete(const HapFrame& f) {
    char name_raw[HAP_SCRIPT_NAME_MAX + 8] = {0};
    HapRuleExecResult result{};
    if (!hap_json_decode_script_delete(f.payload, f.payload_len,
                                        name_raw, sizeof(name_raw))) {
        result.ok = false;
        strncpy(result.err, "decode failed", sizeof(result.err) - 1);
        send_exec_result(HapMsgType::SCRIPT_ACK, result);
        return;
    }
    lua_script_cache_delete(name_raw);
    result.ok = true;
    ESP_LOGI(TAG, "SCRIPT_DELETE name=%s", name_raw);
    send_exec_result(HapMsgType::SCRIPT_ACK, result);
}

// SCRIPT_RUN_REQ — fire the named script as a fresh Lua coroutine.
static void handle_script_run_req(const HapFrame& f) {
    char name_raw[HAP_SCRIPT_NAME_MAX + 8] = {0};
    HapRuleExecResult result{};
    if (!hap_json_decode_script_run_req(f.payload, f.payload_len,
                                         name_raw, sizeof(name_raw))) {
        result.ok = false;
        strncpy(result.err, "decode failed", sizeof(result.err) - 1);
        send_exec_result(HapMsgType::SCRIPT_ACK, result);
        return;
    }
    if (!lua_engine_run_script(name_raw)) {
        result.ok = false;
        strncpy(result.err, "not found or queue full", sizeof(result.err) - 1);
        send_exec_result(HapMsgType::SCRIPT_ACK, result);
        return;
    }
    result.ok = true;
    ESP_LOGI(TAG, "SCRIPT_RUN name=%s", name_raw);
    send_exec_result(HapMsgType::SCRIPT_ACK, result);
}

// SCRIPT_CHECK_REQ — syntax-check a source buffer without executing.
// Feeds the SPA's inline linter; the answer is a SCRIPT_CHECK_RSP
// with {ok, err, line}.
static void handle_script_check_req(const HapFrame& f) {
    hap_dispatch_assert_single_task();  // F-08
    static char name_raw[HAP_SCRIPT_NAME_MAX + 8];
    static char src_buf[HAP_SCRIPT_MAX_SRC + 1];
    name_raw[0] = '\0';
    src_buf[0]  = '\0';
    auto& tx_buf = hap_tx_scratch();
    uint16_t tx_len = 0;

    if (!hap_json_decode_script_check_req(f.payload, f.payload_len,
                                           name_raw, sizeof(name_raw),
                                           src_buf,  sizeof(src_buf))) {
        if (hap_json_encode_script_check_rsp(tx_buf, sizeof(tx_buf), &tx_len,
                                              false, "decode failed", 1)) {
            hap_send(HapMsgType::SCRIPT_CHECK_RSP, tx_buf, tx_len,
                      HAP_FLAG_NO_ACK, f.seq);
        }
        return;
    }

    char err[160] = {0};
    int  line     = 1;
    const bool ok = lua_engine_check_syntax(name_raw, src_buf,
                                             err, sizeof(err), &line);
    if (hap_json_encode_script_check_rsp(tx_buf, sizeof(tx_buf), &tx_len,
                                          ok, ok ? "" : err, ok ? 0 : line)) {
        hap_send(HapMsgType::SCRIPT_CHECK_RSP, tx_buf, tx_len,
                  HAP_FLAG_NO_ACK, f.seq);
    }
}

static void handle_script_list_req(const HapFrame& f) {
    hap_dispatch_assert_single_task();  // F-08
    static HapScriptInfo scripts[LUA_SCRIPT_MAX];
    LuaScriptEntry lua_entries[LUA_SCRIPT_MAX];
    const uint16_t lua_count = lua_script_cache_list(
        lua_entries, LUA_SCRIPT_MAX);
    uint16_t count = 0;
    for (uint16_t i = 0; i < lua_count; ++i) {
        const size_t cap = HAP_SCRIPT_NAME_MAX;
        const size_t n   = strnlen(lua_entries[i].name, cap);
        memcpy(scripts[count].name, lua_entries[i].name, n);
        scripts[count].name[n] = '\0';
        scripts[count].size = lua_entries[i].size;
        count++;
    }
    auto& tx_buf = hap_tx_scratch();
    uint16_t tx_len = 0;
    bool encoded = hap_json_encode_script_list_rsp(
        tx_buf, sizeof(tx_buf), &tx_len, scripts, count);
    if (!encoded) {
        // Payload would exceed HAP_MAX_PAYLOAD. Send an empty list rather
        // than nothing so the S3 caller gets a deterministic answer instead
        // of timing out and starting the retry burst.
        ESP_LOGE("hap_dispatch",
                 "SCRIPT_LIST_RSP encode overflow count=%u — sending empty list",
                 count);
        encoded = hap_json_encode_script_list_rsp(
            tx_buf, sizeof(tx_buf), &tx_len, scripts, 0);
        if (!encoded) return;
    }
    hap_send(HapMsgType::SCRIPT_LIST_RSP, tx_buf, tx_len, HAP_FLAG_NO_ACK, f.seq);
}

static void handle_script_read_req(const HapFrame& f) {
    hap_dispatch_assert_single_task();  // F-08
    char name_raw[HAP_SCRIPT_NAME_MAX + 8] = {0};
    HapRuleExecResult err_result{};
    if (!hap_json_decode_script_read_req(f.payload, f.payload_len,
                                          name_raw, sizeof(name_raw))) {
        err_result.ok = false;
        strncpy(err_result.err, "decode failed", sizeof(err_result.err) - 1);
        send_exec_result(HapMsgType::SCRIPT_ACK, err_result);
        return;
    }
    static char src_buf[HAP_SCRIPT_MAX_SRC + 1];
    const int n = lua_script_cache_read(name_raw, src_buf, sizeof(src_buf));
    if (n < 0) {
        err_result.ok = false;
        strncpy(err_result.err, "not found", sizeof(err_result.err) - 1);
        send_exec_result(HapMsgType::SCRIPT_ACK, err_result);
        return;
    }
    auto& tx_buf = hap_tx_scratch();
    uint16_t tx_len = 0;
    if (hap_json_encode_script_read_rsp(tx_buf, sizeof(tx_buf), &tx_len,
                                         name_raw, src_buf)) {
        hap_send(HapMsgType::SCRIPT_READ_RSP, tx_buf, tx_len, HAP_FLAG_NO_ACK, f.seq);
    }
}

static void handle_permit_join(const HapFrame& f) {
    uint8_t dur = 0;
    if (hap_json_decode_permit_join(f.payload, f.payload_len, &dur))
        zigbee_permit_join(dur);
    else
        ESP_LOGW(TAG, "PERMIT_JOIN decode failed");
}

static void handle_bind_req(const HapFrame& f) {
    HapBindReq req{};
    auto& tx_buf = hap_tx_scratch();
    uint16_t tx_len = 0;
    bool ok = false;
    if (hap_json_decode_bind_req(f.payload, f.payload_len, req)) {
        ZapDevice* src_dev = pool_find_by_ieee(req.src_ieee);
        uint16_t src_nwk = src_dev ? src_dev->nwk_addr : 0xFFFE;
        if (req.unbind)
            ok = zigbee_zdo_unbind(src_nwk, req.src_ieee, req.src_ep,
                                   req.cluster, req.dst_ieee, req.dst_ep);
        else
            ok = zigbee_zdo_bind(src_nwk, req.src_ieee, req.src_ep,
                                 req.cluster, req.dst_ieee, req.dst_ep);
    }
    if (hap_json_encode_bind_ack(tx_buf, sizeof(tx_buf), &tx_len, ok)) {
        hap_send(HapMsgType::BIND_ACK, tx_buf, tx_len, HAP_FLAG_NO_ACK, f.seq);
    }
}

static void handle_device_delete(const HapFrame& f) {
    uint64_t ieee = 0;
    bool hard = false;
    auto& tx_buf = hap_tx_scratch();
    uint16_t tx_len = 0;
    bool ok = false;
    if (hap_json_decode_device_delete(f.payload, f.payload_len, &ieee, &hard)) {
        ESP_LOGI(TAG, "DEVICE_DELETE ieee=0x%016llX hard=%d",
                 (unsigned long long)ieee, (int)hard);
        ZapDevice* dev = pool_find_by_ieee(ieee);
        if (dev) {
            zigbee_leave_req(dev->nwk_addr, ieee);
            if (hard) {
                // "Forget forever" — drop NVS record, pool slot, and
                // every per-ieee cache so the next rejoin runs a full
                // interview against the current device-definition
                // library (no fast-path, no stale shadow attrs, no
                // stale adapter def pointer).
                zap_store_delete_device(ieee);
                device_shadow_clear_attrs(ieee);
                zhac_adapter_invalidate_def_cache(ieee);
                zhac_adapter_fallback_clear(ieee);
                ok = zigbee_pool_remove(ieee);
                ESP_LOGI(TAG, "DEVICE_DELETE hard sweep done ieee=0x%016llX "
                               "pool_removed=%d", (unsigned long long)ieee, (int)ok);
            } else {
                // Soft-remove: keep the slot + NVS record so a rejoin
                // restores configure state without rediscovery. Hide
                // from the UI until then (see hap_json device-list
                // filter).
                zap_dev_mark_removed(dev);
                zap_store_mark_dirty(dev, ZAP_PERSIST_LOW);
                ok = true;
            }
        } else if (hard) {
            // Pool entry already gone (orphaned NVS record or stale
            // shadow / adapter cache). Still do the full sweep so the
            // UI's "hard" option is guaranteed idempotent.
            zap_store_delete_device(ieee);
            device_shadow_clear_attrs(ieee);
            zhac_adapter_invalidate_def_cache(ieee);
            ok = true;
        }
    }
    if (hap_json_encode_device_delete_ack(tx_buf, sizeof(tx_buf), &tx_len, ok)) {
        hap_send(HapMsgType::DEVICE_DELETE_ACK, tx_buf, tx_len, HAP_FLAG_NO_ACK, f.seq);
    }
}

static void handle_interview_req(const HapFrame& f) {
    uint64_t ieee = 0;
    if (hap_json_decode_interview_req(f.payload, f.payload_len, &ieee))
        zigbee_interview_trigger(ieee);
    else
        ESP_LOGW(TAG, "INTERVIEW_REQ decode failed");
}

// CONFIGURE_REQ — re-fire `zhac_adapter_configure` for one device WITHOUT
// redoing the full ZNP interview. Used when a firmware update adds new
// wiring (read-on-join attrs, new reports, new config_steps) to a def
// that some paired devices already match — the SPA "Configure" button
// posts this, P4 walks the pipeline against the cached (model, manuf)
// from the pool. Payload shape is identical to INTERVIEW_REQ so the
// same JSON decoder works for both.
//
// Releases the pool lock before invoking `zhac_adapter_configure` —
// the call issues radio frames that can block on AF_DATA_CONFIRM for
// ~2.5 s; holding the pool mutex would stall every other device
// dispatching attrs simultaneously.
static void handle_configure_req(const HapFrame& f) {
    uint64_t ieee = 0;
    if (!hap_json_decode_interview_req(f.payload, f.payload_len, &ieee)) {
        ESP_LOGW(TAG, "CONFIGURE_REQ decode failed");
        return;
    }
    zigbee_pool_lock();
    const ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev || dev->model_id[0] == '\0') {
        zigbee_pool_unlock();
        ESP_LOGW(TAG, "CONFIGURE_REQ ieee=0x%016llx unknown or never interviewed",
                 (unsigned long long)ieee);
        return;
    }
    const uint64_t ieee_cp = dev->ieee_addr;
    const uint16_t nwk_cp  = dev->nwk_addr;
    // Buffers sized to comfortably exceed ZapDevice::model_id /
    // manufacturer_name (~34 B each) — anything smaller trips
    // -Werror=format-truncation.
    char model_cp[64], manu_cp[64];
    snprintf(model_cp, sizeof(model_cp), "%s", dev->model_id);
    snprintf(manu_cp,  sizeof(manu_cp),  "%s", dev->manufacturer_name);
    zigbee_pool_unlock();

    const bool ok = zhac_adapter_configure(ieee_cp, nwk_cp, model_cp, manu_cp);
    ESP_LOGI(TAG, "CONFIGURE_REQ ieee=0x%016llx model=%s -> %s",
             (unsigned long long)ieee_cp, model_cp, ok ? "ok" : "fail");
}

static void handle_device_options_set(const HapFrame& f) {
    uint64_t ieee = 0;
    int32_t occupancy_timeout_s = -1;  // -1 = absent
    int32_t debounce_ms         = -1;
    int32_t throttle_ms         = -1;
    bool ok = true;
    bool any = false;
    if (hap_json_decode_device_options_set(f.payload, f.payload_len,
                                           &ieee, &occupancy_timeout_s, &debounce_ms,
                                           &throttle_ms)) {
        if (occupancy_timeout_s >= 0) {
            uint16_t v = (occupancy_timeout_s > 3600) ? 3600
                                                      : (uint16_t)occupancy_timeout_s;
            ok = ok && device_shadow_set_occupancy_timeout(ieee, v);
            any = true;
        }
        if (debounce_ms >= 0) {
            uint32_t v = (debounce_ms > 60000) ? 60000 : (uint32_t)debounce_ms;
            ok = ok && device_shadow_set_debounce_ms(ieee, v);
            any = true;
        }
        if (throttle_ms >= 0) {
            uint32_t v = (throttle_ms > 600000) ? 600000 : (uint32_t)throttle_ms;
            ok = ok && device_shadow_set_throttle_ms(ieee, v);
            any = true;
        }
        ESP_LOGI(TAG, "DEVICE_OPTIONS_SET ieee=0x%016llx occ=%ld debounce=%ldms throttle=%ldms -> %s",
                 (unsigned long long)ieee,
                 (long)occupancy_timeout_s, (long)debounce_ms, (long)throttle_ms,
                 (any && ok) ? "ok" : (any ? "fail" : "no-op"));
        if (!any) ok = false;  // nothing forwarded → NAK so caller retries
    } else {
        ESP_LOGW(TAG, "DEVICE_OPTIONS_SET decode failed");
        ok = false;
    }
    auto& tx_buf = hap_tx_scratch();
    uint16_t tx_len = 0;
    if (hap_json_encode_device_options_ack(tx_buf, sizeof(tx_buf), &tx_len, ok)) {
        hap_send(HapMsgType::DEVICE_OPTIONS_SET_ACK, tx_buf, tx_len, HAP_FLAG_NO_ACK, f.seq);
    }
}

// ── ZIGBEE_CFG_SET ──────────────────────────────────────────────────
// Persists operator-chosen channel / network-key to NVS. Changes take
// effect on the next factory-reset + reboot cycle (they seed the
// commissioning flow). The ACK reports the current stored values so
// the UI can refresh without a separate GET.
static void handle_zigbee_cfg_set(const HapFrame& f) {
    int8_t   new_chan    = -1;
    uint8_t  new_key[16] = {};
    bool     key_present = false;
    bool     regenerate  = false;
    bool     ok          = false;

    if (hap_json_decode_zigbee_cfg_set(f.payload, f.payload_len,
                                        &new_chan,
                                        new_key, sizeof(new_key),
                                        &key_present, &regenerate)) {
        nvs_handle_t h;
        if (nvs_open("zigbee_cfg", NVS_READWRITE, &h) == ESP_OK) {
            if (new_chan >= 11 && new_chan <= 26) {
                nvs_set_u8(h, "channel", (uint8_t)new_chan);
            }
            if (regenerate) {
                esp_fill_random(new_key, sizeof(new_key));
                key_present = true;
                ESP_LOGI(TAG, "ZIGBEE_CFG_SET: regenerated random network key");
            }
            if (key_present) {
                nvs_set_blob(h, "net_key", new_key, sizeof(new_key));
            }
            nvs_commit(h);
            nvs_close(h);
            ok = true;
            ESP_LOGI(TAG, "ZIGBEE_CFG_SET: channel=%d net_key=%s "
                          "(applies on next factory reset)",
                     new_chan, key_present ? "updated" : "unchanged");
        } else {
            ESP_LOGW(TAG, "ZIGBEE_CFG_SET: nvs_open failed");
        }
    }

    // Read back current stored state for the ACK.
    uint8_t cur_chan = 11;
    bool    cur_set  = false;
    {
        nvs_handle_t h;
        if (nvs_open("zigbee_cfg", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "channel", &cur_chan);
            uint8_t dummy[16];
            size_t l = sizeof(dummy);
            cur_set = (nvs_get_blob(h, "net_key", dummy, &l) == ESP_OK && l == 16);
            nvs_close(h);
        }
    }

    auto& tx_buf = hap_tx_scratch();
    uint16_t tx_len = 0;
    if (hap_json_encode_zigbee_cfg_ack(tx_buf, sizeof(tx_buf), &tx_len,
                                        ok, cur_chan, cur_set)) {
        hap_send(HapMsgType::ZIGBEE_CFG_SET_ACK, tx_buf, tx_len, HAP_FLAG_NO_ACK, f.seq);
    }
}

// P-F2 (docs/OPTIMIZATIONS.md): table-driven handler registration.
// Each row is (HapMsgType, fn pointer). Adding a handler = 1 row + 1
// function. Replaces 27 individual `s_hap_handlers[…] = handle_…`
// assignments + the inline-lambda block.
//
// Forward-declare the global helper before opening the anonymous
// namespace so the extracted handler resolves to it rather than an
// inner-namespace ghost. (Defined as plain C++ in
// firmware/zhac-main-core/main/main.cpp.)
void zigbee_factory_reset();

namespace {

void handle_zigbee_factory_reset_msg(const HapFrame&) {
    ESP_LOGW("p4_main", "ZIGBEE_FACTORY_RESET received from S3");
    zigbee_factory_reset();      // global; does not return
}

void handle_diag_unhandled_req(const HapFrame& f);
void handle_metrics_req(const HapFrame& f);

struct HapRow { HapMsgType type; HapHandler fn; };
constexpr HapRow kHapHandlers[] = {
    { HapMsgType::OTA_CHUNK,            handle_ota_chunk             },
    { HapMsgType::OTA_CHECKPOINT_REQ,   handle_ota_checkpoint_req    },
    { HapMsgType::GET_DEVICES,          handle_get_devices           },
    { HapMsgType::GET_DEVICE_BY_ID,     handle_get_device_by_id      },
    { HapMsgType::SET_ATTRIBUTE,        handle_set_attribute         },
    { HapMsgType::HEARTBEAT,            handle_heartbeat             },
    { HapMsgType::RULE_CREATE,          handle_rule_create           },
    { HapMsgType::RULE_DELETE,          handle_rule_delete           },
    { HapMsgType::RULE_UPDATE,          handle_rule_update           },
    { HapMsgType::RULE_UPDATE_DSL,      handle_rule_update_dsl       },
    { HapMsgType::DEVICE_SET_NAME,      handle_device_set_name       },
    { HapMsgType::TIME_SYNC,            handle_time_sync             },
    { HapMsgType::MQTT_MSG_IN,          handle_mqtt_msg_in           },
    { HapMsgType::RULE_LIST_REQ,        handle_rule_list_req         },
    { HapMsgType::SCRIPT_WRITE,         handle_script_write          },
    { HapMsgType::SCRIPT_DELETE,        handle_script_delete         },
    { HapMsgType::SCRIPT_RUN_REQ,       handle_script_run_req        },
    { HapMsgType::SCRIPT_CHECK_REQ,     handle_script_check_req      },
    { HapMsgType::SCRIPT_LIST_REQ,      handle_script_list_req       },
    { HapMsgType::SCRIPT_READ_REQ,      handle_script_read_req       },
    { HapMsgType::PERMIT_JOIN,          handle_permit_join           },
    { HapMsgType::BIND_REQ,             handle_bind_req              },
    { HapMsgType::DEVICE_DELETE,        handle_device_delete         },
    { HapMsgType::INTERVIEW_REQ,        handle_interview_req         },
    { HapMsgType::CONFIGURE_REQ,        handle_configure_req         },
    { HapMsgType::DEVICE_OPTIONS_SET,   handle_device_options_set    },
    { HapMsgType::ZIGBEE_CFG_SET,       handle_zigbee_cfg_set        },
    { HapMsgType::ZIGBEE_FACTORY_RESET, handle_zigbee_factory_reset_msg },
    { HapMsgType::DIAG_UNHANDLED_REQ,   handle_diag_unhandled_req    },
    { HapMsgType::METRICS_REQ,          handle_metrics_req           },
};

void handle_diag_unhandled_req(const HapFrame& f) {
        // Snapshot the diagnostics ring, encode as JSON, respond.
        static ZbUnhandledFrame snap[32];
        uint16_t n = zb_diag_snapshot(snap, 32);
        auto& tx_buf = hap_tx_scratch();
        // `last_seen_s` in the ring is *uptime-seconds* since P4
        // boot, not a Unix epoch — fmtSince on the SPA side turned
        // "20 s ago" into "493579 h ago". Emit the age relative to
        // now instead; SPA renders directly without needing wall
        // clock alignment.
        const uint32_t now_up_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        int pos = snprintf(reinterpret_cast<char*>(tx_buf), sizeof(tx_buf),
                           "{\"entries\":[");
        for (uint16_t i = 0; i < n; i++) {
            const auto& e = snap[i];
            const uint32_t age_s = (e.last_seen_s > 0 && now_up_s >= e.last_seen_s)
                                     ? now_up_s - e.last_seen_s : 0;
            int w = snprintf(reinterpret_cast<char*>(tx_buf) + pos,
                             sizeof(tx_buf) - pos,
                             "%s{\"cluster\":%u,\"id\":%u,\"cs\":%u,"
                             "\"count\":%" PRIu32 ",\"age_s\":%" PRIu32
                             ",\"ieee\":\"0x%016llx\"}",
                             i == 0 ? "" : ",",
                             e.cluster_id, e.attr_or_cmd_id, e.cluster_specific,
                             e.count, age_s,
                             (unsigned long long)e.last_ieee);
            if (w <= 0 || (size_t)w >= sizeof(tx_buf) - pos) break;
            pos += w;
        }
        int w = snprintf(reinterpret_cast<char*>(tx_buf) + pos,
                         sizeof(tx_buf) - pos, "]}");
        if (w > 0 && (size_t)w < sizeof(tx_buf) - pos) pos += w;
        hap_send(HapMsgType::DIAG_UNHANDLED_RSP, tx_buf, static_cast<uint16_t>(pos), HAP_FLAG_NO_ACK, f.seq);
}

void handle_metrics_req(const HapFrame& f) {
    // Freshen heap-value samples so the scrape reflects now-ish.
    metrics::update_memory_snapshot();

    auto& tx_buf = hap_tx_scratch();
    const size_t n = metrics::prometheus_format(
        reinterpret_cast<char*>(tx_buf), sizeof(tx_buf), "zhac_p4");

    HapFrame rsp = hap_make_reply(f, HapMsgType::METRICS_RSP,
                                     HAP_FLAG_NO_ACK);
    rsp.seq         = hap_session_next_seq();
    rsp.payload     = tx_buf;
    rsp.payload_len = static_cast<uint16_t>(n);
    hap_session_send(rsp);
}

}  // namespace (P-F2 handler table + extracted lambdas)

static void send_heartbeat() {
    uint8_t hb_buf[384];
    uint16_t len = 0;
    HapHeartbeat hbi{};
    hbi.uptime      = (uint32_t)(esp_timer_get_time() / 1000000UL);
    populate_mem_metrics(hbi);
    sys_metrics_sample_cpu_pct(hbi.cpu_pct_c0, hbi.cpu_pct_c1);
    hbi.proto_mask  = 0;
    for (uint8_t i = 0; i < device_backend_count(); i++) {
        DeviceBackend* b = device_backend_get(i);
        if (b && b->is_running && b->is_running())
            hbi.proto_mask |= (1u << b->protocol);
    }
    // pool_count_active filters out soft-removed slots so the SPA Info
    // page agrees with the Devices page list. Raw pool_count() includes
    // tombstones kept for re-pair fast-path.
    hbi.device_count = pool_count_active();
    if (hap_json_encode_heartbeat(hb_buf, sizeof(hb_buf), &len, hbi)) {
        hap_send(HapMsgType::HEARTBEAT, hb_buf, len, HAP_FLAG_NO_ACK);
    }
}

void task_hap(void*) {
    ESP_LOGI(TAG, "TaskHAP started");

    // Create the async-tx queue before subscribing to event_bus so the
    // first ZCL_ATTR publish has somewhere to land.
    s_hap_tx_q = xQueueCreate(HAP_TX_QUEUE_DEPTH, sizeof(HapTxItem));
    configASSERT(s_hap_tx_q);

    // P-F2: register handlers from the table.
    for (const auto& row : kHapHandlers) {
        s_hap_handlers[static_cast<uint8_t>(row.type)] = row.fn;
    }

    hap_session_init(HapSessionCfg{
        .send         = hap_slave_send,
        .on_frame     = [](const HapFrame& f) {
            uint8_t idx = static_cast<uint8_t>(f.type);
            if (s_hap_handlers[idx])
                s_hap_handlers[idx](f);
            else
                ESP_LOGW(TAG, "unhandled HAP type=0x%02x", idx);
        },
        .on_sync      = handle_sync,
        .on_link_dead = []() { ESP_LOGE(TAG, "HAP link dead — awaiting SYNC"); },
    });

    hap_slave_set_callback([](const HapFrame& f) {
        hap_session_on_receive(f);
    });

    event_bus_subscribe(EventType::DEVICE_JOIN,  on_device_event_for_hap);
    event_bus_subscribe(EventType::DEVICE_LEAVE, on_device_leave_for_hap);
    event_bus_subscribe(EventType::ZCL_ATTR,     on_zcl_attr_for_hap);

    // Send initial heartbeat to wake S3
    send_heartbeat();

    TickType_t last_hb   = xTaskGetTickCount();

    while (true) {
        // Tick session every 10 ms for retransmit checks
        hap_session_tick();

        // Drain async-tx queue (sync-SPI fix, 2026-04-27). Block up to
        // 10 ms — wakes immediately on enqueue, otherwise serves as the
        // retransmit-tick pacing. After the first item processes we
        // poll non-blocking to drain a burst in one tick before going
        // back to hap_session_tick().
        HapTxItem rx;
        if (xQueueReceive(s_hap_tx_q, &rx, pdMS_TO_TICKS(10)) == pdTRUE) {
            emit_attr_update(rx.attr);
            while (xQueueReceive(s_hap_tx_q, &rx, 0) == pdTRUE) {
                emit_attr_update(rx.attr);
            }
        }

        // Send heartbeat every 5 s
        if (xTaskGetTickCount() - last_hb >= pdMS_TO_TICKS(5000)) {
            last_hb = xTaskGetTickCount();
            send_heartbeat();
        }

        // No vTaskDelay — xQueueReceive above handles the 10 ms tick
        // (blocks for that long when the queue is empty).
    }
}
