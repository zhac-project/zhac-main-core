// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// Component-private scheduler API.

#include <stdbool.h>
#include <stdint.h>

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

// Schedule a sleep for the coroutine identified by `ref`. Returns
// false if the timer pool is saturated.
bool lua_scheduler_sleep(int coroutine_ref, uint32_t delay_ms);

// Push a resume event for an already-registered coroutine ref.
// ISR-safe. Returns false when the queue is full.
bool lua_scheduler_push_resume(int coroutine_ref);

// Queue an event dispatch. TaskLua walks the REG_ON_* table for the
// given `event_kind` (0=boot, 1=attr_change, 2=mqtt, 3=zcl_raw) and
// spawns one coroutine per registered handler, passing payload fields
// as args.
// `payload_96` is a 96-byte buffer matching the EventBus `Event::data`
// layout for the corresponding event type. Returns false when the
// queue is full (dropped-on-overflow semantics).
bool lua_scheduler_push_event(uint8_t event_kind, const void* payload_96);

// Trigger context handed to scripts invoked via `DO script.run "..."`.
// Mirrors `SimpleRulesScriptEvent` without taking a simple_rules
// dependency at the scheduler layer. Any field may be empty/zero when
// the trigger isn't a device-attr event.
typedef struct {
    const char* key;       // attr name, or "" for non-attr triggers
    const char* value;     // stringified trigger value (legacy arg)
    uint64_t    ieee;      // source device IEEE, 0 when not device-triggered
    uint16_t    cluster;   // ZCL cluster id, 0 for non-attr triggers
    uint16_t    attr_id;   // ZCL attribute id, 0 for non-attr triggers
    uint8_t     val_type;  // ValType enum
    int32_t     int_val;   // raw int (VAL_INT/BOOL)
    const char* str_val;   // raw string (VAL_STR), or ""
} LuaScriptInvokeArgs;

// Enqueue an on-demand execution of a named script from the SPIFFS
// cache. Used by the simple_rules `DO script.run "<name>"` hook. The
// script is resumed with a single table arg carrying `args` verbatim
// (`value`, `key`, `ieee`, `cluster`, `attr_id`, `val_type`, `int_val`,
// `str_val`). `args` may be NULL for pure trigger fire-and-forget with
// no context.
bool lua_scheduler_push_run_named(const char* name,
                                    const LuaScriptInvokeArgs* args);

// Consume the function on top of `L` (the master state), wrap it in a
// new coroutine, register it, and enqueue an initial resume. Returns
// the registry ref or LUA_NOREF on OOM.
int lua_scheduler_spawn(lua_State* L);

#ifdef __cplusplus
}
#endif
