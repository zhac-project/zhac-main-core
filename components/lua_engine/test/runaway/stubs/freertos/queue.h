// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host shim for the FreeRTOS queue API. The LU-01 test drives resume_and_settle
// directly (never through the queue/task loop), so every op is a benign no-op.
#pragma once
#include "FreeRTOS.h"
typedef void* QueueHandle_t;
static inline QueueHandle_t xQueueCreate(UBaseType_t, UBaseType_t) {
    static int s_q;
    return &s_q;
}
static inline BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t) { return pdTRUE; }
static inline BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t) { return pdFALSE; }
static inline BaseType_t xQueueOverwrite(QueueHandle_t, const void*) { return pdTRUE; }
static inline void       vQueueDelete(QueueHandle_t) {}
