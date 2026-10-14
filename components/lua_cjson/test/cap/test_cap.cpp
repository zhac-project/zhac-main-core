// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// cjson.encode's 64 KB output limit holds for a single value too (review F20):
// it was checked before each value, so one 70000-byte string went through,
// after reserving 6x its length as scratch outside the Lua memory budget.
extern "C" {
#include "lauxlib.h"
#include "lualib.h"
int luaopen_cjson(lua_State* l);
}
#include <cstdio>

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

// true when `expr` (a Lua expression using cjson) evaluates without error.
static bool runs(lua_State* L, const char* expr) {
    char chunk[256];
    std::snprintf(chunk, sizeof(chunk), "local cjson = ... ; return %s", expr);
    if (luaL_loadstring(L, chunk) != LUA_OK) return false;
    lua_pushvalue(L, 1);                      // the cjson module
    const bool ok = lua_pcall(L, 1, 1, 0) == LUA_OK;
    lua_pop(L, 1);
    return ok;
}

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    luaopen_cjson(L);                          // stack[1] = module
    expect("a 60000-byte string encodes", runs(L, "#cjson.encode(string.rep('a', 60000)) == 60002 or error()"));
    expect("a 70000-byte string is refused", !runs(L, "cjson.encode(string.rep('a', 70000))"));
    expect("a string whose escapes pass 64 KB is refused", !runs(L, "cjson.encode(string.rep('\\1', 11000))"));
    expect("escapes under 64 KB encode", runs(L, "#cjson.encode(string.rep('\"', 20000)) == 40002 or error()"));
    expect("a huge key is refused", !runs(L, "cjson.encode({[string.rep('k', 70000)] = 1})"));
    expect("a table of strings past 64 KB is refused",
           !runs(L, "cjson.encode({string.rep('a', 40000), string.rep('b', 40000)})"));
    expect("cjson still works after a refusal", runs(L, "cjson.encode({a = 1}) == '{\"a\":1}' or error()"));
    lua_close(L);
    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\ncjson cap OK\n", g_fail);
    return g_fail;
}
