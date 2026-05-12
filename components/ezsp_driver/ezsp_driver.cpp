// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// components/ezsp_driver/ezsp_driver.cpp
// EZSP/ASH driver for Silicon Labs EFR32 NCP.
// ASH framing ported from Tasmota xdrv_23_zigbee_9_serial.ino.
#include "ezsp_driver.h"
#include "esp_log.h"
#include <cstring>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char* TAG = "ezsp_drv";

// ── Pin assignments from Kconfig ─────────────────────────────────────────
#ifdef CONFIG_ZHAC_EZSP_UART_TX_GPIO
static constexpr gpio_num_t  EZSP_PIN_TX     = (gpio_num_t)CONFIG_ZHAC_EZSP_UART_TX_GPIO;
static constexpr gpio_num_t  EZSP_PIN_RX     = (gpio_num_t)CONFIG_ZHAC_EZSP_UART_RX_GPIO;
static constexpr gpio_num_t  EZSP_PIN_NRESET = (gpio_num_t)CONFIG_ZHAC_EZSP_UART_RST_GPIO;
#else
static constexpr gpio_num_t  EZSP_PIN_TX     = GPIO_NUM_26;
static constexpr gpio_num_t  EZSP_PIN_RX     = GPIO_NUM_27;
static constexpr gpio_num_t  EZSP_PIN_NRESET = GPIO_NUM_28;
#endif

static constexpr uart_port_t EZSP_UART    = UART_NUM_1;
static constexpr int         UART_BUF_SZ  = 4096;

// ── ASH state ────────────────────────────────────────────────────────────
static uint8_t s_tx_frame_num = 0;   // DATA frame counter (0-7)
static uint8_t s_rx_ack_num   = 0;   // expected ACK number from NCP
static uint8_t s_ezsp_seq     = 0;   // EZSP sequence counter

// ── Callback registry ────────────────────────────────────────────────────
static constexpr size_t MAX_EZSP_HANDLERS = 24;
struct EzspCbEntry {
    uint16_t       command_id;
    EzspCallbackFn cb;
};
static EzspCbEntry s_cbs[MAX_EZSP_HANDLERS];
static size_t      s_cb_count = 0;

// ── SREQ/response synchronisation ────────────────────────────────────────
static SemaphoreHandle_t s_sreq_mutex = nullptr;
static SemaphoreHandle_t s_rsp_sem    = nullptr;
static uint16_t          s_rsp_cmd_id = 0;
static uint8_t           s_rsp_buf[EZSP_MAX_PAYLOAD + 16];
static EzspFrame         s_rsp_frame;
static portMUX_TYPE      s_sreq_mux   = portMUX_INITIALIZER_UNLOCKED;

// ── ASH RSTACK detection ─────────────────────────────────────────────────
static SemaphoreHandle_t s_rstack_sem = nullptr;

// ── RX state machine ─────────────────────────────────────────────────────
static uint8_t s_rx_raw[ASH_MAX_FRAME * 2];
static uint8_t s_rx_frame[ASH_MAX_FRAME];
static size_t  s_rx_frame_len = 0;
static bool    s_rx_escape    = false;

// ══════════════════════════════════════════════════════════════════════════
// ASH CRC16-CCITT
// ══════════════════════════════════════════════════════════════════════════

uint16_t ash_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i]) << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

// ══════════════════════════════════════════════════════════════════════════
// ASH byte stuffing
// ══════════════════════════════════════════════════════════════════════════

static bool ash_needs_escape(uint8_t b) {
    return b == ASH_FLAG || b == ASH_ESCAPE || b == ASH_XON ||
           b == ASH_XOFF || b == ASH_SUB || b == ASH_CANCEL;
}

static size_t ash_stuff_byte(uint8_t b, uint8_t* out, size_t pos, size_t cap) {
    if (ash_needs_escape(b)) {
        if (pos + 2 > cap) return 0;
        out[pos]     = ASH_ESCAPE;
        out[pos + 1] = b ^ 0x20;
        return 2;
    }
    if (pos + 1 > cap) return 0;
    out[pos] = b;
    return 1;
}

// ══════════════════════════════════════════════════════════════════════════
// ASH DATA frame randomization (LFSR, init=0x42, poly=0xB8)
// Applied to all bytes except the control byte (first byte).
// ══════════════════════════════════════════════════════════════════════════

static void ash_randomize(uint8_t* data, size_t len) {
    uint8_t rand = 0x42;
    for (size_t i = 0; i < len; i++) {
        data[i] ^= rand;
        if (rand & 1)
            rand = (rand >> 1) ^ 0xB8;
        else
            rand >>= 1;
    }
}

static void ash_derandomize(uint8_t* data, size_t len) {
    ash_randomize(data, len);  // XOR is its own inverse
}

// ══════════════════════════════════════════════════════════════════════════
// ASH frame encoding
// ══════════════════════════════════════════════════════════════════════════

size_t ash_encode_data(uint8_t frame_num, uint8_t ack_num, bool retransmit,
                       const uint8_t* ezsp_data, size_t ezsp_len,
                       uint8_t* out, size_t out_cap) {
    // DATA control byte: bit7=0, bits[6:4]=frameNum, bit3=retransmit, bits[2:0]=ackNum
    uint8_t ctrl = ((frame_num & 0x07) << 4) | (retransmit ? 0x08 : 0x00) | (ack_num & 0x07);

    // Build unescaped frame: [ctrl] [ezsp_data (randomized)] [CRC16 MSB] [CRC16 LSB]
    uint8_t raw[EZSP_MAX_PAYLOAD + 4];
    raw[0] = ctrl;
    memcpy(raw + 1, ezsp_data, ezsp_len);
    // Randomize all bytes after control byte
    ash_randomize(raw + 1, ezsp_len);

    // CRC over control byte + randomized data
    uint16_t crc = ash_crc16(raw, 1 + ezsp_len);
    raw[1 + ezsp_len]     = (crc >> 8) & 0xFF;  // MSB
    raw[1 + ezsp_len + 1] = crc & 0xFF;          // LSB
    size_t raw_len = 1 + ezsp_len + 2;

    // Byte-stuff into output
    size_t pos = 0;
    for (size_t i = 0; i < raw_len; i++) {
        size_t n = ash_stuff_byte(raw[i], out, pos, out_cap - 1);
        if (n == 0) return 0;
        pos += n;
    }
    if (pos >= out_cap) return 0;
    out[pos++] = ASH_FLAG;  // frame delimiter
    return pos;
}

size_t ash_encode_ack(uint8_t ack_num, uint8_t* out, size_t out_cap) {
    // ACK control byte: 0b100_0_0_ackNum = 0x80 | (ack_num & 0x07)
    uint8_t raw[3];
    raw[0] = 0x80 | (ack_num & 0x07);
    uint16_t crc = ash_crc16(raw, 1);
    raw[1] = (crc >> 8) & 0xFF;
    raw[2] = crc & 0xFF;

    size_t pos = 0;
    for (size_t i = 0; i < 3; i++) {
        size_t n = ash_stuff_byte(raw[i], out, pos, out_cap - 1);
        if (n == 0) return 0;
        pos += n;
    }
    if (pos >= out_cap) return 0;
    out[pos++] = ASH_FLAG;
    return pos;
}

size_t ash_encode_rst(uint8_t* out, size_t out_cap) {
    // RST control byte: 0xC0
    uint8_t raw[3];
    raw[0] = 0xC0;
    uint16_t crc = ash_crc16(raw, 1);
    raw[1] = (crc >> 8) & 0xFF;
    raw[2] = crc & 0xFF;

    size_t pos = 0;
    for (size_t i = 0; i < 3; i++) {
        size_t n = ash_stuff_byte(raw[i], out, pos, out_cap - 1);
        if (n == 0) return 0;
        pos += n;
    }
    if (pos >= out_cap) return 0;
    out[pos++] = ASH_FLAG;
    return pos;
}

// ══════════════════════════════════════════════════════════════════════════
// UART I/O
// ══════════════════════════════════════════════════════════════════════════

static void uart_send(const uint8_t* data, size_t len) {
    uart_write_bytes(EZSP_UART, data, len);
}

// ══════════════════════════════════════════════════════════════════════════
// ASH frame RX and dispatch
// ══════════════════════════════════════════════════════════════════════════

static void dispatch_ezsp_frame(const uint8_t* ezsp_data, size_t ezsp_len) {
    if (ezsp_len < 5) return;  // seq(1) + frame_control(2) + command_id(2)

    EzspFrame f{};
    f.sequence      = ezsp_data[0];
    f.frame_control = (uint16_t)ezsp_data[1] | ((uint16_t)ezsp_data[2] << 8);
    f.command_id    = (uint16_t)ezsp_data[3] | ((uint16_t)ezsp_data[4] << 8);
    f.payload       = ezsp_data + 5;
    f.payload_len   = (uint8_t)(ezsp_len - 5);

    // Check if this is a response to a pending SREQ
    portENTER_CRITICAL(&s_sreq_mux);
    if (f.command_id == s_rsp_cmd_id) {
        // Copy to stable buffer
        memcpy(s_rsp_buf, ezsp_data, ezsp_len);
        s_rsp_frame.sequence      = s_rsp_buf[0];
        s_rsp_frame.frame_control = (uint16_t)s_rsp_buf[1] | ((uint16_t)s_rsp_buf[2] << 8);
        s_rsp_frame.command_id    = (uint16_t)s_rsp_buf[3] | ((uint16_t)s_rsp_buf[4] << 8);
        s_rsp_frame.payload       = s_rsp_buf + 5;
        s_rsp_frame.payload_len   = (uint8_t)(ezsp_len - 5);
        xSemaphoreGive(s_rsp_sem);
        portEXIT_CRITICAL(&s_sreq_mux);
        return;
    }
    portEXIT_CRITICAL(&s_sreq_mux);

    // Dispatch to registered callbacks
    for (size_t i = 0; i < s_cb_count; i++) {
        if (s_cbs[i].command_id == f.command_id) {
            s_cbs[i].cb(f);
        }
    }
}

static void process_ash_frame(const uint8_t* frame, size_t len) {
    if (len < 3) return;  // minimum: control(1) + CRC(2)

    // Verify CRC
    uint16_t expected_crc = ash_crc16(frame, len - 2);
    uint16_t received_crc = ((uint16_t)frame[len - 2] << 8) | frame[len - 1];
    if (expected_crc != received_crc) {
        ESP_LOGW(TAG, "ASH CRC error: expected=0x%04x got=0x%04x", expected_crc, received_crc);
        return;
    }

    uint8_t ctrl = frame[0];

    if ((ctrl & 0x80) == 0) {
        // DATA frame: bit7=0
        uint8_t frame_num = (ctrl >> 4) & 0x07;
        uint8_t ack_num   = ctrl & 0x07;
        (void)frame_num;
        s_rx_ack_num = ack_num;

        // Derandomize payload (everything after control, before CRC)
        size_t data_len = len - 3;  // minus ctrl(1) + CRC(2)
        uint8_t data[EZSP_MAX_PAYLOAD + 8];
        if (data_len > sizeof(data)) return;
        memcpy(data, frame + 1, data_len);
        ash_derandomize(data, data_len);

        // Send ACK for this frame
        uint8_t ack_buf[8];
        uint8_t next_frame = (frame_num + 1) & 0x07;
        size_t ack_len = ash_encode_ack(next_frame, ack_buf, sizeof(ack_buf));
        if (ack_len > 0) uart_send(ack_buf, ack_len);

        // Dispatch EZSP content
        dispatch_ezsp_frame(data, data_len);

    } else if ((ctrl & 0xE0) == 0x80) {
        // ACK frame: bits[7:5]=100
        uint8_t ack_num = ctrl & 0x07;
        s_rx_ack_num = ack_num;

    } else if ((ctrl & 0xE0) == 0xA0) {
        // NAK frame: bits[7:5]=101
        ESP_LOGW(TAG, "ASH NAK received — retransmit needed");

    } else if (ctrl == 0xC0) {
        // RST frame
        ESP_LOGI(TAG, "ASH RST received");

    } else if (ctrl == 0xC1) {
        // RSTACK frame — NCP acknowledged our reset
        ESP_LOGI(TAG, "ASH RSTACK received");
        s_tx_frame_num = 0;
        s_rx_ack_num   = 0;
        if (s_rstack_sem) xSemaphoreGive(s_rstack_sem);

    } else if (ctrl == 0xC2) {
        // ERROR frame
        ESP_LOGE(TAG, "ASH ERROR received");
    }
}

// ══════════════════════════════════════════════════════════════════════════
// Public API
// ══════════════════════════════════════════════════════════════════════════

void ezsp_driver_init() {
    uart_config_t cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk          = UART_SCLK_DEFAULT,
        .flags               = {},
    };
    ESP_ERROR_CHECK(uart_param_config(EZSP_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(EZSP_UART, EZSP_PIN_TX, EZSP_PIN_RX,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(EZSP_UART, UART_BUF_SZ, 0, 0,
                                        nullptr, 0));

    gpio_config_t gc = {
        .pin_bit_mask = (1ULL << EZSP_PIN_NRESET),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
#if SOC_GPIO_SUPPORT_PIN_HYS_FILTER
        .hys_ctrl_mode = GPIO_HYS_SOFT_DISABLE,
#endif
    };
    ESP_ERROR_CHECK(gpio_config(&gc));
    gpio_set_level(EZSP_PIN_NRESET, 1);

    s_sreq_mutex = xSemaphoreCreateMutex();
    configASSERT(s_sreq_mutex);
    s_rsp_sem = xSemaphoreCreateBinary();
    configASSERT(s_rsp_sem);
    s_rstack_sem = xSemaphoreCreateBinary();
    configASSERT(s_rstack_sem);

    s_tx_frame_num = 0;
    s_rx_ack_num   = 0;
    s_ezsp_seq     = 0;
    s_rx_frame_len = 0;
    s_rx_escape    = false;

    ESP_LOGI(TAG, "ezsp_driver init OK — UART%d 115200 baud, nRST GPIO%d",
             EZSP_UART, EZSP_PIN_NRESET);
}

void ezsp_driver_poll() {
    int avail = uart_read_bytes(EZSP_UART, s_rx_raw, sizeof(s_rx_raw),
                                 pdMS_TO_TICKS(20));
    if (avail <= 0) return;

    for (int i = 0; i < avail; i++) {
        uint8_t b = s_rx_raw[i];

        if (b == ASH_CANCEL) {
            s_rx_frame_len = 0;
            s_rx_escape = false;
            continue;
        }
        if (b == ASH_FLAG) {
            if (s_rx_frame_len > 0) {
                process_ash_frame(s_rx_frame, s_rx_frame_len);
            }
            s_rx_frame_len = 0;
            s_rx_escape = false;
            continue;
        }
        if (b == ASH_ESCAPE) {
            s_rx_escape = true;
            continue;
        }
        if (s_rx_escape) {
            b ^= 0x20;
            s_rx_escape = false;
        }
        if (s_rx_frame_len < sizeof(s_rx_frame)) {
            s_rx_frame[s_rx_frame_len++] = b;
        }
    }
}

void ezsp_hw_reset() {
    gpio_set_level(EZSP_PIN_NRESET, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(EZSP_PIN_NRESET, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "EFR32 hardware reset");
}

bool ezsp_ash_reset(uint32_t timeout_ms) {
    // Send RST and wait for RSTACK
    xSemaphoreTake(s_rstack_sem, 0);  // clear stale
    uint8_t rst_buf[8];
    size_t rst_len = ash_encode_rst(rst_buf, sizeof(rst_buf));
    if (rst_len == 0) return false;
    uart_send(rst_buf, rst_len);

    if (xSemaphoreTake(s_rstack_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "ASH RSTACK timeout");
        return false;
    }
    s_tx_frame_num = 0;
    s_rx_ack_num   = 0;
    s_ezsp_seq     = 0;
    ESP_LOGI(TAG, "ASH reset complete — link ready");
    return true;
}

EzspFrame ezsp_make_req(uint16_t command_id, const uint8_t* payload,
                        uint8_t payload_len) {
    EzspFrame f{};
    f.sequence      = s_ezsp_seq++;
    f.frame_control = 0x0000;
    f.command_id    = command_id;
    f.payload       = payload;
    f.payload_len   = payload_len;
    return f;
}

bool ezsp_sreq(const EzspFrame& req, EzspFrame& rsp_out, uint32_t timeout_ms) {
    // Build EZSP frame bytes: seq(1) + frame_control(2) + command_id(2) + payload
    uint8_t ezsp_buf[EZSP_MAX_PAYLOAD + 8];
    ezsp_buf[0] = req.sequence;
    ezsp_buf[1] = req.frame_control & 0xFF;
    ezsp_buf[2] = (req.frame_control >> 8) & 0xFF;
    ezsp_buf[3] = req.command_id & 0xFF;
    ezsp_buf[4] = (req.command_id >> 8) & 0xFF;
    if (req.payload_len > 0 && req.payload)
        memcpy(ezsp_buf + 5, req.payload, req.payload_len);
    size_t ezsp_len = 5 + req.payload_len;

    // Take mutex for exclusive UART access
    if (xSemaphoreTake(s_sreq_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "EZSP SREQ mutex timeout cmd=0x%04x", req.command_id);
        return false;
    }

    portENTER_CRITICAL(&s_sreq_mux);
    s_rsp_cmd_id = req.command_id;
    xSemaphoreTake(s_rsp_sem, 0);  // clear stale
    portEXIT_CRITICAL(&s_sreq_mux);

    // Encode ASH DATA frame and send
    uint8_t ash_buf[ASH_MAX_FRAME];
    size_t ash_len = ash_encode_data(s_tx_frame_num & 0x07, s_rx_ack_num & 0x07,
                                     false, ezsp_buf, ezsp_len,
                                     ash_buf, sizeof(ash_buf));
    s_tx_frame_num = (s_tx_frame_num + 1) & 0x07;

    if (ash_len == 0) {
        xSemaphoreGive(s_sreq_mutex);
        return false;
    }
    uart_send(ash_buf, ash_len);

    bool ok = (xSemaphoreTake(s_rsp_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
    if (!ok) {
        ESP_LOGE(TAG, "EZSP response timeout cmd=0x%04x", req.command_id);
    } else {
        rsp_out = s_rsp_frame;
    }
    xSemaphoreGive(s_sreq_mutex);
    return ok;
}

bool ezsp_sreq_retry(const EzspFrame& req, EzspFrame& rsp_out,
                     uint32_t timeout_ms, int max_attempts) {
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        if (ezsp_sreq(req, rsp_out, timeout_ms)) return true;
        ESP_LOGW(TAG, "EZSP attempt %d/%d failed cmd=0x%04x",
                 attempt, max_attempts, req.command_id);
    }
    return false;
}

void ezsp_register_callback(uint16_t command_id, EzspCallbackFn cb) {
    for (size_t i = 0; i < s_cb_count; i++) {
        if (s_cbs[i].command_id == command_id) {
            s_cbs[i].cb = std::move(cb);
            return;
        }
    }
    if (s_cb_count >= MAX_EZSP_HANDLERS) {
        ESP_LOGE(TAG, "EZSP callback table full");
        return;
    }
    s_cbs[s_cb_count++] = {command_id, std::move(cb)};
}
