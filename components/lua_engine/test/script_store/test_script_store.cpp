// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The script store keeps a saved script whole (review F08, F21):
//   - a power cut in the middle of replacing a script leaves the old or the
//     new version, never none (checked first: recovery runs at the first mount);
//   - two saves of one script at once leave exactly one of them, and a reader
//     never finds the script missing meanwhile.
#include "lua_engine_scripts.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

// SPIFFS's rename() does not replace an existing file (POSIX's does); the
// store's calls are routed here at link time (-Wl,--wrap=rename).
// It also notes the longest file name the store renames: SPIFFS object names
// hold at most 31 characters (CONFIG_SPIFFS_OBJ_NAME_LEN=32 with the NUL).
static size_t g_longest = 0;
static size_t base_len(const char* p) { const char* s = std::strrchr(p, '/'); return std::strlen(s ? s + 1 : p); }
extern "C" int __real_rename(const char* from, const char* to);
extern "C" int __wrap_rename(const char* from, const char* to) {
    if (base_len(from) > g_longest) g_longest = base_len(from);
    if (base_len(to) > g_longest) g_longest = base_len(to);
    struct stat st;
    if (stat(to, &st) == 0) { errno = EEXIST; return -1; }
    return __real_rename(from, to);
}

static int g_fail = 0;
static void expect(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static const std::string kDir = LUA_SCRIPT_BASE_PATH;
static void put(const char* file, const char* text) {
    FILE* f = std::fopen((kDir + "/" + file).c_str(), "w");
    std::fputs(text, f);
    std::fclose(f);
}
static bool there(const char* file) {
    struct stat st;
    return stat((kDir + "/" + file).c_str(), &st) == 0;
}
static std::string get(const char* name) {
    char buf[LUA_SCRIPT_SRC_MAX + 1];
    const int n = lua_script_cache_read(name, buf, sizeof(buf));
    return n < 0 ? std::string("<missing>") : std::string(buf, n);
}

int main() {
    std::system(("rm -rf '" + kDir + "' && mkdir -p '" + kDir + "'").c_str());

    // Power cut while replacing: the old version was set aside, the new one
    // not yet in place.
    put("cut.lua.bak", "-- old");
    put("cut.lua.tmp", "-- new, maybe half");
    // Power cut after the new version landed, before the old one was dropped.
    put("done.lua", "-- new");
    put("done.lua.bak", "-- old");
    // The same with today's names (<name>.bak / <name>.tmp).
    put("cut2.bak", "-- old");
    put("cut2.tmp", "-- new, maybe half");
    put("done2.lua", "-- new");
    put("done2.bak", "-- old");
    expect("a script cut mid-replace comes back as its old version", get("cut") == "-- old");
    expect("  its half-written copy is gone", !there("cut.lua.tmp"));
    expect("a finished replace keeps the new version", get("done") == "-- new");
    expect("  and drops the old one", !there("done.lua.bak"));
    expect("today's names: a cut replace comes back as the old version", get("cut2") == "-- old" && !there("cut2.tmp"));
    expect("  a finished one keeps the new version", get("done2") == "-- new" && !there("done2.bak"));

    // A 24-character name: its temp and set-aside names fit SPIFFS.
    const char* longest = "abcdefghijklmnopqrstuvwx";
    expect("a 24-character name saves", lua_script_cache_write(longest, "-- 1") && lua_script_cache_write(longest, "-- 2"));
    expect("  and reads back", get(longest) == "-- 2");
    expect("  every name the save renames fits 31 characters", g_longest <= 31);
    if (g_longest > 31) std::printf("     (longest %zu)\n", g_longest);

    // Two saves of one script at once, and a reader.
    const std::string a(3000, 'a'), b(3000, 'b');
    expect("first save", lua_script_cache_write("race", a.c_str()));
    std::atomic<bool> stop{false};
    std::atomic<int>  missing{0};
    std::thread reader([&] {
        while (!stop) if (get("race") == "<missing>") missing++;
    });
    std::thread wa([&] { for (int i = 0; i < 300; i++) lua_script_cache_write("race", a.c_str()); });
    std::thread wb([&] { for (int i = 0; i < 300; i++) lua_script_cache_write("race", b.c_str()); });
    wa.join();
    wb.join();
    stop = true;
    reader.join();
    const std::string last = get("race");
    expect("after concurrent saves the script is there, whole", last == a || last == b);
    expect("a reader never found it missing", missing == 0);
    if (missing) std::printf("     (missing %d times)\n", missing.load());

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nscript store OK\n", g_fail);
    return g_fail;
}
