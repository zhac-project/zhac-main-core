// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// script.error reports (spec 2026-10-05 §3.6).
#include "lua_engine_err.h"
#include "lua_engine_guard.h"
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
    // More names than slots: no crash, and a new name still reports (17 of 20 here; the global budget below
    // ends the burst).
    LuaErrGate f{};
    for (int i = 0; i < 20; i++) {
        char nm[8];
        std::snprintf(nm, sizeof nm, "s%d", i);
        assert(lua_err_gate_due(&f, nm, 100) == (i < 17));
    }

    // The gate's key (spec §3.6, the Task 8 ruling): a script can forge the `[string "<name>"]:` prefix with
    // error(msg, 0), so the key is the script being loaded or run when the caller knows it, else the prefix's
    // name only when it could be a stored script's ([A-Za-z0-9_-]{1,24}), else "".
    lua_err_parse("[string \"x\\\"y\nz\"]:7: boom", nullptr, &e);   // a quote and a newline in the "name"
    assert(std::strcmp(lua_err_gate_key(nullptr, e), "") == 0);
    lua_err_parse("[string \"\xff\xfe\"]:7: boom", nullptr, &e);
    assert(std::strcmp(lua_err_gate_key(nullptr, e), "") == 0);
    lua_err_parse("[string \"other\"]:7: boom", "pump", &e);         // a forged prefix during pump's load
    assert(std::strcmp(lua_err_gate_key("pump", e), "pump") == 0);
    lua_err_parse("[string \"heating-2_b\"]:3: boom", nullptr, &e);   // a handler's own error: its chunk name
    assert(std::strcmp(lua_err_gate_key(nullptr, e), "heating-2_b") == 0);
    assert(lua_err_name_ok("abcdefghijklmnopqrstuvwx") && !lua_err_name_ok("abcdefghijklmnopqrstuvwxy") &&
           !lua_err_name_ok("") && !lua_err_name_ok(nullptr) && !lua_err_name_ok("a b"));

    // Forged names cannot defeat the gate. Invalid ones share the "" slot: one event per 10 s.
    LuaErrGate g2{};
    int passed = 0;
    for (int i = 0; i < 40; i++) {
        char msg[48];
        std::snprintf(msg, sizeof msg, "[string \"x\\\"%d\"]:1: boom", i);
        lua_err_parse(msg, nullptr, &e);
        passed += lua_err_gate_due(&g2, lua_err_gate_key(nullptr, e), 500) ? 1 : 0;
    }
    assert(passed == 1);
    // Valid-looking ones (x1 ... x40) each have a slot, so a global budget ends the burst: at most 17 events
    // (16 stored scripts and "", what honest scripts can raise) in any 10 s window, then 17 again.
    LuaErrGate g3{};
    passed = 0;
    for (int i = 0; i < 40; i++) {
        char nm[8];
        std::snprintf(nm, sizeof nm, "x%d", i);
        passed += lua_err_gate_due(&g3, nm, 1000 + static_cast<uint32_t>(i)) ? 1 : 0;   // a forging loop: 1 ms apart
    }
    assert(passed == kLuaErrGlobalMax && kLuaErrGlobalMax == 17);
    passed = 0;
    for (int i = 0; i < 40; i++) {
        char nm[8];
        std::snprintf(nm, sizeof nm, "y%d", i);
        passed += lua_err_gate_due(&g3, nm, 5000 + static_cast<uint32_t>(i)) ? 1 : 0;   // the same window
    }
    assert(passed == 0);
    assert(lua_err_gate_due(&g3, "y0", 1000 + 10000));                                  // the next window
    // Sixteen honest scripts failing at once, plus one error of unknown origin, all report.
    LuaErrGate h{};
    for (int i = 0; i < 16; i++) {
        char nm[8];
        std::snprintf(nm, sizeof nm, "s%d", i);
        assert(lua_err_gate_due(&h, nm, 7));
    }
    assert(lua_err_gate_due(&h, "", 8));

    // The boot-loop guard's count (lua_scheduler.cpp keeps it in no-init RAM): unexpected resets in a row that
    // found the guard armed. Garbage after power-on reads as 0; a clean boot or a crash outside the window
    // resets it; two in a row hold the scripts; it saturates instead of wrapping.
    uint32_t c = lua_guard_count(0xDEADBEEFu, false, true);      // power-on: garbage, a clean reset
    assert(!lua_guard_held(c) && lua_guard_count(c, true, true) == lua_guard_count(0x12345678u, true, true));
    c = lua_guard_count(0xDEADBEEFu, true, true);                // garbage that is not ours counts from 0
    assert(!lua_guard_held(c));                                  // one crash: load again
    c = lua_guard_count(c, true, true);
    assert(lua_guard_held(c));                                   // two in a row: hold
    assert(lua_guard_held(lua_guard_count(c, true, true)));      // a crash during Apply while held: still held
    assert(!lua_guard_held(lua_guard_count(c, false, true)));    // a clean boot (power-on, software restart) ends it
    assert(!lua_guard_held(lua_guard_count(c, true, false)));    // a crash outside the window breaks the row
    uint32_t s = lua_guard_count(0, false, false);
    for (int i = 0; i < 300; i++) s = lua_guard_count(s, true, true);
    assert(lua_guard_held(s) && lua_guard_held(lua_guard_count(s, true, true)));   // no wrap back to 0

    // Which resets count: a panic and the watchdogs; power-on, a software restart (Restart, an OTA), a brownout
    // and the rest are clean boots.
    for (esp_reset_reason_t r : {ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT}) assert(lua_guard_crashed(r));
    assert(lua_guard_crashed(ESP_RST_CPU_LOCKUP));   // a double exception on RISC-V: a crash loop can end in one
    for (esp_reset_reason_t r : {ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_DEEPSLEEP,
                                 ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB, ESP_RST_JTAG, ESP_RST_EFUSE, ESP_RST_PWR_GLITCH})
        assert(!lua_guard_crashed(r));
    std::printf("lua_err_tests: ok\n");
    return 0;
}
