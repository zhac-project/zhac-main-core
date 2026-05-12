// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// Private header — exposes the allocator entry point to lua_engine.c
// so we can pass it to lua_newstate.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* lua_engine_alloc(void* ud, void* ptr, size_t osize, size_t nsize);

// Accessors — defined alongside the allocator. Public API declares
// matching symbols in lua_engine.h; they forward here.
size_t lua_engine_heap_used_bytes(void);
size_t lua_engine_heap_peak_bytes(void);

#ifdef __cplusplus
}
#endif
