// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Lua error reports for the script.error event (spec 2026-10-05 §3.6). Pure and header-only (C++), so
// no build list changes; host-tested in test/host/lua_err_test.cpp.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

constexpr uint32_t kLuaErrEveryMs    = 10000;   // at most one event per script per 10 s
constexpr size_t   kLuaErrMessageMax = 120;     // bytes of Lua's text in an event

struct LuaErrInfo {
    char name[25];                         // a script name (LUA_SCRIPT_NAME_MAX + 1); "" when unknown
    int  line;                             // 0 when unknown
    char message[kLuaErrMessageMax + 1];   // Lua's text without its `[string "<name>"]:<line>: ` prefix
};

// Lua prefixes an error with its chunk name and line, `[string "<name>"]:<line>: <text>`, and chunk names
// are script names (luaL_loadbufferx(…, name, "t")). Without that prefix the name is `ctx_name` (the
// script being loaded or run), else "" and line 0. The text is cut to 120 bytes on a character boundary
// (a cut character would be invalid UTF-8 in a WebSocket text frame, which closes the socket).
inline void lua_err_parse(const char* msg, const char* ctx_name, LuaErrInfo* out) {
    *out = LuaErrInfo{};
    const char* text = msg ? msg : "";
    static constexpr char kOpen[] = "[string \"";
    if (std::strncmp(text, kOpen, sizeof(kOpen) - 1) == 0) {
        const char* name = text + sizeof(kOpen) - 1;
        const char* close = std::strstr(name, "\"]:");
        if (close) {
            char* end = nullptr;
            const long line = std::strtol(close + 3, &end, 10);
            if (end != close + 3 && *end == ':') {
                const size_t n = static_cast<size_t>(close - name);
                if (n < sizeof(out->name)) std::memcpy(out->name, name, n);
                out->line = static_cast<int>(line);
                text = end + 1;
                if (*text == ' ') text++;
            }
        }
    }
    if (!out->name[0] && ctx_name) std::snprintf(out->name, sizeof(out->name), "%.24s", ctx_name);
    size_t n = std::strlen(text);
    if (n > kLuaErrMessageMax) {
        n = kLuaErrMessageMax;
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) n--;
    }
    std::memcpy(out->message, text, n);
    out->message[n] = '\0';
}

// One slot per script name: 16 stored scripts and "" for errors of unknown origin. A full table reuses
// the slot that reported longest ago.
struct LuaErrGate {
    char     name[17][25];
    uint32_t last_ms[17];
    uint8_t  used;
};

// True when `name` may raise script.error now: its first error, or 10 s after its last event. The rest go
// to the log only, so a failing handler on a busy sensor cannot flood the cloud relay's queue (16 frames,
// oldest dropped first) and push device events out. Unsigned subtraction survives the clock wrapping.
inline bool lua_err_gate_due(LuaErrGate* g, const char* name, uint32_t now_ms) {
    const char* key = name ? name : "";
    for (uint8_t i = 0; i < g->used; i++) {
        if (std::strcmp(g->name[i], key) != 0) continue;
        if (now_ms - g->last_ms[i] < kLuaErrEveryMs) return false;
        g->last_ms[i] = now_ms;
        return true;
    }
    uint8_t slot = g->used;
    if (slot < 17) {
        g->used++;
    } else {
        slot = 0;
        for (uint8_t i = 1; i < 17; i++) {
            if (now_ms - g->last_ms[i] > now_ms - g->last_ms[slot]) slot = i;
        }
    }
    std::snprintf(g->name[slot], sizeof(g->name[slot]), "%.24s", key);
    g->last_ms[slot] = now_ms;
    return true;
}
