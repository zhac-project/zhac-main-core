// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// lua-cjson's part of the Lua C-stack limits (final fix wave I-2(a); the rest and the arithmetic are in
// lua_limits.cmake). cjson.encode / decode recurse in C, outside Lua's own counter: encode about 144 B a level,
// decode 96 B, and upstream allows 1,000 levels, which no TaskLua stack holds. Shared with test/host.

#include "lua.h"

#define LUA_ENGINE_CJSON_MAX_DEPTH 16

// On the cjson module table at the top of L's stack: encode and decode nesting capped at
// LUA_ENGINE_CJSON_MAX_DEPTH, and a script can no longer raise it. The two setters go, and so does cjson.new(),
// which would open a fresh instance with the upstream defaults.
static inline void lua_engine_cjson_limit(lua_State* L) {
    static const char* const kSetters[] = {"encode_max_depth", "decode_max_depth"};
    for (int i = 0; i < 2; i++) {
        lua_getfield(L, -1, kSetters[i]);
        if (lua_isfunction(L, -1)) {
            lua_pushinteger(L, LUA_ENGINE_CJSON_MAX_DEPTH);
            lua_call(L, 1, 0);
        } else {
            lua_pop(L, 1);
        }
        lua_pushnil(L);
        lua_setfield(L, -2, kSetters[i]);
    }
    lua_pushnil(L);
    lua_setfield(L, -2, "new");
}
