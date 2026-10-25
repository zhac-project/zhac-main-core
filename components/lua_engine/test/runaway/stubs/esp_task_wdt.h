// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_err.h"
static inline esp_err_t esp_task_wdt_add(void*) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
