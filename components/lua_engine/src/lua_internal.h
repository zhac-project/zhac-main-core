// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// Component-private shared header. All Lua-facing source files pull
// this; everything else stays inside lua_engine.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#ifdef __cplusplus
extern "C" {
#endif

// The single master state. Created once in lua_engine_init().
extern lua_State* g_L;

// Kick-off helpers invoked by lua_engine_init(). Each returns false
// and logs on failure — init() short-circuits.
bool lua_engine_sandbox_apply(lua_State* L);
bool lua_engine_require_install(lua_State* L);
bool lua_engine_scheduler_start(lua_State* L);
void lua_engine_scheduler_stop(void);

// Native module loader entry points — called by lua_engine_require_install
// to preload C modules before any `require` runs.
int luaopen_zhac(lua_State* L);

// Coroutine tracking — the scheduler keeps a count so the metrics
// engine can publish `lua_coroutines_live` without walking state.
void     lua_scheduler_coroutine_enter(void);
void     lua_scheduler_coroutine_exit(void);
uint16_t lua_scheduler_live_count(void);
uint32_t lua_scheduler_error_count(void);
uint32_t lua_scheduler_yield_count(void);
void     lua_scheduler_error_bump(void);
void     lua_scheduler_yield_bump(void);

#ifdef __cplusplus
}
#endif
