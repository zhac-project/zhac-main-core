// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: Apache-2.0
#include "sdkconfig.h"
#ifdef CONFIG_HAP_BENCHMARK
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hap_protocol.h"
#include "hap_session.h"

static const char* TAG = "hap_bench";

void task_hap_bench(void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));   // let normal startup settle
    ESP_LOGI(TAG, "starting throughput benchmark — 1000 x 16 B frames");

    constexpr int N = 1000;
    uint8_t junk[16] = {};
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < N; i++) {
        HapFrame f{};
        f.type        = HapMsgType::HEARTBEAT;
        f.seq         = hap_session_next_seq();
        f.flags       = HAP_FLAG_NO_ACK;
        f.payload     = junk;
        f.payload_len = sizeof(junk);
        hap_session_send(f);
    }
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "1000x 16 B frames in %lld us = %.1f frames/s",
             (long long)(t1 - t0), 1e6 * N / (double)(t1 - t0));
    vTaskDelete(nullptr);
}
#endif
