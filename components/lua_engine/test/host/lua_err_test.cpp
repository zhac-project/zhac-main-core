// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// script.error reports (spec 2026-10-05 §3.6).
#include "lua_engine_err.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

int main() {
    LuaErrInfo e{};

    // A runtime error: the chunk name is the script's name.
    lua_err_parse("[string \"heating\"]:12: attempt to index a nil value (global 'x')", nullptr, &e);
    assert(std::strcmp(e.name, "heating") == 0 && e.line == 12);
    assert(std::strcmp(e.message, "attempt to index a nil value (global 'x')") == 0);

    // A compile error at load: the same prefix.
    lua_err_parse("[string \"door_log\"]:3: '=' expected near 'end'", "door_log", &e);
    assert(std::strcmp(e.name, "door_log") == 0 && e.line == 3);
    assert(std::strcmp(e.message, "'=' expected near 'end'") == 0);

    // No prefix (an error raised with a table, not enough memory): the script being loaded or run, line 0.
    lua_err_parse("not enough memory", "pump", &e);
    assert(std::strcmp(e.name, "pump") == 0 && e.line == 0 && std::strcmp(e.message, "not enough memory") == 0);

    // Nothing known: "" and 0.
    lua_err_parse("boom", nullptr, &e);
    assert(e.name[0] == '\0' && e.line == 0 && std::strcmp(e.message, "boom") == 0);
    lua_err_parse(nullptr, nullptr, &e);
    assert(e.name[0] == '\0' && e.message[0] == '\0');

    // A chunk name too long for a script name is not taken as one.
    lua_err_parse("[string \"this_is_far_too_long_to_be_a_name\"]:1: x", "ctx", &e);
    assert(std::strcmp(e.name, "ctx") == 0 && e.line == 1 && std::strcmp(e.message, "x") == 0);
    // The longest script name is 24 bytes; 25 is not a name (and must not overrun `name`).
    lua_err_parse("[string \"abcdefghijklmnopqrstuvwx\"]:1: x", nullptr, &e);
    assert(std::strcmp(e.name, "abcdefghijklmnopqrstuvwx") == 0 && e.line == 1);
    lua_err_parse("[string \"abcdefghijklmnopqrstuvwxy\"]:1: x", "ctx", &e);
    assert(std::strcmp(e.name, "ctx") == 0 && e.line == 1);

    // The message is cut to 120 bytes, never inside a character.
    const std::string cut = "[string \"a\"]:1: " + std::string(119, 'x') + "\xd0\xb6" + "tail";   // ж at bytes 119-120
    lua_err_parse(cut.c_str(), nullptr, &e);
    assert(std::strlen(e.message) == 119);
    const std::string fits = std::string(118, 'x') + "\xd0\xb6";                                  // exactly 120
    lua_err_parse(fits.c_str(), nullptr, &e);
    assert(std::strlen(e.message) == 120);
    lua_err_parse(std::string(130, 'x').c_str(), nullptr, &e);                                    // plain text: cut at 120
    assert(std::strlen(e.message) == 120);

    // At most one event per script per 10 s; scripts are independent; the clock may wrap.
    LuaErrGate g{};
    assert(lua_err_gate_due(&g, "heating", 1000));
    assert(!lua_err_gate_due(&g, "heating", 1000 + 9999));
    assert(lua_err_gate_due(&g, "pump", 1001));
    assert(lua_err_gate_due(&g, "heating", 1000 + 10000));
    assert(!lua_err_gate_due(&g, "heating", 1000 + 10000 + 9999));   // the window restarts at each event
    assert(lua_err_gate_due(&g, "heating", 1000 + 20000));
    assert(lua_err_gate_due(&g, "", 5));
    assert(!lua_err_gate_due(&g, "", 6));
    LuaErrGate w{};
    assert(lua_err_gate_due(&w, "a", 0xFFFFF000u));
    assert(!lua_err_gate_due(&w, "a", 0x00000100u));             // 4,352 ms later, across the wrap
    assert(lua_err_gate_due(&w, "a", 0xFFFFF000u + 10000u));
    // More names than slots: no crash, and a new name still reports.
    LuaErrGate f{};
    for (int i = 0; i < 20; i++) {
        char nm[8];
        std::snprintf(nm, sizeof nm, "s%d", i);
        assert(lua_err_gate_due(&f, nm, 100));
    }
    std::printf("lua_err_tests: ok\n");
    return 0;
}
