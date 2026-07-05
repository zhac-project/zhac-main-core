// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// hap_slave.cpp — P4-side SPI slave, always-armed design.
//
// Architecture:
//
//   A dedicated `hap_slave` task loops on spi_slave_queue_trans() /
//   get_trans_result(). Because the task spends almost all of its time
//   inside get_trans_result() with a transaction queued, the SPI slave is
//   effectively always ready when the S3 master initiates a transfer.
//   There is no longer a window where the slave is only receptive during
//   outbound TX (which was the root cause of lost S3→P4 commands).
//
//   Outbound frames are produced by hap_slave_send() from any task: the
//   frame is heap-encoded into a blob and enqueued onto s_tx_q. The slave
//   task consumes the blob on its next loop iteration, copies it into the
//   DMA-aligned s_tx_buf, and asserts DRDY to wake the S3 master.
//
//   If no TX is pending, the slave still queues a transaction with zeroed
//   TX and DRDY held low. The S3 master only initiates reads on DRDY edges,
//   so a zero TX is never interpreted as a real frame.
//
// Invariants:
//   - Only the slave task calls spi_slave_queue_trans / get_trans_result.
//   - Only the slave task writes s_tx_buf / s_rx_buf.
//   - Producers (hap_slave_send) allocate a TX blob, enqueue it, and return
//     immediately — fire-and-forget.

#include "hap_slave.h"
#include "hap_protocol_decode.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   // esp_rom_printf — bypasses the log_vprintf hook
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "task_stacks.h"
#include <cstring>
#include <cstdlib>

#include <atomic>

static const char* TAG = "hap_slave";

static HapFrameCallback s_cb;
// Set once from task_hap AFTER the slave task is already running (boot-window
// race, REPORT.md §2.3). A std::function can't be assigned atomically, so gate
// reads on this release/acquire flag: a reader only touches s_cb once the setter
// has fully published it. Assumes single registration (the actual usage).
static std::atomic<bool> s_cb_ready{false};

// Pin assignments — P4 SPI slave (per docs/esp32_p4_gpio_allocation.md §1).
static constexpr gpio_num_t PIN_SCLK = GPIO_NUM_18;
static constexpr gpio_num_t PIN_MOSI = GPIO_NUM_19;
static constexpr gpio_num_t PIN_MISO = GPIO_NUM_20;
static constexpr gpio_num_t PIN_CS   = GPIO_NUM_21;
static constexpr gpio_num_t PIN_DRDY = GPIO_NUM_22;    // P4 → S3, data-ready

// P4 SPI slave DMA requires DMA-capable + 64-byte cache-aligned buffers.
static constexpr size_t HAP_DMA_BUF_SIZE =
    ((HAP_MAX_FRAME_SIZE + 63) / 64) * 64;
static uint8_t* s_rx_buf = nullptr;
static uint8_t* s_tx_buf = nullptr;

// ── Outgoing TX queue ─────────────────────────────────────────────────────
struct PendingTx {
    uint8_t* buf;         // malloc'd; slave task frees after use. Holds raw payload + CRC for stage-2.
    uint16_t len;         // total bytes in `buf` (== payload_len + 2 for stage-2; 0 if no payload)
    HapFrame frame_meta;  // shallow copy. payload pointer is stale; payload_len is authoritative.
};
// 128 slots: real device-event bursts (e.g. Miboxer remotes hammering
// move_to_hue / brightness_move at 10+ Hz) plus LOG_LINE forwarding plus
// event_broadcast can outpace the SPI drain rate during transients. 32
// was saturating under live load and dropping ALERT/EVT frames.
// Memory cost: 128 × sizeof(PendingTx) ≈ 8 KB on P4 (internal SRAM has
// ~768 KB total — trivial).
static constexpr size_t TX_QUEUE_LEN = 128;
static QueueHandle_t s_tx_q = nullptr;

// ── Slave task body ───────────────────────────────────────────────────────
//
// v4 two-stage protocol: stage-1 (16 B) advertises payload size; stage-2
// (round_up_4(max(my, peer)+2)) carries the payloads. Master initiates both
// stages on DRDY edges, so the slave queues stage-1, drives DRDY high if it
// has anything to send, decodes peer's stage-1, then queues stage-2 of the
// negotiated length.
static void hap_slave_task(void*) {
    ESP_LOGI(TAG, "slave task started (v4 two-stage)");
    while (true) {
        PendingTx pending{};
        const bool have_tx =
            (xQueueReceive(s_tx_q, &pending, 0) == pdTRUE);

        // Build my stage-1 header (16 B). If no TX, send a zero/empty frame.
        HapFrame my_frame{};
        if (have_tx) my_frame = pending.frame_meta;
        else         my_frame.type = static_cast<HapMsgType>(0);

        // ESP-IDF SPI slave requires DMA-capable + 64B-aligned TX/RX bufs.
        // Length must also be 64B-aligned — clock a HAP_STAGE1_CLOCK_LEN
        // slot with trailing zeros that the decoder ignores.
        memset(s_tx_buf, 0, HAP_STAGE1_CLOCK_LEN);
        hap_encode_stage1(my_frame, s_tx_buf);

        spi_slave_transaction_t st1 = {};
        st1.length    = HAP_STAGE1_CLOCK_LEN * 8;
        st1.tx_buffer = s_tx_buf;
        st1.rx_buffer = s_rx_buf;

        // Order matters: queue the slave-side descriptor BEFORE asserting
        // DRDY. Reversing these two steps creates a race where the S3
        // master ISR fires on the DRDY rising edge and clocks stage-1 in
        // the few microseconds before spi_slave_queue_trans completes —
        // the hardware FIFO is empty at that moment and the master reads
        // a stage-1 of all 0xFF (idle MISO), fails magic-byte decode, and
        // silently drops the frame. The earlier symptom — only 1-in-3
        // P4 heartbeats reaching S3 despite a healthy S3→P4 path — was
        // this race firing on the DRDY-driven recv path. queue_trans is
        // non-blocking (the call only stages a descriptor), so doing it
        // first costs nothing.
        esp_err_t err = spi_slave_queue_trans(SPI2_HOST, &st1, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "stage1 queue_trans: %s", esp_err_to_name(err));
            if (have_tx && pending.buf) free(pending.buf);
            continue;
        }
        if (have_tx) gpio_set_level(PIN_DRDY, 1);

        spi_slave_transaction_t* done = nullptr;
        err = spi_slave_get_trans_result(SPI2_HOST, &done, portMAX_DELAY);
        gpio_set_level(PIN_DRDY, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "stage1 get_trans_result: %s", esp_err_to_name(err));
            if (have_tx && pending.buf) free(pending.buf);
            continue;
        }

        // Decode peer's stage-1 header. Treat decode failures as empty peer.
        HapFrame peer{};
        HapDecodeResult dr = hap_decode_stage1(s_rx_buf, peer);
        if (dr != HAP_DECODE_OK) {
            if (dr == HAP_DECODE_BAD_MAGIC) {
                _METRIC_COUNTER_INC(METRIC_HAP_BAD_MAGIC, 1);
            } else if (dr == HAP_DECODE_BAD_VERSION) {
                _METRIC_COUNTER_INC(METRIC_HAP_VERSION_MISMATCH, 1);
                ESP_LOGE(TAG, "RX HAP_VERSION mismatch (got 0x%02x, expected 0x%02x) — "
                              "one chip needs reflashing",
                         s_rx_buf[3], HAP_VERSION);
            } else if (dr == HAP_DECODE_BAD_HDR_CRC) {
                _METRIC_COUNTER_INC(METRIC_HAP_HDR_CRC_ERRORS, 1);
            }
            peer.payload_len = 0;
            peer.type = static_cast<HapMsgType>(0);
        }

        // Compute stage-2 length: max of both sides' payload+CRC, rounded up to 4.
        const size_t my_s2_bytes   = (have_tx && pending.len > 0) ? pending.len : 0;
        const size_t peer_s2_bytes = (peer.payload_len > 0)
                                       ? static_cast<size_t>(peer.payload_len) + 2 : 0;
        size_t s2_max = (my_s2_bytes > peer_s2_bytes) ? my_s2_bytes : peer_s2_bytes;
        if (s2_max > HAP_DMA_BUF_SIZE) s2_max = HAP_DMA_BUF_SIZE;
        const size_t s2_len = (s2_max + (HAP_DMA_ALIGN - 1)) & ~(HAP_DMA_ALIGN - 1);

        if (s2_len > 0) {
            memset(s_tx_buf, 0, s2_len);
            if (have_tx && pending.len > 0) {
                memcpy(s_tx_buf, pending.buf, pending.len);
            }
            memset(s_rx_buf, 0, s2_len);

            spi_slave_transaction_t st2 = {};
            st2.length    = s2_len * 8;
            st2.tx_buffer = s_tx_buf;
            st2.rx_buffer = s_rx_buf;

            err = spi_slave_queue_trans(SPI2_HOST, &st2, portMAX_DELAY);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "stage2 queue_trans: %s", esp_err_to_name(err));
                if (have_tx && pending.buf) free(pending.buf);
                continue;
            }
            err = spi_slave_get_trans_result(SPI2_HOST, &done, portMAX_DELAY);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "stage2 get_trans_result: %s", esp_err_to_name(err));
                if (have_tx && pending.buf) free(pending.buf);
                continue;
            }

            if (peer.payload_len > 0) {
                if (hap_verify_stage2(s_rx_buf, peer.payload_len)) {
                    // peer.payload points at s_rx_buf, the LIVE DMA receive
                    // buffer — valid ONLY for the duration of this synchronous
                    // s_cb call. The next exchange memsets/overwrites it. A
                    // handler MUST NOT stash peer.payload for async use; copy
                    // the bytes out if needed. (The master path copies into a
                    // dispatch buffer first; the slave dispatches in place
                    // because the P4 HAP dispatcher is single-task — F-08 — and
                    // consumes synchronously.)
                    peer.payload = s_rx_buf;
                    if (s_cb_ready.load(std::memory_order_acquire) && s_cb) s_cb(peer);
                } else {
                    _METRIC_COUNTER_INC(METRIC_HAP_CRC_ERRORS, 1);
                    // CRC failed — don't dispatch this frame. Skip the
                    // zero-payload dispatch below since the peer was
                    // claiming a real payload that just got corrupted.
                    goto skip_zero_payload_dispatch;
                }
            }
        }
        // Dispatch zero-payload peer frames (ACK / HEARTBEAT / SET_ACK
        // with empty body) regardless of whether we had stage-2 data
        // ourselves. Without this, ACKs from S3 are decoded but never
        // delivered when the slave was busy sending its own frame —
        // leading to spurious link-dead loops on response paths.
        if (dr == HAP_DECODE_OK
            && peer.payload_len == 0
            && peer.type != static_cast<HapMsgType>(0)) {
            peer.payload = nullptr;
            if (s_cb_ready.load(std::memory_order_acquire) && s_cb) s_cb(peer);
        }
        skip_zero_payload_dispatch:;

        if (have_tx && pending.buf) free(pending.buf);
    }
}

// ── Public API ────────────────────────────────────────────────────────────
void hap_slave_init() {
    s_rx_buf = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, HAP_DMA_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    s_tx_buf = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, HAP_DMA_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    configASSERT(s_rx_buf && s_tx_buf);

    spi_bus_config_t bus = {
        .mosi_io_num           = PIN_MOSI,
        .miso_io_num           = PIN_MISO,
        .sclk_io_num           = PIN_SCLK,
        .quadwp_io_num         = -1,
        .quadhd_io_num         = -1,
        .data4_io_num          = -1,
        .data5_io_num          = -1,
        .data6_io_num          = -1,
        .data7_io_num          = -1,
        .data_io_default_level = false,
        .max_transfer_sz       = HAP_MAX_FRAME_SIZE,
        .flags                 = 0,
        .isr_cpu_id            = {},
        .intr_flags            = 0,
    };
    spi_slave_interface_config_t slave = {
        .spics_io_num  = PIN_CS,
        .flags         = 0,
        .queue_size    = 4,
        .mode          = 0,
        .post_setup_cb = nullptr,
        .post_trans_cb = nullptr,
    };
    ESP_ERROR_CHECK(spi_slave_initialize(SPI2_HOST, &bus, &slave,
                                          SPI_DMA_CH_AUTO));

    gpio_config_t drdy_cfg = {
        .pin_bit_mask = 1ULL << PIN_DRDY,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
#if SOC_GPIO_SUPPORT_PIN_HYS_FILTER
        .hys_ctrl_mode = GPIO_HYS_SOFT_DISABLE,
#endif
    };
    ESP_ERROR_CHECK(gpio_config(&drdy_cfg));
    gpio_set_level(PIN_DRDY, 0);

    s_tx_q = xQueueCreate(TX_QUEUE_LEN, sizeof(PendingTx));
    configASSERT(s_tx_q);

    // Priority 7: ahead of all application work AND tied with the watchdog
    // task so SPIFFS / Zigbee long-running calls cannot starve the SPI slave
    // task. Round-robin with watchdog at the same level is acceptable —
    // hap_slave never holds CPU long (it parks on the TX queue / DRDY).
    xTaskCreatePinnedToCore(hap_slave_task, "hap_slave", zhac::stack::kHapSlave, nullptr,
                             7, nullptr, 0);

    ESP_LOGI(TAG, "hap_slave init OK — always-armed, DRDY GPIO%d", PIN_DRDY);
}

void hap_slave_send(const HapFrame& frame) {
    // v4: pre-encode just the stage-2 blob (payload + CRC). Empty-payload
    // frames (e.g. HEARTBEAT) carry a null buf — stage-1 alone is enough.
    uint8_t* buf = nullptr;
    size_t   len = 0;
    if (frame.payload_len > 0) {
        const size_t need = static_cast<size_t>(frame.payload_len) + 2;
        buf = static_cast<uint8_t*>(malloc(need));
        if (!buf) {
            ESP_LOGW(TAG, "TX drop: malloc failed type=0x%02x seq=%u",
                     static_cast<uint8_t>(frame.type), frame.seq);
            return;
        }
        len = hap_encode_stage2(frame, buf, need);
        if (len == 0 || len > UINT16_MAX) {
            free(buf);
            ESP_LOGE(TAG, "hap_encode_stage2 overflow type=0x%02x",
                     static_cast<uint8_t>(frame.type));
            return;
        }
    }

    PendingTx item{};
    item.buf        = buf;
    item.len        = static_cast<uint16_t>(len);
    item.frame_meta = frame;
    item.frame_meta.payload = nullptr;  // pointer stale; metadata only
    if (xQueueSend(s_tx_q, &item, pdMS_TO_TICKS(50)) != pdTRUE) {
        if (buf) free(buf);
        _METRIC_COUNTER_INC(METRIC_HAP_TX_DROPS, 1);
        // Drop notification MUST bypass esp_log: the P4 log hook in
        // main.cpp (log_vprintf_hook) forwards every ESP_LOGx line to
        // S3 as a new LOG_LINE HAP frame, which goes through this same
        // hap_slave_send path — a full TX queue means the drop log
        // itself drops, logs again, loops at ~20 Hz. Use esp_rom_printf
        // to write straight to the UART, and rate-limit to once per
        // second so the serial console isn't flooded either.
        static int64_t s_last_drop_us = 0;
        static uint32_t s_dropped_in_window = 0;
        s_dropped_in_window++;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_drop_us >= 1000000LL) {
            esp_rom_printf("[hap_slave] TX queue full — dropped %u frames "
                           "(last type=0x%02x seq=%u)\n",
                           (unsigned)s_dropped_in_window,
                           static_cast<uint8_t>(frame.type), frame.seq);
            s_last_drop_us = now_us;
            s_dropped_in_window = 0;
        }
    }
}

void hap_slave_set_callback(HapFrameCallback cb) {
    s_cb = std::move(cb);
    // release: pairs with the acquire in the dispatch loop so a reader that sees
    // ready==true observes the fully-constructed s_cb (no torn read).
    s_cb_ready.store(true, std::memory_order_release);
}
