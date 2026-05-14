// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/ezsp_backend/ezsp_backend.cpp
// EZSP DeviceBackend: coordinator init, sendUnicast, permitJoining, device pool.
// Uses the same zigbee_pool for device storage (shared with ZNP backend).
#include "ezsp_backend.h"
#include "ezsp_driver.h"
#include "zigbee_mgr.h"
#include "zigbee_pool.h"
#include "zap_store.h"
#include "zhc_adapter.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <ctime>

static const char* TAG = "ezsp_be";

// ── EZSP coordinator state ──────────────────────────────────────────────
static bool     s_running       = false;
static bool     s_crashed       = false;
static uint64_t s_coordinator_ieee = 0;
static uint16_t s_coordinator_nwk  = 0;

// Zigbee HA profile
static constexpr uint16_t ZB_PROFILE_HA = 0x0104;

// ── Helper: send EZSP command with simple payload ────────────────────────
static bool ezsp_cmd(uint16_t cmd_id, const uint8_t* pl, uint8_t pl_len,
                     EzspFrame& rsp, uint32_t timeout = 3000) {
    EzspFrame req = ezsp_make_req(cmd_id, pl, pl_len);
    return ezsp_sreq_retry(req, rsp, timeout, 3);
}

static bool ezsp_cmd_ok(uint16_t cmd_id, const uint8_t* pl, uint8_t pl_len,
                        const char* label) {
    EzspFrame rsp{};
    if (!ezsp_cmd(cmd_id, pl, pl_len, rsp)) {
        ESP_LOGE(TAG, "%s: no response", label);
        return false;
    }
    if (rsp.payload_len >= 1 && rsp.payload[0] != EMBER_SUCCESS) {
        ESP_LOGE(TAG, "%s: status=0x%02x", label, rsp.payload[0]);
        return false;
    }
    ESP_LOGI(TAG, "%s: OK", label);
    return true;
}

// ── EZSP callback handlers ──────────────────────────────────────────────

static void on_stack_status(const EzspFrame& f) {
    if (f.payload_len < 1) return;
    uint8_t status = f.payload[0];
    ESP_LOGI(TAG, "stackStatus: 0x%02x", status);
    if (status == EMBER_NETWORK_UP) {
        s_running = true;
        ESP_LOGI(TAG, "Network UP");
    } else if (status == EMBER_NETWORK_DOWN) {
        s_running = false;
        s_crashed = true;
        ESP_LOGW(TAG, "Network DOWN");
    }
}

static void on_trust_center_join(const EzspFrame& f) {
    // Payload: newNodeId(2) newNodeEui64(8) status(1) policyDecision(1) parentOfNewNodeId(2)
    if (f.payload_len < 13) return;
    uint16_t nwk = (uint16_t)f.payload[0] | ((uint16_t)f.payload[1] << 8);
    uint64_t ieee = 0;
    for (int i = 0; i < 8; i++)
        ieee |= ((uint64_t)f.payload[2 + i]) << (8 * i);
    uint8_t status = f.payload[10];

    if (status == 0x01 || status == 0x03) {
        // Standard/rejoin security — device joined
        ESP_LOGI(TAG, "Device joined: ieee=0x%016llx nwk=0x%04x",
                 (unsigned long long)ieee, nwk);
        ZapDevice* dev = pool_find_by_ieee(ieee);
        if (!dev) {
            dev = pool_add();
            if (dev) {
                dev->ieee_addr = ieee;
                dev->nwk_addr  = nwk;
                zap_store_mark_dirty(dev, ZAP_PERSIST_HIGH);
            }
        } else {
            dev->nwk_addr = nwk;
            zigbee_pool_mark_dirty();
        }
    } else if (status == 0x02) {
        // Device left
        ESP_LOGI(TAG, "Device left: ieee=0x%016llx", (unsigned long long)ieee);
    }
}

static void on_incoming_message(const EzspFrame& f) {
    // Payload: type(1) apsFrame(11) lastHopLqi(1) lastHopRssi(1)
    //          sender(2) bindingIndex(1) addressIndex(1) messageLength(1) message(N)
    if (f.payload_len < 19) return;

    // Parse EmberApsFrame from payload offset 1
    // uint16_t profile  = (uint16_t)f.payload[1]  | ((uint16_t)f.payload[2] << 8);
    uint16_t cluster  = (uint16_t)f.payload[3]  | ((uint16_t)f.payload[4] << 8);
    uint8_t  src_ep   = f.payload[5];
    uint8_t  dst_ep   = f.payload[6];
    // uint16_t options   = (uint16_t)f.payload[7]  | ((uint16_t)f.payload[8] << 8);
    uint16_t group_id  = (uint16_t)f.payload[9]  | ((uint16_t)f.payload[10] << 8);
    // uint8_t  aps_seq   = f.payload[11];

    uint8_t  lqi       = f.payload[12];
    // int8_t   rssi      = (int8_t)f.payload[13];
    uint16_t sender    = (uint16_t)f.payload[14] | ((uint16_t)f.payload[15] << 8);
    // uint8_t  binding   = f.payload[16];
    // uint8_t  addr_idx  = f.payload[17];
    uint8_t  msg_len   = f.payload[18];
    const uint8_t* msg = f.payload + 19;

    if (f.payload_len < 19u + msg_len) return;

    (void)dst_ep;

    // Update device LQI + last-seen. Use wall-clock Unix seconds via
    // time(nullptr) (same source as the ZNP path in zigbee_mgr.cpp).
    // The old esp_timer_get_time() values are seconds-since-boot
    // (uptime), not Unix epoch, so the SPA's `Date.now()/1000 -
    // last_seen` calc treated them as 1970+uptime → 55-year deltas
    // (the "494079h ago" UI symptom). If SNTP hasn't ticked yet
    // time() returns a small value — skip the write so the field
    // doesn't get junk; UI renders "—" instead.
    ZapDevice* dev = pool_find_by_nwk(sender);
    if (dev) {
        dev->link_quality = lqi;
        const time_t now = time(nullptr);
        if (now > 1577836800) {  // > 2020-01-01 → SNTP plausibly synced
            dev->last_seen = (uint32_t)now;
        }
    }

    // Route ZCL frame through the ZHC adapter (same as ZNP path). The
    // adapter publishes decoded attributes through its own shadow hook,
    // so no explicit device_shadow_process() call is required here.
    if (dev && msg_len >= 3) {
        zhac_adapter_try_decode(dev->ieee_addr,
                                 dev->model_id,
                                 dev->manufacturer_name,
                                 group_id,
                                 cluster, src_ep, lqi,
                                 msg, msg_len);
    }
}

// ── Coordinator startup sequence ─────────────────────────────────────────

static bool coordinator_start() {
    s_running = false;
    s_crashed = false;

    // Step 1: Hardware reset + ASH reset
    ezsp_hw_reset();
    if (!ezsp_ash_reset(5000)) {
        ESP_LOGE(TAG, "ASH reset failed");
        return false;
    }

    // Step 2: EZSP version check
    {
        uint8_t pl[] = { 0x0D };  // Request EZSP protocol version 13
        EzspFrame rsp{};
        if (!ezsp_cmd(EZSP_VERSION, pl, 1, rsp)) {
            ESP_LOGE(TAG, "getVersion failed");
            return false;
        }
        if (rsp.payload_len >= 4) {
            uint8_t proto_ver  = rsp.payload[0];
            uint8_t stack_type = rsp.payload[1];
            uint16_t stack_ver = (uint16_t)rsp.payload[2] | ((uint16_t)rsp.payload[3] << 8);
            ESP_LOGI(TAG, "EZSP version: proto=%d stack_type=%d stack_ver=0x%04x",
                     proto_ver, stack_type, stack_ver);
        }
    }

    // Step 3: Get coordinator EUI64
    {
        EzspFrame rsp{};
        if (ezsp_cmd(EZSP_GET_EUI64, nullptr, 0, rsp) && rsp.payload_len >= 8) {
            s_coordinator_ieee = 0;
            for (int i = 0; i < 8; i++)
                s_coordinator_ieee |= ((uint64_t)rsp.payload[i]) << (8 * i);
            ESP_LOGI(TAG, "Coordinator IEEE: 0x%016llx",
                     (unsigned long long)s_coordinator_ieee);
        }
    }

    // Step 4: Set security — install HA link key
    {
        // setInitialSecurityState: bitmask(2) + preconfiguredKey(16) + networkKey(16) + ...
        // HA well-known link key: 5A:69:67:42:65:65:41:6C:6C:69:61:6E:63:65:30:39
        uint8_t pl[24];
        memset(pl, 0, sizeof(pl));
        // bitmask: HAVE_PRECONFIGURED_KEY | HAVE_NETWORK_KEY | REQUIRE_ENCRYPTED_KEY
        pl[0] = 0x08; pl[1] = 0x02;  // 0x0208
        // preconfigured key (HA link key)
        static const uint8_t ha_key[] = {
            0x5A, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6C,
            0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65, 0x30, 0x39
        };
        memcpy(pl + 2, ha_key, 16);
        // network key — leave zeroed (NCP generates one)
        // remaining fields (keySequence, etc.) left zeroed

        ezsp_cmd_ok(EZSP_SET_INITIAL_SECURITY, pl, sizeof(pl), "setInitialSecurity");
    }

    // Step 5: Try networkInit first (resume existing network)
    {
        uint8_t pl[] = { 0x00, 0x00 };  // networkInitStruct (unused, 2 bytes)
        EzspFrame rsp{};
        if (ezsp_cmd(EZSP_NETWORK_INIT, pl, 2, rsp) &&
            rsp.payload_len >= 1 && rsp.payload[0] == EMBER_SUCCESS) {
            ESP_LOGI(TAG, "networkInit: existing network resumed");
        } else {
            ESP_LOGI(TAG, "networkInit: no existing network — forming new");
            // Step 5b: formNetwork with EmberNetworkParameters
            // extendedPanId(8) + panId(2) + radioTxPower(1) + radioChannel(1) +
            // joinMethod(1) + nwkManagerId(2) + nwkUpdateId(1) + channels(4)
            uint8_t nparams[20];
            memset(nparams, 0, sizeof(nparams));
            // extendedPanId: all zeros = NCP picks random
            // panId: 0x0000 = NCP picks random
            nparams[8]  = 0x00; nparams[9] = 0x00;  // panId
            nparams[10] = 0x08;  // radioTxPower (8 dBm)
            nparams[11] = 0x0B;  // radioChannel 11 (default)
            nparams[12] = 0x00;  // joinMethod: MAC association
            nparams[13] = 0x00; nparams[14] = 0x00;  // nwkManagerId
            nparams[15] = 0x00;  // nwkUpdateId
            // channels: bitmask for channel 11-26
            nparams[16] = 0x00; nparams[17] = 0xF8;
            nparams[18] = 0xFF; nparams[19] = 0x07;  // channels 11-26

            if (!ezsp_cmd_ok(EZSP_FORM_NETWORK, nparams, sizeof(nparams), "formNetwork")) {
                ESP_LOGE(TAG, "formNetwork failed");
                return false;
            }
        }
    }

    // Wait for stackStatus = NETWORK_UP
    for (int i = 0; i < 100 && !s_running; i++) {
        ezsp_driver_poll();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_running) {
        ESP_LOGE(TAG, "Network did not come up within 10 s");
        return false;
    }

    // Step 6: Get node ID
    {
        EzspFrame rsp{};
        if (ezsp_cmd(EZSP_GET_NODE_ID, nullptr, 0, rsp) && rsp.payload_len >= 2) {
            s_coordinator_nwk = (uint16_t)rsp.payload[0] | ((uint16_t)rsp.payload[1] << 8);
            ESP_LOGI(TAG, "Coordinator NWK: 0x%04x", s_coordinator_nwk);
        }
    }

    // Step 7: Add endpoint (EP=1, profile=0x0104 HA)
    {
        uint8_t pl[12];
        pl[0]  = 0x01;  // endpoint
        pl[1]  = ZB_PROFILE_HA & 0xFF; pl[2] = (ZB_PROFILE_HA >> 8) & 0xFF;  // profile
        pl[3]  = 0x05; pl[4] = 0x00;  // deviceId = 0x0005 (configuration tool)
        pl[5]  = 0x00;  // appFlags
        pl[6]  = 0x01;  // inputClusterCount
        pl[7]  = 0x01;  // outputClusterCount
        // input cluster: 0x0000 (Basic)
        pl[8]  = 0x00; pl[9] = 0x00;
        // output cluster: 0x0000 (Basic)
        pl[10] = 0x00; pl[11] = 0x00;

        ezsp_cmd_ok(EZSP_ADD_ENDPOINT, pl, sizeof(pl), "addEndpoint EP1");
    }

    // Step 8: Set concentrator (source routing)
    {
        uint8_t pl[7];
        pl[0] = 0x01;  // on
        pl[1] = 0xF9; pl[2] = 0xFF;  // concentratorType = LOW_RAM (0xFFF9)
        pl[3] = 0x0A; pl[4] = 0x00;  // minTime = 10s
        pl[5] = 0x3C; pl[6] = 0x00;  // maxTime = 60s
        ezsp_cmd_ok(EZSP_SET_CONCENTRATOR, pl, sizeof(pl), "setConcentrator");
    }

    ESP_LOGI(TAG, "EZSP coordinator ready — ieee=0x%016llx nwk=0x%04x",
             (unsigned long long)s_coordinator_ieee, s_coordinator_nwk);
    return true;
}

// ── DeviceBackend callbacks ──────────────────────────────────────────────

static bool ezsp_init() {
    ezsp_driver_init();

    // Register EZSP callbacks before starting coordinator
    ezsp_register_callback(EZSP_CB_STACK_STATUS, on_stack_status);
    ezsp_register_callback(EZSP_CB_TRUST_CENTER_JOIN, on_trust_center_join);
    ezsp_register_callback(EZSP_CB_INCOMING_MESSAGE, on_incoming_message);

    return coordinator_start();
}

static bool ezsp_poll() {
    ezsp_driver_poll();
    return true;
}

static bool ezsp_is_running() {
    return s_running && !s_crashed;
}

static bool ezsp_start_discovery(uint8_t duration_s) {
    uint8_t pl[] = { duration_s };
    return ezsp_cmd_ok(EZSP_PERMIT_JOINING, pl, 1, "permitJoining");
}

static bool ezsp_stop_discovery() {
    uint8_t pl[] = { 0x00 };
    return ezsp_cmd_ok(EZSP_PERMIT_JOINING, pl, 1, "permitJoining(close)");
}

static bool ezsp_interview(uint64_t ieee, uint16_t /*addr_hint*/) {
    // TODO: implement EZSP ZDO interview (NODE_DESC, ACTIVE_EP, SIMPLE_DESC)
    ESP_LOGW(TAG, "interview not yet implemented for EZSP");
    (void)ieee;
    return false;
}

static bool ezsp_write_attr(uint64_t ieee, uint8_t ep, const char* key, int32_t val) {
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) return false;

    // Caller-supplied endpoint wins; 0 means "device default".
    if (ep == 0) {
        ep = (dev->endpoint_count > 0) ? dev->endpoints[0] : 1;
    }
    // sendUnicast helper for common attribute commands. ZHC's TzConverters
    // require an AfSendFn hook wired to EZSP (not implemented yet), so we
    // keep the raw cluster path below until that bridge exists.
    // Build ZCL frame, then wrap in EZSP sendUnicast
    uint8_t zcl_frame[8];
    uint8_t zcl_len = 0;
    uint16_t cluster = 0;

    if (strcmp(key, "state") == 0) {
        cluster = 0x0006;
        zcl_frame[0] = 0x01;  // cluster-specific
        zcl_frame[1] = 0x01;  // seq
        zcl_frame[2] = (val != 0) ? 0x01 : 0x00;  // On/Off
        zcl_len = 3;
    } else if (strcmp(key, "brightness") == 0) {
        cluster = 0x0008;
        zcl_frame[0] = 0x01;
        zcl_frame[1] = 0x01;
        zcl_frame[2] = 0x04;  // MoveToLevel
        zcl_frame[3] = (uint8_t)(val & 0xFF);
        zcl_frame[4] = 0x00; zcl_frame[5] = 0x00;  // transition = 0
        zcl_len = 6;
    } else if (strcmp(key, "color_temp") == 0) {
        cluster = 0x0300;
        zcl_frame[0] = 0x01;
        zcl_frame[1] = 0x01;
        zcl_frame[2] = 0x0A;  // MoveToColorTemperature
        zcl_frame[3] = val & 0xFF;
        zcl_frame[4] = (val >> 8) & 0xFF;
        zcl_frame[5] = 0x00; zcl_frame[6] = 0x00;  // transition = 0
        zcl_len = 7;
    } else {
        ESP_LOGW(TAG, "write_attr: no handler for key='%s'", key);
        return false;
    }

    // Build sendUnicast payload:
    // type(1) + indexOrDestination(2) + EmberApsFrame(11) + messageTag(1) + messageLength(1) + message(N)
    uint8_t pl[16 + 8];  // max ZCL frame = 8
    pl[0]  = 0x00;  // EMBER_OUTGOING_DIRECT
    pl[1]  = dev->nwk_addr & 0xFF;
    pl[2]  = (dev->nwk_addr >> 8) & 0xFF;
    // EmberApsFrame: profileId(2) clusterId(2) srcEp(1) dstEp(1) options(2) groupId(2) sequence(1)
    pl[3]  = ZB_PROFILE_HA & 0xFF; pl[4] = (ZB_PROFILE_HA >> 8) & 0xFF;
    pl[5]  = cluster & 0xFF; pl[6] = (cluster >> 8) & 0xFF;
    pl[7]  = 0x01;  // srcEndpoint = 1
    pl[8]  = ep;    // dstEndpoint
    pl[9]  = 0x40; pl[10] = 0x00;  // options: APS_RETRY (0x0040)
    pl[11] = 0x00; pl[12] = 0x00;  // groupId
    pl[13] = 0x00;  // sequence (NCP fills)
    pl[14] = 0x01;  // messageTag
    pl[15] = zcl_len;
    memcpy(pl + 16, zcl_frame, zcl_len);

    EzspFrame rsp{};
    if (!ezsp_cmd(EZSP_SEND_UNICAST, pl, (uint8_t)(16 + zcl_len), rsp)) {
        ESP_LOGE(TAG, "sendUnicast failed for key='%s'", key);
        return false;
    }
    if (rsp.payload_len >= 1 && rsp.payload[0] != EMBER_SUCCESS) {
        ESP_LOGE(TAG, "sendUnicast status=0x%02x key='%s'", rsp.payload[0], key);
        return false;
    }
    return true;
}

static bool ezsp_read_attr(uint64_t /*ieee*/, uint8_t /*ep*/, const char* /*key*/) {
    return false;  // TODO
}

static bool ezsp_get_device_list(ZapDevice* out, uint16_t max, uint16_t* count_out) {
    uint16_t n = pool_count();
    if (n > max) n = max;
    memcpy(out, pool_all(), n * sizeof(ZapDevice));
    *count_out = n;
    return true;
}

static bool ezsp_get_device(uint64_t ieee, ZapDevice* out) {
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) return false;
    *out = *dev;
    return true;
}

static bool ezsp_remove_device(uint64_t ieee) {
    // TODO: send EZSP removeDevice command
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) return false;
    zigbee_pool_remove(ieee);
    zap_store_delete_device(ieee);
    return true;
}

static bool ezsp_rename_device(uint64_t ieee, const char* name) {
    ZapDevice* dev = pool_find_by_ieee(ieee);
    if (!dev) return false;
    strncpy(dev->friendly_name, name, sizeof(dev->friendly_name) - 1);
    dev->friendly_name[sizeof(dev->friendly_name) - 1] = '\0';
    zap_store_mark_dirty(dev, ZAP_PERSIST_HIGH);
    return true;
}

// ── Singleton instance ───────────────────────────────────────────────────

static DeviceBackend s_ezsp_backend = {
    .protocol        = PROTO_ZIGBEE,  // EZSP is also Zigbee — same protocol, different NCP
    .name            = "Zigbee (EZSP)",
    .init            = ezsp_init,
    .poll            = ezsp_poll,
    .is_running      = ezsp_is_running,
    .start_discovery = ezsp_start_discovery,
    .stop_discovery  = ezsp_stop_discovery,
    .interview       = ezsp_interview,
    .write_attr      = ezsp_write_attr,
    .read_attr       = ezsp_read_attr,
    .get_device_list = ezsp_get_device_list,
    .get_device      = ezsp_get_device,
    .remove_device   = ezsp_remove_device,
    .rename_device   = ezsp_rename_device,
};

DeviceBackend* ezsp_backend_instance() {
    return &s_ezsp_backend;
}

bool ezsp_backend_register() {
    return device_backend_register(&s_ezsp_backend);
}
