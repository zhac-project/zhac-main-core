// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// LU-01 (review 2026-10-24): a bare `coroutine.yield()` (not via zhac.sleep)
// used to leak one slot of CONFIG_LUA_ENGINE_MAX_COROUTINES — the thread was
// orphaned but the live-coroutine count was never decremented, so after 64 such
// yields the whole engine refused every handler/cron/run forever.
//
// This pulls in the REAL scheduler (src/lua_scheduler.cpp) so it drives the
// actual static resume_and_settle() / spawn_coroutine() against the vendored
// Lua, and reads the real lua_scheduler_live_count(). Its ESP/FreeRTOS includes
// resolve to the host stubs in ./stubs.
//
// Fails on the pre-fix scheduler: the bare-yield path skipped
// lua_scheduler_coroutine_exit(), so the count stayed elevated.
//
// It also covers LU-02's scheduler half (cases D/E): the real runaway hook must
// set the over-budget flag the sandbox's pcall/xpcall read (the real
// lua_sandbox.c is linked too), arm_resume_deadline must clear it, and under the
// real hook a pcall-wrapped runaway is killed rather than caught.
// test_runaway.cpp covers the sandbox half with a stand-in flag.
//
// Run with PATH=/usr/bin first if ESP-IDF is active (its `as` breaks g++).
#include <cassert>
#include <chrono>
#include <cstdio>

// The whole scheduler TU — brings in its statics (resume_and_settle, ...).
#include "../../src/lua_scheduler.cpp"

// ── host stand-ins for symbols defined in other firmware TUs ──────────
// None are exercised by the yield paths under test; they only need to link.
extern "C" lua_State* g_L = nullptr;
extern "C" size_t   lua_engine_heap_used_bytes(void) { return 0; }
extern "C" size_t   lua_engine_heap_peak_bytes(void) { return 0; }
extern "C" lua_State* lua_engine_new_state(lua_CFunction) { return nullptr; }
extern "C" uint16_t lua_script_cache_list(LuaScriptEntry*, uint16_t) { return 0; }
extern "C" int      lua_script_cache_read(const char*, char*, size_t) { return -1; }
// A real monotonic clock: the LU-01 yields finish far inside the 50 ms budget,
// while cases D/E run a resume past it so the real hook fires.
int64_t esp_timer_get_time(void) {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
bool cron_matches(const CronExpr&, time_t) { return false; }

// A zhac.sleep-style binding: arms a timer slot (the real continuation path),
// then yields. Exercises s_sleep_armed so the fix must NOT exit this coroutine.
static int test_sleep(lua_State* L) {
    lua_pushthread(L);
    const int r = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_scheduler_sleep(r, 0);
    return lua_yield(L, 0);
}

// Spawn the chunk `src` as a scheduler coroutine and drive one resume.
static void spawn_and_settle(lua_State* L, const char* src, const char* ctx) {
    assert(luaL_loadstring(L, src) == LUA_OK);   // the chunk is the coroutine body
    int ref = LUA_NOREF;
    lua_State* co = spawn_coroutine(L, &ref);
    assert(co);
    resume_and_settle(L, co, ref, 0, ctx, nullptr);
}

int main() {
    g_L = luaL_newstate();
    assert(g_L);
    luaL_openlibs(g_L);
    lua_pushcfunction(g_L, test_sleep);
    lua_setglobal(g_L, "test_sleep");

    // A: a bare coroutine.yield() must balance the live count (orphan released).
    const uint16_t base = lua_scheduler_live_count();
    assert(luaL_loadstring(g_L, "coroutine.yield()") == LUA_OK);
    int ref = LUA_NOREF;
    lua_State* co = spawn_coroutine(g_L, &ref);
    assert(co && lua_scheduler_live_count() == base + 1);   // spawn counted it
    resume_and_settle(g_L, co, ref, 0, "bare-yield", nullptr);
    assert(lua_scheduler_live_count() == base);             // FAILS pre-fix (stays base+1)
    std::printf("  ok: bare coroutine.yield() balances the live count (%u)\n",
                (unsigned)lua_scheduler_live_count());

    // B: a zhac.sleep-style yield keeps its slot (the timer continuation owns it).
    const uint16_t base2 = lua_scheduler_live_count();
    spawn_and_settle(g_L, "test_sleep()", "sleep-yield");
    assert(lua_scheduler_live_count() == base2 + 1);        // still parked, not exited
    std::printf("  ok: zhac.sleep-style yield keeps its slot (%u)\n",
                (unsigned)lua_scheduler_live_count());

    // C: 100 bare yields do not accrete (pre-fix this blows past the 64 cap).
    const uint16_t base3 = lua_scheduler_live_count();
    for (int i = 0; i < 100; ++i) spawn_and_settle(g_L, "coroutine.yield()", "bare-yield-loop");
    assert(lua_scheduler_live_count() == base3);            // FAILS pre-fix (base3 + 100)
    std::printf("  ok: 100 bare yields leave the live count flat (%u)\n",
                (unsigned)lua_scheduler_live_count());

    // D (LU-02, real hook): a resume driven past LUA_RESUME_DEADLINE_US sets the
    // over-budget flag the sandbox's pcall/xpcall read; arm_resume_deadline,
    // which every resume starts with, clears it again.
    lua_State* L2 = luaL_newstate();
    assert(L2);
    luaL_openlibs(L2);
    assert(lua_engine_sandbox_apply(L2));                  // the real kill-aware pcall/xpcall
    assert(!lua_scheduler_resume_over_budget());
    const uint16_t base4 = lua_scheduler_live_count();
    spawn_and_settle(L2, "while true do end", "runaway"); // the real hook raises at the deadline
    assert(lua_scheduler_resume_over_budget());            // FAILS if the hook stops setting it
    assert(lua_scheduler_live_count() == base4);           // the killed coroutine was settled
    lua_State* t = lua_newthread(L2);
    arm_resume_deadline(t);
    lua_pop(L2, 1);
    assert(!lua_scheduler_resume_over_budget());           // FAILS if arm stops clearing it
    std::printf("  ok: the real hook sets the over-budget flag; arm_resume_deadline clears it\n");

    // E (LU-02, end to end): under the real hook a pcall-wrapped runaway is
    // killed, not caught — the statement after the pcall never runs.
    const uint32_t errs = lua_scheduler_error_count();
    spawn_and_settle(L2, "pcall(function() while true do end end) caught = true", "pcall-runaway");
    lua_getglobal(L2, "caught");
    assert(lua_isnil(L2, -1));                             // FAILS if the pcall swallowed the kill
    lua_pop(L2, 1);
    assert(lua_scheduler_error_count() == errs + 1);       // the resume ended in the kill
    assert(lua_scheduler_live_count() == base4);
    std::printf("  ok: a pcall-wrapped runaway under the real hook is killed, not caught\n");
    lua_close(L2);

    lua_close(g_L);
    std::printf("lua_coro_leak_tests: ok\n");
    return 0;
}
