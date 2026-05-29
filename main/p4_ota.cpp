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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

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
        esp_err_t err = esp_ota_begin(s_ota_part, hdr->total, &s_ota_handle);
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
            ESP_LOGE(TAG,
                     "esp_ota_write failed at offset=%" PRIu32 " len=%u: %s "
                     "(int_largest=%u)",
                     hdr->offset, (unsigned)data_len, esp_err_to_name(err),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
        ESP_LOGI(TAG, "OTA complete — rebooting in 2 s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}
