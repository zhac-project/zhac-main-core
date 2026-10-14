// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What an attribute-change handler receives for each value type. The producer
// tags values with zcl_attribute.h's ValType (INT=1, BOOL=2, STR=3, FLOAT=4);
// the scheduler once read 1 as boolean and 2 as string, so 42 arrived as true
// and false as an (truthy) empty string.
extern "C" {
#include "lauxlib.h"
#include "lualib.h"
}
#include "lua_attr_value.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;

static void check(lua_State* L, const char* what, uint8_t type, const uint8_t* val,
                  int want_type, const char* want) {
    lua_settop(L, 0);
    lua_push_attr_value(L, type, val);
    lua_getglobal(L, "tostring");
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    const bool ok = lua_type(L, 1) == want_type && std::strcmp(lua_tostring(L, 2), want) == 0;
    std::printf("%s %s: %s %s\n", ok ? "ok  " : "FAIL", what, luaL_typename(L, 1), lua_tostring(L, 2));
    if (!ok) g_fail++;
}

static const uint8_t* i32(int32_t v) {
    static uint8_t buf[ATTR_STR_MAX];
    std::memset(buf, 0, sizeof(buf));
    std::memcpy(buf, &v, 4);
    return buf;
}

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    check(L, "VAL_INT 42",           VAL_INT,   i32(42),   LUA_TNUMBER,  "42");
    check(L, "VAL_INT -7",           VAL_INT,   i32(-7),   LUA_TNUMBER,  "-7");
    check(L, "VAL_BOOL false",       VAL_BOOL,  i32(0),    LUA_TBOOLEAN, "false");
    check(L, "VAL_BOOL true",        VAL_BOOL,  i32(1),    LUA_TBOOLEAN, "true");
    check(L, "VAL_FLOAT raw x100",   VAL_FLOAT, i32(2550), LUA_TNUMBER,  "2550");
    uint8_t s[ATTR_STR_MAX] = "on";
    check(L, "VAL_STR on",           VAL_STR,   s,         LUA_TSTRING,  "on");
    uint8_t full[ATTR_STR_MAX];
    std::memset(full, 'x', sizeof(full));   // no NUL: bounded by the field
    check(L, "VAL_STR without NUL",  VAL_STR,   full,      LUA_TSTRING,
          std::string(ATTR_STR_MAX, 'x').c_str());
    lua_close(L);
    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nattr values OK\n", g_fail);
    return g_fail;
}
