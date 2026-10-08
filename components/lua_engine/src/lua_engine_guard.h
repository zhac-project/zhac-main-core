// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// The boot-loop guard's pure parts (lua_scheduler.cpp keeps the state in no-init RAM). Host-tested in
// test/host/lua_err_test.cpp, against a stub esp_system.h.

#include <cstdint>

#include "esp_system.h"

// Which resets count as a crash: a panic, every watchdog reset, and a CPU lockup (a double exception on RISC-V:
// IDF v6.0 and v6.1 report it on the P4 and the S31, and a crash loop can end in one). Power-on, a software
// restart, a brownout and the rest are clean boots.
inline bool lua_guard_crashed(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
           r == ESP_RST_CPU_LOCKUP;
}

// The count: the unexpected resets in a row that found the guard armed, tagged so the garbage no-init RAM holds
// after power-on reads as 0. `crashed` = this boot follows a crash (lua_guard_crashed), `armed` = the scripts
// were starting when it happened. Two in a row hold the scripts; it saturates rather than wrapping back to 0.
constexpr uint32_t kLuaGuardTag    = 0x4C434E00u;   // "LCN" + the count in the low byte
constexpr uint32_t kLuaGuardHoldAt = 2;
inline uint32_t lua_guard_count(uint32_t stored, bool crashed, bool armed) {
    uint32_t n = (stored & 0xFFFFFF00u) == kLuaGuardTag ? (stored & 0xFFu) : 0;
    n = (crashed && armed) ? (n < 0xFFu ? n + 1 : n) : 0;
    return kLuaGuardTag | n;
}
inline bool lua_guard_held(uint32_t count) { return (count & 0xFFu) >= kLuaGuardHoldAt; }
