// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host shim for zhac_task.h. The LU-01 test never starts TaskLua, so create is
// a no-op that reports success without running the entry function.
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
static inline QueueHandle_t zhac_queue_create(UBaseType_t len, UBaseType_t item) { return xQueueCreate(len, item); }
static inline void          zhac_queue_delete(QueueHandle_t q) { vQueueDelete(q); }
static inline BaseType_t    zhac_task_create(TaskFunction_t, const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*) { return pdPASS; }
static inline BaseType_t    zhac_task_create_pinned(TaskFunction_t, const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*, BaseType_t) { return pdPASS; }
