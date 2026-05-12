// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/zigbee_backend/zigbee_backend.cpp
// Thin adapter: wraps existing zigbee_mgr functions as a DeviceBackend.
#include "zigbee_backend.h"
#include "zigbee_mgr.h"
#include "zigbee_pool.h"
#include "zap_store.h"
#include "zhc_adapter.h"
#include "znp_transport.h"   // znp_get_state / ZnpTransportState
#include "esp_log.h"
#include <cstring>

static const char* TAG = "zb_backend";

// ── DeviceBackend callbacks ──────────────────────────────────────────────

static bool zb_init() {
    return zigbee_mgr_init();
}

static bool zb_is_running() {
    // Report healthy only if the manager hasn't flagged a crash AND the
    // ZNP transport is actually Up. Previously we only checked the local
    // crash flag, so a transport stuck in Recovering/Error would look
    // healthy to higher layers (CODEX §5).
    if (zigbee_mgr_crashed()) return false;
    const auto st = znp_get_state();
    return st == ZnpTransportState::Up;
}

static bool zb_start_discovery(uint8_t duration_s) {
    return zigbee_permit_join(duration_s);
}

static bool zb_stop_discovery() {
    return zigbee_permit_join(0);
}

static bool zb_interview(uint64_t ieee, uint16_t /*addr_hint*/) {
    // Propagate the real scheduling result so REST callers see a 500
    // instead of a silent success when the queue is full (CODEX §7).
    return zigbee_interview_trigger(ieee);
}

static bool zb_write_attr(uint64_t ieee, uint8_t ep, const char* key, int32_t val) {
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) {
        ESP_LOGW(TAG, "write_attr: device 0x%016llx not in pool", (unsigned long long)ieee);
        return false;
    }

    // Resolve endpoint: caller-supplied takes precedence; fall back to the
    // device's first registered endpoint when caller passes 0.
    if (ep == 0) {
        ep = (dev->endpoint_count > 0) ? dev->endpoints[0] : 1;
    }

    // ZHC-first: TzConverter resolution via zhac_adapter.
    if (zhac_adapter_send_uint(dev->ieee_addr,
                                dev->model_id,
                                dev->manufacturer_name,
                                dev->nwk_addr, ep, key,
                                static_cast<uint64_t>(val))) {
        return true;
    }

    // Fallback: direct key→cluster mapping for common attributes
    if (strcmp(key, "state") == 0) {
        uint8_t cmd = (val != 0) ? 0x01 : 0x00;
        return zigbee_zcl_on_off(dev->nwk_addr, ep, cmd);
    }
    if (strcmp(key, "brightness") == 0) {
        return zigbee_zcl_level(dev->nwk_addr, ep, (uint8_t)(val & 0xFF), 0);
    }
    if (strcmp(key, "color_temp") == 0) {
        return zigbee_zcl_color_temp(dev->nwk_addr, ep, (uint16_t)(val & 0xFFFF), 0);
    }

    ESP_LOGW(TAG, "write_attr: no handler for key='%s' on 0x%016llx", key,
             (unsigned long long)ieee);
    return false;
}

static bool zb_read_attr(uint64_t /*ieee*/, uint8_t /*ep*/, const char* /*key*/) {
    // TODO: implement ZCL read attribute
    return false;
}

static bool zb_get_device_list(ZapDevice* out, uint16_t max, uint16_t* count_out) {
    // Hold the pool lock across count + memcpy so interview/remove can't
    // swap-with-last under us mid-copy.
    zigbee_pool_lock();
    uint16_t n = pool_count();
    if (n > max) n = max;
    memcpy(out, pool_all(), n * sizeof(ZapDevice));
    *count_out = n;
    zigbee_pool_unlock();
    return true;
}

static bool zb_get_device(uint64_t ieee, ZapDevice* out) {
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) return false;
    *out = *dev;
    return true;
}

static bool zb_remove_device(uint64_t ieee) {
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) return false;
    zigbee_leave_req(dev->nwk_addr, ieee);
    zigbee_pool_remove(ieee);
    zap_store_delete_device(ieee);
    return true;
}

static bool zb_rename_device(uint64_t ieee, const char* name) {
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) return false;
    strncpy(dev->friendly_name, name, sizeof(dev->friendly_name) - 1);
    dev->friendly_name[sizeof(dev->friendly_name) - 1] = '\0';
    zap_store_mark_dirty(dev, ZAP_PERSIST_HIGH);
    return true;
}

// ── Singleton instance ───────────────────────────────────────────────────

static DeviceBackend s_zigbee_backend = {
    .protocol       = PROTO_ZIGBEE,
    .name           = "Zigbee",
    .init           = zb_init,
    // .poll — not needed: znp_driver spawns its own RX/worker tasks
    .poll           = nullptr,
    .is_running     = zb_is_running,
    .start_discovery = zb_start_discovery,
    .stop_discovery = zb_stop_discovery,
    .interview      = zb_interview,
    .write_attr     = zb_write_attr,
    .read_attr      = zb_read_attr,
    .get_device_list = zb_get_device_list,
    .get_device     = zb_get_device,
    .remove_device  = zb_remove_device,
    .rename_device  = zb_rename_device,
};

DeviceBackend* zigbee_backend_instance() {
    return &s_zigbee_backend;
}

bool zigbee_backend_register() {
    return device_backend_register(&s_zigbee_backend);
}
