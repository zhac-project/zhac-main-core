// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include "esp_err.h"

typedef void* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void*);
typedef enum { ESP_TIMER_TASK, ESP_TIMER_ISR } esp_timer_dispatch_t;

// Field order matches the scheduler's designated initialiser (C++20 requires it).
typedef struct {
    esp_timer_cb_t       callback;
    void*                arg;
    esp_timer_dispatch_t dispatch_method;
    const char*          name;
    bool                 skip_unhandled_events;
} esp_timer_create_args_t;

int64_t esp_timer_get_time(void);   // defined by the test

static inline esp_err_t esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t* out) {
    static int handle_obj;
    if (out) *out = &handle_obj;
    return ESP_OK;
}
static inline esp_err_t esp_timer_start_once(esp_timer_handle_t, uint64_t) { return ESP_OK; }
static inline esp_err_t esp_timer_stop(esp_timer_handle_t) { return ESP_OK; }
