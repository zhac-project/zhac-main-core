// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// `require` loads every module the store accepts (review F28): the store
// takes scripts up to LUA_SCRIPT_SRC_MAX (16 KB), the loader read into 8 KB.
extern "C" {
#include "lauxlib.h"
#include "lualib.h"
#include "lua_internal.h"
}
#include "lua_engine_scripts.h"

#include <cstdio>
#include <cstdlib>
#include <string>

// The real `zhac` module needs the whole hub; require only preloads it.
extern "C" int luaopen_zhac(lua_State* L) { lua_newtable(L); return 1; }

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

// A module of exactly `size` bytes that returns `size`.
static std::string module(size_t size) {
    std::string body = "return " + std::to_string(size) + "\n";
    std::string pad = "-- ";
    pad.append(size - body.size() - pad.size() - 1, 'x');
    return pad + "\n" + body;
}

static bool loads(lua_State* L, const char* name, size_t size) {
    const std::string m = module(size);
    if (m.size() != size || !lua_script_cache_write(name, m.c_str())) return false;
    lua_getglobal(L, "require");
    lua_pushstring(L, name);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) { std::printf("     %s\n", lua_tostring(L, -1)); lua_pop(L, 1); return false; }
    const bool ok = lua_tointeger(L, -1) == (lua_Integer)size;
    lua_pop(L, 1);
    return ok;
}

int main() {
    std::system("rm -rf '" LUA_SCRIPT_BASE_PATH "' && mkdir -p '" LUA_SCRIPT_BASE_PATH "'");
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    expect("require installs", lua_engine_require_install(L));
    expect("a small module loads", loads(L, "small", 100));
    expect("an 8191-byte module loads", loads(L, "m8191", 8191));
    expect("an 8192-byte module loads", loads(L, "m8192", 8192));
    expect("a module at the store's limit loads", loads(L, "max", LUA_SCRIPT_SRC_MAX));
    lua_close(L);
    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nrequire OK\n", g_fail);
    return g_fail;
}
