// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// LU-02 (review 2026-10-24): a pcall-wrapped runaway loop must not be able to
// catch the engine's CPU-deadline kill and keep running. This links the REAL
// sandbox (src/lua_sandbox.c) against the vendored Lua on the host and drives
// the over-budget flag the scheduler's runaway hook sets, so it exercises the
// actual sandboxed_pcall / sandboxed_xpcall rethrow path.
//
// Fails on the pre-fix sandbox: the stock pcall/xpcall it installed returned the
// kill to the script (cases 2/3 assert the kill cannot be swallowed).
//
// Run with PATH=/usr/bin first if ESP-IDF is active (its `as` breaks g++).
#include <atomic>
#include <chrono>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lua_internal.h"   // lua_engine_sandbox_apply, lua_scheduler_resume_over_budget
}

// ── stand-in for the scheduler's over-budget flag ─────────────────────
// The real flag lives in lua_scheduler.cpp (set by the runaway count hook).
// The sandbox reads it through this accessor; here the test drives it, and the
// optional count hook below flips it exactly as the real hook does.
static std::atomic<bool> g_over{false};
extern "C" bool lua_scheduler_resume_over_budget(void) { return g_over.load(); }

static int64_t now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// Mirrors lua_scheduler.cpp's hook: once past the (scaled-down) deadline, flip
// the over-budget flag and raise the same budget error, escalating to every
// instruction past the hard ceiling.
static int64_t s_deadline_us, s_hard_us;
static bool s_killed;
static void runaway_hook(lua_State* L, lua_Debug*) {
    const int64_t n = now_us();
    if (n < s_deadline_us) return;
    g_over.store(true);
    if (!s_killed && n >= s_hard_us) { s_killed = true; lua_sethook(L, runaway_hook, LUA_MASKCOUNT, 1); }
    luaL_where(L, 0);
    lua_pushliteral(L, "lua_engine: resume exceeded budget");
    lua_concat(L, 2);
    lua_error(L);
}

static lua_State* new_sandboxed_state() {
    lua_State* L = luaL_newstate();
    assert(L);
    luaL_openlibs(L);
    assert(lua_engine_sandbox_apply(L));   // installs the real sandboxed pcall/xpcall
    return L;
}

// Compile+run a chunk on the main state. Returns the lua_resume/pcall status and
// the top-of-stack string.
static int run_chunk(lua_State* L, const char* src, std::string& out) {
    int status = luaL_loadstring(L, src);
    if (status == LUA_OK) status = lua_pcall(L, 0, LUA_MULTRET, 0);
    const char* s = lua_tostring(L, -1);
    out = s ? s : "(nil)";
    return status;
}

static bool contains(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

// ── Case 1: ordinary pcall still catches ordinary errors (fix must not break it)
static void test_ordinary_pcall() {
    lua_State* L = new_sandboxed_state();
    g_over.store(false);
    std::string out;
    int st = run_chunk(L, "local ok, e = pcall(function() error('boom') end) "
                          "return tostring(ok) .. ' ' .. tostring(e)", out);
    assert(st == LUA_OK);
    assert(contains(out, "false") && contains(out, "boom"));
    lua_close(L);
    std::printf("  ok: ordinary pcall still catches errors (%s)\n", out.c_str());
}

// ── Case 2: over budget, a pcall cannot swallow the kill ──────────────
static void test_pcall_cannot_swallow_kill() {
    lua_State* L = new_sandboxed_state();
    g_over.store(true);   // resume is past its CPU deadline
    std::string out;
    int st = run_chunk(L, "pcall(function() error('kill') end) return 'SWALLOWED'", out);
    // Fix: the sandboxed pcall re-raises, so the chunk errors out (never returns
    // 'SWALLOWED'). Pre-fix (stock pcall) the chunk returned LUA_OK/'SWALLOWED'.
    assert(st != LUA_OK);
    assert(!contains(out, "SWALLOWED"));
    lua_close(L);
    std::printf("  ok: over-budget pcall re-raises the kill (status=%d)\n", st);
}

// ── Case 3: same for xpcall (even with a message handler) ─────────────
static void test_xpcall_cannot_swallow_kill() {
    lua_State* L = new_sandboxed_state();
    g_over.store(true);
    std::string out;
    int st = run_chunk(L, "xpcall(function() error('kill') end, function(e) return e end) "
                          "return 'SWALLOWED'", out);
    assert(st != LUA_OK);
    assert(!contains(out, "SWALLOWED"));
    lua_close(L);
    std::printf("  ok: over-budget xpcall re-raises the kill (status=%d)\n", st);
}

// ── Case 4: pcall stays yieldable (zhac.sleep inside pcall must still work) ──
static void test_yield_across_pcall() {
    lua_State* L = new_sandboxed_state();
    g_over.store(false);
    lua_State* co = lua_newthread(L);
    assert(luaL_loadstring(co, "local ok, v = pcall(function() local x = coroutine.yield(7) return x end) "
                               "return 'ok=' .. tostring(ok) .. ' v=' .. tostring(v)") == LUA_OK);
    int nres = 0;
    int st = lua_resume(co, L, 0, &nres);
    assert(st == LUA_YIELD && nres == 1 && lua_tointeger(co, -1) == 7);   // yielded through pcall
    lua_pop(co, nres);
    lua_pushinteger(co, 99);                 // value the yield returns
    st = lua_resume(co, L, 1, &nres);
    assert(st == LUA_OK);
    std::string out = lua_tostring(co, -1) ? lua_tostring(co, -1) : "(nil)";
    assert(contains(out, "ok=true") && contains(out, "v=99"));
    lua_close(L);
    std::printf("  ok: pcall stays yieldable across a yield (%s)\n", out.c_str());
}

// ── Case 5 (end-to-end): a pcall-wrapped infinite loop is actually killed ──
struct E2E { std::atomic<bool> done{false}; int status{-1}; };
static E2E* g_e2e;
static void* watchdog(void*) {
    const int64_t start = now_us();
    while (!g_e2e->done.load()) {
        if (now_us() - start > 3 * 1000000LL) {
            std::fprintf(stderr, "FAIL: runaway pcall loop wedged (no return in 3s)\n");
            _exit(42);
        }
        struct timespec ts{0, 5 * 1000000}; nanosleep(&ts, nullptr);
    }
    return nullptr;
}
static void* e2e_run(void*) {
    lua_State* L = new_sandboxed_state();
    lua_State* co = lua_newthread(L);
    luaL_loadstring(co, "while true do pcall(function() while true do end end) end");
    const int64_t base = now_us();
    s_deadline_us = base + 5 * 1000; s_hard_us = base + 20 * 1000; s_killed = false; g_over.store(false);
    lua_sethook(co, runaway_hook, LUA_MASKCOUNT, 1000);
    int nres = 0;
    g_e2e->status = lua_resume(co, L, 0, &nres);
    lua_close(L);
    g_e2e->done.store(true);
    return nullptr;
}
static void test_runaway_pcall_killed_e2e() {
    g_e2e = new E2E();
    pthread_t wd, t;
    pthread_create(&wd, nullptr, watchdog, nullptr);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_create(&t, &at, e2e_run, nullptr);
    pthread_join(t, nullptr);
    pthread_join(wd, nullptr);
    assert(g_e2e->status != LUA_OK && g_e2e->status != LUA_YIELD);   // killed, not swallowed/looping
    std::printf("  ok: runaway pcall loop killed end-to-end (status=%d)\n", g_e2e->status);
    delete g_e2e;
}

int main() {
    test_ordinary_pcall();
    test_pcall_cannot_swallow_kill();
    test_xpcall_cannot_swallow_kill();
    test_yield_across_pcall();
    test_runaway_pcall_killed_e2e();
    std::printf("lua_runaway_tests: ok\n");
    return 0;
}
