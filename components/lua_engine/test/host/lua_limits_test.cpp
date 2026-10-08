// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Lua C-stack limits (final fix wave, I-2(a), Option A). The vendored Lua 5.5 and lua-cjson are built with
// the definitions lua_limits.cmake gives the firmware's Lua library, cjson gets the engine's limits
// (lua_engine_limits.h), and every case runs on a thread with a 64 KB stack. A script too deep for the stack
// must end in a Lua error, never a crash: with Lua's own limits (200 C levels, 200 pattern levels, cjson 1,000
// deep) the deep pcall case overflows this stack (it needed about 160 KB here) and the pattern and cjson cases
// do not fail at all; with the engine's it needs about 20 KB.
#include <pthread.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lua_engine_limits.h"
int luaopen_cjson(lua_State* L);
}

struct Run {
    std::string src;
    int rc = -1;
    std::string out;
};

static void* run_on_thread(void* arg) {
    auto* r = static_cast<Run*>(arg);
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    luaL_requiref(L, "cjson", luaopen_cjson, 1);   // as lua_require.c preloads it
    lua_engine_cjson_limit(L);
    lua_pop(L, 1);
    r->rc = luaL_loadbufferx(L, r->src.data(), r->src.size(), "t", "t");
    if (r->rc == LUA_OK) r->rc = lua_pcall(L, 0, 1, 0);
    const char* s = lua_tostring(L, -1);
    r->out = s ? s : "(nil)";
    lua_close(L);
    return nullptr;
}

static Run run(const std::string& src) {
    Run r;
    r.src = src;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    pthread_t t;
    if (pthread_create(&t, &at, run_on_thread, &r) != 0) std::abort();
    pthread_join(t, nullptr);
    pthread_attr_destroy(&at);
    return r;
}

static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static std::string nested(int n, const char* open, const char* body, const char* close) {
    std::string s;
    for (int i = 0; i < n; i++) s += open;
    s += body;
    for (int i = 0; i < n; i++) s += close;
    return s;
}

int main() {
    // 200 nested parentheses: the parser stops with a Lua error.
    Run r = run("return " + nested(200, "(", "1", ")"));
    assert(r.rc != LUA_OK && has(r.out, "C stack overflow"));

    // Deep recursion through pcall: each level costs about 690 B of C stack on the hub; the innermost call fails
    // with "C stack overflow" after at most LUAI_MAXCCALLS levels, and the error comes back up as a Lua error.
    r = run("local d = 0 local function f() d = d + 1 local ok, e = pcall(f) if not ok then error(e, 0) end end "
            "local ok, e = pcall(f) return d .. ' ' .. tostring(e)");
    assert(r.rc == LUA_OK && has(r.out, "stack overflow"));
    assert(std::atoi(r.out.c_str()) >= 10 && std::atoi(r.out.c_str()) < 17);
    // The same through string.gsub callbacks (about 900 B a level on the hub).
    r = run("local function f(s) return (s:gsub('.', function(c) return f(c) end)) end return f('x')");
    assert(r.rc == LUA_ERRRUN && has(r.out, "stack overflow"));

    // Pattern matching recurses once per optional item: 199 of them is "pattern too complex", 30 still match.
    r = run("return tostring(string.find(('a'):rep(300), ('a?'):rep(199)))");
    assert(r.rc == LUA_ERRRUN && has(r.out, "pattern too complex"));
    r = run("return tostring(string.find(('a'):rep(300), ('a?'):rep(30)))");
    assert(r.rc == LUA_OK && r.out == "1");

    // cjson: 16 levels encode and decode, 17 are refused, and a script cannot raise the limit or open a fresh
    // instance with the defaults (1,000 levels).
    static const char kDeep[] = "local function deep(n) local t = {} local c = t for i = 2, n do c[1] = {} c = c[1] end "
                                "return t end ";
    r = run(std::string(kDeep) + "return cjson.encode(deep(16))");
    assert(r.rc == LUA_OK && r.out == nested(15, "[", "{}", "]"));
    r = run(std::string(kDeep) + "return cjson.encode(deep(17))");
    assert(r.rc == LUA_ERRRUN && has(r.out, "excessive nesting"));
    r = run("return #cjson.decode('" + nested(16, "[", "", "]") + "')");
    assert(r.rc == LUA_OK && r.out == "1");
    r = run("return cjson.decode('" + nested(17, "[", "", "]") + "')");
    assert(r.rc == LUA_ERRRUN && has(r.out, "too many nested"));
    r = run("return tostring(cjson.encode_max_depth) .. tostring(cjson.decode_max_depth) .. tostring(cjson.new)");
    assert(r.rc == LUA_OK && r.out == "nilnilnil");
    assert(LUA_ENGINE_CJSON_MAX_DEPTH == 16);

    // An ordinary script still compiles and runs: 10 nested levels, as deep as the deepest documentation example.
    r = run("local t = {a = {b = {1, 2, {3}}}} "
            "local function h(ev) "
            "  if ev and ev.v then "
            "    for i = 1, 2 do "
            "      if (ev.v + (i * (2 + (3 - (1))))) > 0 then "
            "        return string.format('%d', math.max(1, math.min(10, (t.a.b[3][1] + i)))) "
            "      end "
            "    end "
            "  end "
            "end "
            "return h({v = 1})");
    assert(r.rc == LUA_OK && r.out == "4");

    std::printf("lua_limits_tests: ok\n");
    return 0;
}
