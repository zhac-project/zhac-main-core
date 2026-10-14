// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Push an attribute event's value onto a Lua stack as its Lua type. Split out
// of lua_scheduler.cpp so the type mapping is tested on the host
// (test/attr_value/): it is where a number turned into a boolean before.
#pragma once

#include <cstdint>
#include <cstring>

#include "lua.h"
#include "zcl_attribute.h"

// `val` points at the event's value union (int_val / str_val). VAL_INT and
// VAL_FLOAT both arrive as integers: a float stays the raw x100 value, the
// rules/Lua convention (temperature > 2500 means 25.00).
inline void lua_push_attr_value(lua_State* L, uint8_t val_type, const uint8_t* val) {
    if (val_type == VAL_STR) {
        const char* sv = (const char*)val;
        lua_pushlstring(L, sv, strnlen(sv, ATTR_STR_MAX));
        return;
    }
    int32_t v; memcpy(&v, val, 4);
    if (val_type == VAL_BOOL) lua_pushboolean(L, v != 0);
    else                      lua_pushinteger(L, v);
}
