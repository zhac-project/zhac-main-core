// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Silent host log shim. The args are referenced inside `if (0)` so variables
// logged only at a given level aren't flagged unused under -Werror, but nothing
// is ever printed. Used by both the C sandbox TU and the C++ scheduler TU, so
// it stays C-compatible (<stdio.h>, not <cstdio>).
#pragma once
#include <stdio.h>
typedef enum {
    ESP_LOG_NONE = 0, ESP_LOG_ERROR, ESP_LOG_WARN,
    ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE,
} esp_log_level_t;
#define ZHAC_LOG_SINK(tag, fmt, ...) do { if (0) { (void)(tag); printf(fmt, ##__VA_ARGS__); } } while (0)
#define ESP_LOG_LEVEL(level, tag, fmt, ...) do { (void)(level); ZHAC_LOG_SINK(tag, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) ZHAC_LOG_SINK(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ZHAC_LOG_SINK(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ZHAC_LOG_SINK(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ZHAC_LOG_SINK(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ZHAC_LOG_SINK(tag, fmt, ##__VA_ARGS__)
