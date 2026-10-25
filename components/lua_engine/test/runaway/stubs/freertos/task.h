// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "FreeRTOS.h"
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
static inline TickType_t xTaskGetTickCount(void) { return 0; }
static inline void vTaskDelay(TickType_t) {}
