// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// lua_require.c — custom require() resolver.
//
// Three search levels:
//   1. Preloaded native C modules (zhac, cjson, lpeg, miniz)
//   2. NVS-backed script cache (lua_script_cache_read)
//   3. Fail with `module not found`.
//
// Loaded modules cache into package.loaded per Lua's normal rules.

#include "sdkconfig.h"

#if CONFIG_LUA_ENGINE_ENABLED

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "lua_engine_scripts.h"
#include "lua_internal.h"

static const char* TAG = "lua_require";

// Step 2: look in the NVS script cache for an entry named `<name>`
// (no extension — cache keys are plain). Load and run the chunk,
// leave the module return value on the stack.
static int loader_script_cache(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);

    // Names in lua_script_cache don't carry the extension — `require
    // "util"` matches a stored entry named `util`.
    enum { kBufSz = 8192 };
    static char* buf = NULL;
    if (!buf) {
        buf = (char*)heap_caps_malloc(kBufSz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (char*)heap_caps_malloc(kBufSz, MALLOC_CAP_8BIT);
        if (!buf) {
            lua_pushliteral(L, "\n\tloader_script_cache: buf alloc failed");
            return 1;
        }
    }
    const int n = lua_script_cache_read(name, buf, kBufSz);
    if (n < 0) {
        lua_pushfstring(L, "\n\tno script cache entry '%s'", name);
        return 1;
    }

    // Compile as text only — no bytecode load allowed.
    if (luaL_loadbufferx(L, buf, (size_t)n, name, "t") != LUA_OK) {
        return lua_error(L);   // propagate compile error
    }
    lua_pushstring(L, name);   // module name as argument
    return 2;
}

// Externals provided by curated library components, wired behind
// Kconfig so a build without the sources still links. Each function
// opens its module into a new table on top of L, per Lua's C-module
// contract.
#if CONFIG_LUA_ENGINE_WITH_CJSON
extern int luaopen_cjson(lua_State* L);
#endif
#if CONFIG_LUA_ENGINE_WITH_LPEG
extern int luaopen_lpeg(lua_State* L);
#endif
#if CONFIG_LUA_ENGINE_WITH_MINIZ
extern int luaopen_miniz(lua_State* L);
#endif

bool lua_engine_require_install(lua_State* L) {
    // Preload native modules. `luaL_requiref(L, "zhac",
    // luaopen_zhac, 1)` pre-populates `package.loaded.zhac` so the
    // first `require "zhac"` returns it directly.
    luaL_requiref(L, "zhac", luaopen_zhac, /*glb=*/1);
    lua_pop(L, 1);

#if CONFIG_LUA_ENGINE_WITH_CJSON
    luaL_requiref(L, "cjson", luaopen_cjson, 1);
#if CONFIG_LUA_ENGINE_CJSON_DECODE_MAX_DEPTH > 0
    // The table from luaL_requiref is on top of the stack; look up
    // decode_max_depth on it and call with the cap.
    lua_getfield(L, -1, "decode_max_depth");
    if (lua_isfunction(L, -1)) {
        lua_pushinteger(L, CONFIG_LUA_ENGINE_CJSON_DECODE_MAX_DEPTH);
        lua_call(L, 1, 0);
    } else {
        lua_pop(L, 1);
    }
#endif
    lua_pop(L, 1);
    ESP_LOGI(TAG, "cjson preloaded");
#endif

#if CONFIG_LUA_ENGINE_WITH_LPEG
    luaL_requiref(L, "lpeg", luaopen_lpeg, 1);
    lua_pop(L, 1);
    ESP_LOGI(TAG, "lpeg preloaded");
#endif

#if CONFIG_LUA_ENGINE_WITH_MINIZ
    luaL_requiref(L, "miniz", luaopen_miniz, 1);
    lua_pop(L, 1);
    ESP_LOGI(TAG, "miniz preloaded");
#endif

    // Insert our script-cache searcher at index 2 — after the
    // preload searcher, before any remaining (now-nulled) ones.
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        ESP_LOGE(TAG, "package table missing");
        lua_pop(L, 1);
        return false;
    }
    lua_getfield(L, -1, "searchers");   // Lua 5.2+: table of searchers
    if (!lua_istable(L, -1)) {
        ESP_LOGE(TAG, "package.searchers missing");
        lua_pop(L, 2);
        return false;
    }
    // Shift existing searchers right.
    const lua_Integer len = luaL_len(L, -1);
    for (lua_Integer i = len; i >= 2; --i) {
        lua_geti(L, -1, i);
        lua_seti(L, -2, i + 1);
    }
    lua_pushcfunction(L, loader_script_cache);
    lua_seti(L, -2, 2);
    lua_pop(L, 2);   // pop searchers + package

    ESP_LOGI(TAG, "require resolver installed");
    return true;
}

#endif
