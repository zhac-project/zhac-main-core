// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/ezsp_driver/include/ezsp_driver.h
// EZSP/ASH driver for Silicon Labs EFR32 NCP.
// Mirrors znp_driver API patterns but uses ASH framing + EZSP protocol.
#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

// ── ASH constants ────────────────────────────────────────────────────────
static constexpr uint8_t ASH_FLAG       = 0x7E;  // frame delimiter
static constexpr uint8_t ASH_ESCAPE     = 0x7D;  // escape byte
static constexpr uint8_t ASH_CANCEL     = 0x1A;  // cancel / reset
static constexpr uint8_t ASH_SUB        = 0x18;  // substitute
static constexpr uint8_t ASH_XON        = 0x11;  // software flow control
static constexpr uint8_t ASH_XOFF       = 0x13;  // software flow control

static constexpr size_t  EZSP_MAX_PAYLOAD = 200;  // max EZSP payload
static constexpr size_t  ASH_MAX_FRAME    = 512;  // max encoded ASH frame (with escaping)

// ── EZSP frame ───────────────────────────────────────────────────────────
struct EzspFrame {
    uint8_t        sequence;      // EZSP sequence number
    uint16_t       frame_control; // EZSP frame control word
    uint16_t       command_id;    // EZSP command/response ID
    const uint8_t* payload;       // non-owning pointer
    uint8_t        payload_len;
};

// ── EZSP command IDs ─────────────────────────────────────────────────────
static constexpr uint16_t EZSP_VERSION              = 0x0000;
static constexpr uint16_t EZSP_GET_CONFIG_VALUE     = 0x0052;
static constexpr uint16_t EZSP_SET_CONFIG_VALUE     = 0x0053;
static constexpr uint16_t EZSP_SET_VALUE            = 0x00AB;
static constexpr uint16_t EZSP_GET_VALUE            = 0x00AA;
static constexpr uint16_t EZSP_SET_POLICY           = 0x0055;
static constexpr uint16_t EZSP_NETWORK_INIT         = 0x0017;
static constexpr uint16_t EZSP_NETWORK_STATE        = 0x0018;
static constexpr uint16_t EZSP_FORM_NETWORK         = 0x001E;
static constexpr uint16_t EZSP_LEAVE_NETWORK        = 0x0020;
static constexpr uint16_t EZSP_PERMIT_JOINING       = 0x0022;
static constexpr uint16_t EZSP_GET_NETWORK_PARAMS   = 0x0028;
static constexpr uint16_t EZSP_GET_EUI64            = 0x0026;
static constexpr uint16_t EZSP_GET_NODE_ID          = 0x0027;
static constexpr uint16_t EZSP_SEND_UNICAST         = 0x0034;
static constexpr uint16_t EZSP_SEND_BROADCAST       = 0x0036;
static constexpr uint16_t EZSP_SET_CONCENTRATOR     = 0x0010;
static constexpr uint16_t EZSP_SET_SOURCE_ROUTE     = 0x00AE;
static constexpr uint16_t EZSP_ADD_ENDPOINT         = 0x0002;
static constexpr uint16_t EZSP_SET_INITIAL_SECURITY  = 0x0068;
static constexpr uint16_t EZSP_CLEAR_KEY_TABLE      = 0x00B1;
static constexpr uint16_t EZSP_START_SCAN           = 0x001A;

// ── EZSP callback IDs (unsolicited from NCP) ────────────────────────────
static constexpr uint16_t EZSP_CB_STACK_STATUS       = 0x0019;
static constexpr uint16_t EZSP_CB_TRUST_CENTER_JOIN  = 0x0024;
static constexpr uint16_t EZSP_CB_INCOMING_MESSAGE   = 0x0045;
static constexpr uint16_t EZSP_CB_MESSAGE_SENT       = 0x003F;
static constexpr uint16_t EZSP_CB_CHILD_JOIN         = 0x0023;

// ── EmberStatus values ───────────────────────────────────────────────────
static constexpr uint8_t EMBER_SUCCESS              = 0x00;
static constexpr uint8_t EMBER_NETWORK_UP           = 0x90;
static constexpr uint8_t EMBER_NETWORK_DOWN         = 0x91;
static constexpr uint8_t EMBER_NETWORK_OPENED       = 0x9C;
static constexpr uint8_t EMBER_NETWORK_CLOSED       = 0x98;

// ── EmberApsFrame (for sendUnicast) ──────────────────────────────────────
struct EmberApsFrame {
    uint16_t profile_id;     // 0x0104 for Zigbee HA
    uint16_t cluster_id;
    uint8_t  src_endpoint;   // usually 1 (coordinator)
    uint8_t  dst_endpoint;
    uint16_t options;        // 0x0040 = APS retry
    uint16_t group_id;
    uint8_t  sequence;
};

// ── Callback type ────────────────────────────────────────────────────────
using EzspCallbackFn = std::function<void(const EzspFrame&)>;

// ── ASH framing ──────────────────────────────────────────────────────────
// CRC16-CCITT (poly 0x1021, init 0xFFFF)
uint16_t ash_crc16(const uint8_t* data, size_t len);

// Encode EZSP payload into ASH DATA frame (with randomization, CRC, byte stuffing)
size_t ash_encode_data(uint8_t frame_num, uint8_t ack_num, bool retransmit,
                       const uint8_t* ezsp_data, size_t ezsp_len,
                       uint8_t* out, size_t out_cap);

// Encode ASH ACK frame
size_t ash_encode_ack(uint8_t ack_num, uint8_t* out, size_t out_cap);

// Encode ASH RST frame (reset)
size_t ash_encode_rst(uint8_t* out, size_t out_cap);

// ── Driver API (mirrors znp_driver pattern) ──────────────────────────────

// Initialize UART and ASH state. GPIO pins come from Kconfig.
void ezsp_driver_init();

// Synchronous EZSP command: send request, wait for response.
// Like znp_sreq but over ASH framing.
bool ezsp_sreq(const EzspFrame& req, EzspFrame& rsp_out, uint32_t timeout_ms = 3000);

// Retry wrapper
bool ezsp_sreq_retry(const EzspFrame& req, EzspFrame& rsp_out,
                     uint32_t timeout_ms = 3000, int max_attempts = 3);

// Register callback for unsolicited EZSP callbacks (like znp_register_areq)
void ezsp_register_callback(uint16_t command_id, EzspCallbackFn cb);

// Called from task loop — reads UART, handles ASH ACK/NAK, dispatches frames
void ezsp_driver_poll();

// Hardware reset EFR32 — drives nRESET low then releases
void ezsp_hw_reset();

// Send ASH RST and wait for RSTACK response
bool ezsp_ash_reset(uint32_t timeout_ms = 5000);

// Helper: build an EZSP request frame with command_id and payload
EzspFrame ezsp_make_req(uint16_t command_id, const uint8_t* payload = nullptr,
                        uint8_t payload_len = 0);
