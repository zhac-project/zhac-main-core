// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host stand-in: logging is dropped (arguments still type-checked). C and C++.
#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); if (0) printf(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); if (0) printf(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); if (0) printf(fmt, ##__VA_ARGS__); } while (0)
