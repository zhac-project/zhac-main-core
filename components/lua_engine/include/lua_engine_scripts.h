// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// SPIFFS-backed storage for Lua scripts at `/scripts/<name>.lua`
// (partition label `scripts`). Names up to LUA_SCRIPT_NAME_MAX chars
// (alphanum + `_` + `-`); source ≤ 16 KB.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LUA_SCRIPT_NAME_MAX 24   // chars, excluding NUL terminator
#define LUA_SCRIPT_MAX      16   // soft cap on distinct files

typedef struct {
    char     name[LUA_SCRIPT_NAME_MAX + 1];
    uint16_t size;
} LuaScriptEntry;

// List entries into caller-provided array. Returns count written.
uint16_t lua_script_cache_list(LuaScriptEntry* out, uint16_t max);

// Read the UTF-8 source into `out`; returns bytes written or -1 on
// miss / truncation. `cap` includes room for the NUL terminator.
int lua_script_cache_read(const char* name, char* out, size_t cap);

// Create / overwrite. `src` is a NUL-terminated Lua source buffer.
// Returns false on invalid name or NVS failure.
bool lua_script_cache_write(const char* name, const char* src);

// Delete. Returns true even if the slot was empty.
bool lua_script_cache_delete(const char* name);

bool lua_script_cache_exists(const char* name);

// Enqueue a fresh coroutine invocation of the named script with an
// empty trigger context. Used by the S3-side UI "Run" button via HAP.
// Returns false when the script isn't present or the scheduler queue
// is saturated.
bool lua_engine_run_script(const char* name);

#ifdef __cplusplus
}
#endif
