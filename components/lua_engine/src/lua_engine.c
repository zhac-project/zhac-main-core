// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// lua_engine.c — VM creation, bootstrap, and shutdown.

#include "sdkconfig.h"

#if CONFIG_LUA_ENGINE_ENABLED

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "lua_alloc.h"
#include "lua_engine.h"
#include "lua_engine_scripts.h"
#include "lua_internal.h"
#include "lua_scheduler.h"

static const char* TAG = "lua_engine";

lua_State* g_L = NULL;

bool lua_engine_init(void) {
    if (g_L) return true;

    // Lua 5.5 adds a seed arg to randomise string hashing; 0 is fine
    // for our single-state embedded use.
    g_L = lua_newstate(lua_engine_alloc, NULL, 0);
    if (!g_L) {
        ESP_LOGE(TAG, "lua_newstate failed — out of memory?");
        return false;
    }

    // Load the standard libs (we'll redact the unsafe bits in the
    // sandbox pass). Lua's `luaL_openlibs` registers base / table /
    // string / math / io / os / coroutine / debug / package.
    luaL_openlibs(g_L);

    if (!lua_engine_sandbox_apply(g_L)) {
        ESP_LOGE(TAG, "sandbox apply failed");
        goto fail;
    }
    if (!lua_engine_require_install(g_L)) {
        ESP_LOGE(TAG, "require install failed");
        goto fail;
    }
    if (!lua_engine_scheduler_start(g_L)) {
        ESP_LOGE(TAG, "scheduler start failed");
        goto fail;
    }

    // EventBus → Lua bridge (defined in lua_engine_event_bridge.cpp).
    extern void lua_engine_event_bridge_start(void);
    lua_engine_event_bridge_start();

    // simple_rules `DO lua.run "<name>"` hook. Queues into TaskLua so
    // the rule engine never blocks on Lua.
    extern void lua_engine_rules_hook_install(void);
    lua_engine_rules_hook_install();

    ESP_LOGI(TAG, "Lua engine ready (budget %d KB, %d coroutines max)",
             CONFIG_LUA_ENGINE_HEAP_KB,
             CONFIG_LUA_ENGINE_MAX_COROUTINES);
    return true;

fail:
    lua_close(g_L);
    g_L = NULL;
    return false;
}

// `lua_engine_heap_used_bytes` / `_peak_bytes` are defined in
// lua_alloc.c directly — the public header's declaration and the
// allocator's definition resolve to the same symbol. Only the
// scheduler-side counters need forwarders.
uint16_t lua_engine_live_coroutines(void)  { return lua_scheduler_live_count(); }
uint32_t lua_engine_error_count(void)      { return lua_scheduler_error_count(); }
uint32_t lua_engine_yield_count(void)      { return lua_scheduler_yield_count(); }

// Compile + run one cached script as a top-level coroutine. Each file
// gets its own coroutine so an error or `zhac.sleep` in one script
// doesn't block the others.
static bool load_one_script(lua_State* L, const char* name) {
    // Park 16 KB script-load scratch in PSRAM. One-shot lazy init,
    // never freed — same lifetime as the original BSS-static array.
    enum { kBufSz = 16 * 1024 };
    static char* buf = NULL;
    if (!buf) {
        buf = (char*)heap_caps_malloc(kBufSz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (char*)heap_caps_malloc(kBufSz, MALLOC_CAP_8BIT);
        if (!buf) {
            ESP_LOGE(TAG, "load_one_script: buf alloc failed");
            return false;
        }
    }
    const int n = lua_script_cache_read(name, buf, kBufSz);
    if (n < 0) {
        ESP_LOGW(TAG, "script '%s' not readable", name);
        return false;
    }
    if (luaL_loadbufferx(L, buf, (size_t)n, name, "t") != LUA_OK) {
        ESP_LOGE(TAG, "compile '%s': %s", name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    // Spawn consumes the function on top of L and schedules it.
    if (lua_scheduler_spawn(L) < 0) {
        ESP_LOGE(TAG, "spawn '%s' failed", name);
        return false;
    }
    return true;
}

void lua_engine_load_all(void) {
    if (!g_L) return;
    LuaScriptEntry entries[LUA_SCRIPT_MAX];
    const uint16_t n = lua_script_cache_list(entries, LUA_SCRIPT_MAX);
    uint16_t loaded = 0;
    for (uint16_t i = 0; i < n; ++i) {
        if (load_one_script(g_L, entries[i].name)) loaded++;
    }
    ESP_LOGI(TAG, "lua scripts loaded: %u/%u", loaded, n);
}

// EventBus → Lua dispatcher. The full struct layouts for Event /
// ZclAttrEvent / MqttMsgEvent are C++-only (they use <cstdint>), so
// the wiring lives in `lua_engine_event_bridge.cpp`. This function
// stays as a C-callable entry point for the initial boot event
// published from main.cpp at startup.
//
// `ev` is treated opaquely — its type byte lives at offset 0 and
// the 96-byte data blob starts at offset 8 (aligned as the
// event_bus header defines). The bridge file knows the real shape.
void lua_engine_dispatch(const struct Event* ev) {
    (void)ev;
}

// ── Syntax-check (no execute) ───────────────────────────────────────
//
// Used by the /api/scripts/check WS command so the rich SPA editor can
// show inline parse errors as the user types. A throwaway lua_State
// keeps the engine's live state clean — we don't want user source in
// the main VM until they hit Save.
//
// Lua compiler error shape: "[string \"NAME\"]:LINE: message" — we
// parse out LINE for the CM6 linter gutter marker.
#include <string.h>
#include <stdlib.h>

bool lua_engine_check_syntax(const char* name, const char* src,
                              char* err_out, size_t err_cap,
                              int*   line_out) {
    if (!src) return false;
    const size_t slen = strlen(src);
    const char* chunkname = (name && name[0]) ? name : "check";

    // Fresh state — default allocator is system heap. ~4 KB transient.
    lua_State* L = luaL_newstate();
    if (!L) {
        if (err_out && err_cap) strncpy(err_out, "out of memory", err_cap - 1);
        if (line_out) *line_out = 1;
        return false;
    }

    const int rc = luaL_loadbufferx(L, src, slen, chunkname, "t");
    bool ok = (rc == LUA_OK);
    if (!ok) {
        const char* msg = lua_tostring(L, -1);
        if (msg && err_out && err_cap) {
            strncpy(err_out, msg, err_cap - 1);
            err_out[err_cap - 1] = '\0';
        }
        int line = 1;
        if (msg) {
            // Walk past the "[string "..."]:" bracket, find the first
            // ':<digits>:' group.
            const char* p = strchr(msg, ']');
            const char* colon = p ? strchr(p, ':') : NULL;
            if (colon) line = atoi(colon + 1);
            if (line < 1) line = 1;
        }
        if (line_out) *line_out = line;
    } else if (err_out && err_cap) {
        err_out[0] = '\0';
    }

    lua_close(L);
    return ok;
}

#else   // ── Disabled ────────────────────────────────────────────────

#include <stddef.h>
#include <stdint.h>
#include "lua_engine.h"

bool     lua_engine_init(void)             { return true; }
void     lua_engine_load_all(void)         {}
void     lua_engine_dispatch(const struct Event* ev) { (void)ev; }
bool     lua_engine_check_syntax(const char* n, const char* s,
                                  char* e, size_t ec, int* l) {
    (void)n; (void)s;
    if (e && ec) e[0] = '\0';
    if (l) *l = 1;
    return true;   // engine disabled → no syntax checks, pretend OK
}
size_t   lua_engine_heap_used_bytes(void)  { return 0; }
size_t   lua_engine_heap_peak_bytes(void)  { return 0; }
uint16_t lua_engine_live_coroutines(void)  { return 0; }
uint32_t lua_engine_error_count(void)      { return 0; }
uint32_t lua_engine_yield_count(void)      { return 0; }

#endif
