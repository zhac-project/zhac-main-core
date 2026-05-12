// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// p4_ota.cpp — OTA update handler for P4 core firmware
// Receives binary chunks from S3 over HAP, then flashes.
// TODO: Add SHA-256 image integrity verification (blocked on mbedtls linkage in IDF v6).
#include "p4_ota.h"
#include "hap_dispatch.h"
#include "hap_json.h"
#include "hap_session.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char* TAG = "p4_ota";

// ── OTA state (file-scope so checkpoint query can read it) ────────────────
static esp_ota_handle_t        s_ota_handle          = 0;
static const esp_partition_t*  s_ota_part            = nullptr;
static uint32_t                s_ota_expected_offset = 0;
static uint32_t                s_ota_total           = 0;

// ── OTA helpers ───────────────────────────────────────────────────────────
static void send_ota_status(bool ok, uint32_t rcvd, uint32_t total, const char* err_msg) {
    HapOtaStatus s{};
    s.ok    = ok;
    s.rcvd  = rcvd;
    s.total = total;
    strncpy(s.err, err_msg, sizeof(s.err) - 1);
    uint8_t buf[128];
    uint16_t len = 0;
    if (!hap_json_encode_ota_status(buf, sizeof(buf), &len, s)) return;
    hap_send(HapMsgType::OTA_STATUS, buf, len);
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
        esp_err_t err = esp_ota_begin(s_ota_part, hdr->total, &s_ota_handle);
        if (err != ESP_OK) {
            send_ota_status(false, 0, hdr->total, esp_err_to_name(err));
            s_ota_part = nullptr;
            return;
        }
        s_ota_expected_offset = 0;
        s_ota_total           = hdr->total;
        ESP_LOGI(TAG, "OTA begin: total=%" PRIu32 " part=%s",
                 hdr->total, s_ota_part->label);
    } else if (resuming) {
        ESP_LOGI(TAG, "OTA resume at offset=%" PRIu32, hdr->offset);
    }

    if (hdr->offset != s_ota_expected_offset) {
        ESP_LOGE(TAG, "OTA offset mismatch: expected=%" PRIu32 " got=%" PRIu32 " — aborting",
                 s_ota_expected_offset, hdr->offset);
        if (s_ota_handle) { esp_ota_abort(s_ota_handle); s_ota_handle = 0; }
        s_ota_part = nullptr;
        s_ota_expected_offset = 0;
        send_ota_status(false, hdr->offset, hdr->total, "offset mismatch");
        return;
    }

    if (data_len > 0 && s_ota_handle) {
        esp_err_t err = esp_ota_write(s_ota_handle, data, data_len);
        if (err != ESP_OK) {
            esp_ota_abort(s_ota_handle);
            s_ota_handle = 0; s_ota_part = nullptr; s_ota_expected_offset = 0;
            send_ota_status(false, hdr->offset, hdr->total, esp_err_to_name(err));
            return;
        }
        s_ota_expected_offset += (uint32_t)data_len;
    }

    uint32_t rcvd = hdr->offset + (uint32_t)data_len;

    if (hdr->flags & 0x01) {
        esp_err_t err = esp_ota_end(s_ota_handle);
        s_ota_handle = 0;
        if (err != ESP_OK) {
            s_ota_part = nullptr;
            send_ota_status(false, rcvd, hdr->total, esp_err_to_name(err));
            return;
        }
        err = esp_ota_set_boot_partition(s_ota_part);
        s_ota_part = nullptr;
        if (err != ESP_OK) {
            send_ota_status(false, rcvd, hdr->total, esp_err_to_name(err));
            return;
        }
        send_ota_status(true, rcvd, hdr->total, "");
        ESP_LOGI(TAG, "OTA complete — rebooting in 2 s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}
