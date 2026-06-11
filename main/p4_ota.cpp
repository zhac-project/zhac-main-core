// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// p4_ota.cpp — OTA update handler for P4 core firmware
// Receives binary chunks from S3 over HAP, then flashes.
// F3 (FINDINGS.md): image integrity (structure + the build-appended
// SHA-256) is verified by esp_ota_end() before the partition is made
// bootable, plus an app-descriptor sanity check. AUTHENTICITY (tamper
// resistance) requires Secure Boot v2 signed images — see F2 /
// sdkconfig.prod.defaults.
#include "p4_ota.h"
#include "hap_dispatch.h"
#include "hap_json.h"
#include "hap_session.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

// T20: persistence flush barriers fired before esp_restart() so an OTA
// reboot can't discard freshly-edited rules / device state that the
// writeback caches still hold in RAM. esp_restart() does invoke
// registered shutdown handlers, but we call these explicitly so the
// ordering is obvious at the OTA call site and survives any future
// change to handler registration. Both headers declare C++-linkage
// symbols (no extern "C"), so include them rather than re-declaring.
#include "rule_store.h"
#include "zap_store.h"

static const char* TAG = "p4_ota";

// ── OTA state (file-scope so checkpoint query can read it) ────────────────
static esp_ota_handle_t        s_ota_handle          = 0;
static const esp_partition_t*  s_ota_part            = nullptr;
static uint32_t                s_ota_expected_offset = 0;
static uint32_t                s_ota_total           = 0;
// Suppresses duplicate OTA_STATUS frames during a failure cascade.
// Without this, every mismatched chunk after the first error re-fires
// send_ota_status(false) — flooding the HAP wire with ~60 frames in a
// 1-2s window while S3 still has chunks in flight. Reset on chunk-0
// begin so a fresh OTA retry can report fresh failures.
static bool                    s_ota_failure_notified = false;

// ── Idle-abort watchdog (T20) ─────────────────────────────────────────
// If S3 starts an OTA and then stops sending (link drop, S3 reboot, user
// cancel) the open esp_ota_handle pins flash + the staging partition
// forever — the next OTA attempt can't begin cleanly and the handle
// leaks. A one-shot esp_timer started on begin (and re-armed on every
// chunk) aborts a session that sees no chunk for OTA_IDLE_TIMEOUT_US.
// Forward decl — send_ota_status is defined below but the idle callback
// (and the chunk handler) reference it first.
static void send_ota_status(bool ok, uint32_t rcvd, uint32_t total, const char* err_msg);
static constexpr int64_t OTA_IDLE_TIMEOUT_US = 60LL * 1000 * 1000;  // 60 s
static int64_t                 s_ota_last_chunk_us = 0;
static esp_timer_handle_t      s_ota_idle_timer    = nullptr;

static void ota_idle_abort_cb(void*) {
    if (s_ota_handle == 0) return;   // already done/aborted — nothing to do
    const int64_t idle = esp_timer_get_time() - s_ota_last_chunk_us;
    if (idle < OTA_IDLE_TIMEOUT_US) {
        // A chunk arrived after the timer was armed but before it fired
        // (the re-arm path replaces this, so this is a belt-and-braces
        // guard). Re-check on the remaining interval.
        esp_timer_start_once(s_ota_idle_timer, OTA_IDLE_TIMEOUT_US - idle);
        return;
    }
    ESP_LOGW(TAG, "OTA idle >%llds with open session — aborting + freeing handle",
             OTA_IDLE_TIMEOUT_US / 1000000);
    esp_ota_abort(s_ota_handle);
    s_ota_handle = 0;
    s_ota_part = nullptr;
    s_ota_expected_offset = 0;
    send_ota_status(false, s_ota_expected_offset, s_ota_total, "idle timeout");
}

static void ota_idle_timer_rearm() {
    s_ota_last_chunk_us = esp_timer_get_time();
    if (!s_ota_idle_timer) {
        const esp_timer_create_args_t args = {
            .callback              = ota_idle_abort_cb,
            .arg                   = nullptr,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "ota_idle",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_ota_idle_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_ota_idle_timer);   // no-op if not running
    esp_timer_start_once(s_ota_idle_timer, OTA_IDLE_TIMEOUT_US);
}

static void ota_idle_timer_disarm() {
    if (s_ota_idle_timer) esp_timer_stop(s_ota_idle_timer);
}

// ── OTA helpers ───────────────────────────────────────────────────────────
static void send_ota_status(bool ok, uint32_t rcvd, uint32_t total, const char* err_msg) {
    // Drop duplicate failure notifications. Only the FIRST failure in a
    // cascade carries useful diagnostic info — subsequent ones are
    // collateral from chunks still in flight while S3 hasn't yet
    // noticed the abort. A successful terminal frame always goes
    // through and clears the flag.
    if (!ok) {
        if (s_ota_failure_notified) return;
        s_ota_failure_notified = true;
    } else {
        s_ota_failure_notified = false;
    }
    HapOtaStatus s{};
    s.ok    = ok;
    s.rcvd  = rcvd;
    s.total = total;
    strncpy(s.err, err_msg, sizeof(s.err) - 1);
    uint8_t buf[128];
    uint16_t len = 0;
    if (!hap_json_encode_ota_status(buf, sizeof(buf), &len, s)) return;
    hap_send(HapMsgType::OTA_STATUS, buf, len);
    // Also log to P4 console — useful when S3 is also dumping
    // hap_bridge: P4 OTA_STATUS lines, so the err string is visible
    // even before the SPA progress UI surfaces it.
    if (!ok) {
        ESP_LOGE(TAG, "OTA_STATUS sent: ok=0 rcvd=%" PRIu32 "/%" PRIu32 " err='%s'",
                 rcvd, total, err_msg ? err_msg : "");
    }
}

// ── OTA_CHECKPOINT_REQ handler ────────────────────────────────────────────
void handle_ota_checkpoint_req(const HapFrame& req) {
    uint8_t buf[8];
    buf[0] = s_ota_expected_offset & 0xFF;
    buf[1] = (s_ota_expected_offset >> 8) & 0xFF;
    buf[2] = (s_ota_expected_offset >> 16) & 0xFF;
    buf[3] = (s_ota_expected_offset >> 24) & 0xFF;
    buf[4] = s_ota_total & 0xFF;
    buf[5] = (s_ota_total >> 8) & 0xFF;
    buf[6] = (s_ota_total >> 16) & 0xFF;
    buf[7] = (s_ota_total >> 24) & 0xFF;
    hap_send(HapMsgType::OTA_CHECKPOINT_RSP, buf, sizeof(buf), 0, req.seq);
}

// ── OTA_CHUNK handler ─────────────────────────────────────────────────────
void handle_ota_chunk(const HapFrame& f) {
    if (f.payload_len < HAP_OTA_CHUNK_HDR_SIZE) {
        ESP_LOGE(TAG, "OTA_CHUNK too short: %u", f.payload_len);
        return;
    }
    const auto* hdr      = reinterpret_cast<const HapOtaChunkHdr*>(f.payload);
    const uint8_t* data  = f.payload + HAP_OTA_CHUNK_HDR_SIZE;
    const size_t data_len = f.payload_len - HAP_OTA_CHUNK_HDR_SIZE;

    // Resume: if a session is already open and offset matches, continue writing
    bool resuming = (s_ota_handle != 0 &&
                     hdr->offset == s_ota_expected_offset &&
                     hdr->offset > 0);

    if (hdr->offset == 0 && !resuming) {
        if (s_ota_handle) { esp_ota_abort(s_ota_handle); s_ota_handle = 0; }
        s_ota_part = esp_ota_get_next_update_partition(nullptr);
        if (!s_ota_part) {
            send_ota_status(false, 0, hdr->total, "no update partition");
            return;
        }
        // T20: OTA_WITH_SEQUENTIAL_WRITES instead of passing the image
        // size. Passing hdr->total makes esp_ota_begin erase the WHOLE
        // destination partition up front — multiple seconds of synchronous
        // flash erase on the dispatch task (hap_slave_task), long enough
        // to stall heartbeats and risk a HAP link-dead/retry storm.
        // SEQUENTIAL_WRITES erases lazily, one sector per write, so the
        // cost is amortised across chunks (~a few ms per 4 KB sector) and
        // no single call blocks the dispatcher for seconds.
        esp_err_t err = esp_ota_begin(s_ota_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s (total=%" PRIu32 ")",
                     esp_err_to_name(err), hdr->total);
            send_ota_status(false, 0, hdr->total, esp_err_to_name(err));
            s_ota_part = nullptr;
            return;
        }
        s_ota_expected_offset  = 0;
        s_ota_total            = hdr->total;
        s_ota_failure_notified = false;     // fresh begin — re-arm the notifier
        ota_idle_timer_rearm();             // T20: arm the idle-abort watchdog
        ESP_LOGI(TAG, "OTA begin: total=%" PRIu32 " part=%s",
                 hdr->total, s_ota_part->label);
    } else if (resuming) {
        ESP_LOGI(TAG, "OTA resume at offset=%" PRIu32, hdr->offset);
    }

    if (hdr->offset != s_ota_expected_offset) {
        ESP_LOGE(TAG, "OTA offset mismatch: expected=%" PRIu32 " got=%" PRIu32 " — aborting",
                 s_ota_expected_offset, hdr->offset);
        if (s_ota_handle) { esp_ota_abort(s_ota_handle); s_ota_handle = 0; }
        ota_idle_timer_disarm();   // T20
        s_ota_part = nullptr;
        s_ota_expected_offset = 0;
        send_ota_status(false, hdr->offset, hdr->total, "offset mismatch");
        return;
    }

    if (data_len > 0 && s_ota_handle) {
        esp_err_t err = esp_ota_write(s_ota_handle, data, data_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "esp_ota_write failed at offset=%" PRIu32 " len=%u: %s "
                     "(int_largest=%u)",
                     hdr->offset, (unsigned)data_len, esp_err_to_name(err),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            esp_ota_abort(s_ota_handle);
            ota_idle_timer_disarm();   // T20
            s_ota_handle = 0; s_ota_part = nullptr; s_ota_expected_offset = 0;
            send_ota_status(false, hdr->offset, hdr->total, esp_err_to_name(err));
            return;
        }
        s_ota_expected_offset += (uint32_t)data_len;
        ota_idle_timer_rearm();   // T20: progress — push the idle deadline out
    }

    uint32_t rcvd = hdr->offset + (uint32_t)data_len;

    if (hdr->flags & 0x01) {
        ota_idle_timer_disarm();   // T20: terminal chunk — no more idle watch
        esp_err_t err = esp_ota_end(s_ota_handle);
        s_ota_handle = 0;
        if (err != ESP_OK) {
            s_ota_part = nullptr;
            send_ota_status(false, rcvd, hdr->total, esp_err_to_name(err));
            return;
        }
        // F3 (FINDINGS.md): esp_ota_end() above already verified image
        // self-integrity (structure + appended SHA-256). Defense in depth:
        // confirm a valid ESP app descriptor before making it bootable.
        // NOTE: integrity, NOT authenticity — tamper resistance requires
        // Secure Boot v2 signed images (F2 / sdkconfig.prod.defaults).
        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(s_ota_part, &desc) != ESP_OK) {
            ESP_LOGE(TAG, "OTA image has no valid app descriptor — aborting");
            s_ota_part = nullptr;
            send_ota_status(false, rcvd, hdr->total, "bad app descriptor");
            return;
        }
        err = esp_ota_set_boot_partition(s_ota_part);
        s_ota_part = nullptr;
        if (err != ESP_OK) {
            send_ota_status(false, rcvd, hdr->total, esp_err_to_name(err));
            return;
        }
        send_ota_status(true, rcvd, hdr->total, "");
        ESP_LOGI(TAG, "OTA complete — flushing persistence + rebooting in 2 s");
        // T20: commit the writeback caches BEFORE the reboot. The rule
        // store and device shadow/zap store batch edits in PSRAM and
        // flush on a ~5 s tick or shutdown; an OTA restart that beat the
        // tick would otherwise drop the most recent rule/name/option
        // edits. rule_store_flush_now() gained a real flush barrier in
        // T9. lua_script_cache writes are already durable (tmp+rename per
        // write), so there's nothing to flush there.
        rule_store_flush_now();
        zap_store_flush_now();
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}
