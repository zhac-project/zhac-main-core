// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// lua_sandbox.c — redact the unsafe bits of the standard library.
//
// After luaL_openlibs(), several functions provide arbitrary file or
// process access. Strip those before the VM services any script. We
// don't drop whole modules — `os.time` is kept — just point the
// dangerous entries at the no-op `nil`.

#include "sdkconfig.h"

#if CONFIG_LUA_ENGINE_ENABLED

#include <stdarg.h>
#include <string.h>

#include "esp_log.h"

#include "lua_internal.h"

static const char* TAG = "lua_sandbox";

// Null out `<table>.<name>` by setting to Lua nil.
static void redact(lua_State* L, const char* tbl, const char* name) {
    lua_getglobal(L, tbl);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_pushnil(L);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

// `print(...)` → ESP_LOGI("lua", "...")
static int sandboxed_print(lua_State* L) {
    const int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addchar(&b, '\t');
        size_t len;
        const char* s = luaL_tolstring(L, i, &len);   // pushes string
        luaL_addlstring(&b, s, len);
        lua_pop(L, 1);
    }
    luaL_pushresult(&b);
    const char* out = lua_tostring(L, -1);
    ESP_LOGI("lua", "%s", out ? out : "(nil)");
    lua_pop(L, 1);
    return 0;
}

bool lua_engine_sandbox_apply(lua_State* L) {
    // io.* — entire module nuked.
    lua_pushnil(L);
    lua_setglobal(L, "io");

    // os.* — keep time/clock/date/difftime; remove shell escapes.
    redact(L, "os", "execute");
    redact(L, "os", "exit");
    redact(L, "os", "getenv");
    redact(L, "os", "remove");
    redact(L, "os", "rename");
    redact(L, "os", "tmpname");
    redact(L, "os", "setlocale");

    // package.* — drop filesystem loaders.
    redact(L, "package", "loadlib");
    redact(L, "package", "searchpath");
    redact(L, "package", "cpath");
    redact(L, "package", "path");

    // Top-level loaders. `load` / `loadstring` let a script compile
    // arbitrary bytes — Lua accepts bytecode by default, so the
    // mode="t" text-only gate the engine uses internally is bypassed
    // the moment a user script calls load() directly. Nil them all.
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "load");
    lua_pushnil(L); lua_setglobal(L, "loadstring");

    // `string.dump` pairs with `load()` to round-trip bytecode; drop
    // it so a script can't emit bytes the sandbox has refused to re-eat.
    redact(L, "string", "dump");

    // `collectgarbage` can stall the single-task Lua VM for tens of ms
    // on a big heap — a tight loop DoSes every coroutine on TaskLua.
    // The engine still drives gc via the C API (lua_gc).
    lua_pushnil(L); lua_setglobal(L, "collectgarbage");

    // debug.* — keep only traceback for error formatting.
    lua_getglobal(L, "debug");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "traceback");
        lua_pop(L, 1);   // keep traceback on the table (untouched)
        const char* drop[] = {
            "debug", "gethook", "getinfo", "getlocal", "getregistry",
            "getupvalue", "getuservalue", "sethook", "setlocal",
            "setmetatable", "setupvalue", "setuservalue",
            "upvaluejoin", "upvalueid",
        };
        for (size_t i = 0; i < sizeof(drop) / sizeof(drop[0]); ++i) {
            lua_pushnil(L);
            lua_setfield(L, -2, drop[i]);
        }
    }
    lua_pop(L, 1);

    // Redirect print.
    lua_pushcfunction(L, sandboxed_print);
    lua_setglobal(L, "print");

    // LUA-F10: lock stdlib table metatables. Defense in depth — every
    // public Lua sandbox CVE chain starts with metatable manipulation
    // (rewriting `string.__index`, etc.). Setting a string-typed
    // `__metatable` makes `getmetatable(t)` return that string and
    // `setmetatable(t, …)` raise an error. The base library's
    // `setmetatable` is unchanged for user tables; only the stdlib
    // surfaces are locked.
    static const char* kLockTables[] = {
        "string", "table", "math", "os", "coroutine", "io",
    };
    for (size_t i = 0; i < sizeof(kLockTables) / sizeof(kLockTables[0]); ++i) {
        lua_getglobal(L, kLockTables[i]);
        if (lua_istable(L, -1)) {
            lua_newtable(L);                       // mt
            lua_pushliteral(L, "locked");
            lua_setfield(L, -2, "__metatable");
            lua_setmetatable(L, -2);
        }
        lua_pop(L, 1);
    }

    ESP_LOGI(TAG, "sandbox applied");
    return true;
}

#endif
