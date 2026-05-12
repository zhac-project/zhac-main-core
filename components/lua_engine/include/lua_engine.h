// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Lua scripting engine — the project's single scripting runtime on
// P4. One FreeRTOS task (TaskLua) services a resume queue; each
// script runs as one or more coroutines. See
// docs/plans/2026-04-21-lua-engine-plan.md.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring the VM up: custom allocator, sandbox, native modules, TaskLua.
// Idempotent. Must be called after nvs_flash_init() so the script
// cache backing store is available.
bool lua_engine_init(void);

// Scan `/spiffs/scripts/*.lua` (or the NVS-backed alternative) and
// spawn a top-level coroutine for each. Registrations (on_attr_change,
// on_mqtt, on_cron, on_boot) are recorded here so subsequent EventBus
// events can dispatch into them.
void lua_engine_load_all(void);

// Enqueue a resume for any coroutines subscribed to `event`. Non-
// blocking. Called from EventBus subscribers.
struct Event;
void lua_engine_dispatch(const struct Event* ev);

// Observability helpers — safe to call from any task.
size_t   lua_engine_heap_used_bytes(void);
size_t   lua_engine_heap_peak_bytes(void);
uint16_t lua_engine_live_coroutines(void);
uint32_t lua_engine_error_count(void);
uint32_t lua_engine_yield_count(void);

// Syntax-check a Lua source buffer. Creates a throwaway lua_State,
// invokes luaL_loadbufferx with mode "t" (text only — no bytecode),
// captures the error string. Returns true on a clean parse.
//
// On failure: err_out is filled (NUL-terminated, truncated to fit),
// line_out receives the 1-based line number extracted from the
// compiler's "[string \"NAME\"]:LINE: …" message (or 1 when the
// message has no line marker). Safe to call off TaskLua.
bool lua_engine_check_syntax(const char* name, const char* src,
                              char* err_out, size_t err_cap,
                              int*   line_out);

#ifdef __cplusplus
}
#endif
