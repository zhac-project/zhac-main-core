// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// lua_script_cache.cpp — SPIFFS-backed storage for Lua scripts at
// `/scripts/<name>.lua`. Partition label: `scripts`.
//
// Mount is lazy + idempotent (first call formats-if-empty), so the
// engine + HAP handlers don't need an explicit mount step. Writes use
// tmp + rename for atomicity; reads/deletes/list go through the VFS.

#include "sdkconfig.h"

#if CONFIG_LUA_ENGINE_ENABLED

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_spiffs.h"

#include "lua_engine_scripts.h"

static const char* TAG             = "lua_script";
#ifndef LUA_SCRIPT_BASE_PATH
#define LUA_SCRIPT_BASE_PATH "/scripts"   // host tests point it at a directory
#endif
static const char* BASE_PATH       = LUA_SCRIPT_BASE_PATH;
static const char* PARTITION_LABEL = "scripts";

// Generous upper bound for any on-disk path we build. `dirent::d_name`
// in newlib is NAME_MAX (255) + NUL; +BASE_PATH + "/" keeps the
// compiler's -Wformat-truncation analysis happy.
static constexpr size_t BASE_PATH_MAX = 320;

namespace {

bool name_ok(const char* name) {
    if (!name || !*name) return false;
    size_t n = 0;
    for (const char* p = name; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        const bool alnum = (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9');
        if (!alnum && c != '_' && c != '-') return false;
        if (++n > LUA_SCRIPT_NAME_MAX) return false;
    }
    return true;
}

bool build_path(char* out, size_t cap, const char* name) {
    const int n = std::snprintf(out, cap, "%s/%s.lua", BASE_PATH, name);
    return n > 0 && (size_t)n < cap;
}

// F42 (FINDINGS.md): count existing `*.lua` files so write() can enforce
// LUA_SCRIPT_MAX. Cheap dirent scan — no per-file stat (unlike _list).
uint16_t count_scripts() {
    DIR* d = opendir(BASE_PATH);
    if (!d) return 0;
    uint16_t c = 0;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        const size_t flen = std::strlen(ent->d_name);
        if (flen >= 5 && std::strcmp(ent->d_name + flen - 4, ".lua") == 0) c++;
    }
    closedir(d);
    return c;
}

// One-shot lazy mount. `esp_vfs_spiffs_register` returns
// ESP_ERR_INVALID_STATE if someone else already mounted — treat that
// as success for idempotence.
// Every store operation runs under this lock: saves come from the local
// web UI, the cloud relay and rules on different tasks, and a replace is
// several filesystem steps that must not interleave (review F08).
std::mutex s_mtx;

bool exists_unlocked(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// After a power cut in the middle of a replace (lua_script_cache_write):
// `x.bak` without `x.lua` is the old version, put it back; next to `x.lua`
// it is leftover. A `x.tmp` never finished. Runs at mount. v2026101401 named
// them `x.lua.bak` / `x.lua.tmp`; those are sorted out the same way.
void recover_interrupted_saves() {
    DIR* d = opendir(BASE_PATH);
    if (!d) return;
    char names[LUA_SCRIPT_MAX * 2][LUA_SCRIPT_NAME_MAX + 9];   // "<name>.lua.bak"
    int n = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) && n < (int)(sizeof(names) / sizeof(names[0]))) {
        const size_t flen = std::strlen(ent->d_name);
        if (flen >= sizeof(names[0]) || flen < 5) continue;
        if (std::strcmp(ent->d_name + flen - 4, ".bak") == 0 ||
            std::strcmp(ent->d_name + flen - 4, ".tmp") == 0)
            std::memcpy(names[n++], ent->d_name, flen + 1);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char leftover[BASE_PATH_MAX], script[BASE_PATH_MAX];
        const int len  = (int)std::strlen(names[i]);   // < sizeof(names[0]), checked above
        const int base = len - 4;                      // drop ".bak" / ".tmp"
        const bool old_name = base > 4 && std::strncmp(names[i] + base - 4, ".lua", 4) == 0;
        std::snprintf(leftover, sizeof(leftover), "%s/%.*s", BASE_PATH, len, names[i]);
        std::snprintf(script, sizeof(script), "%s/%.*s%s", BASE_PATH, base, names[i], old_name ? "" : ".lua");
        const bool bak = std::strcmp(names[i] + base, ".bak") == 0;
        if (bak && !exists_unlocked(script) && rename(leftover, script) == 0) {
            ESP_LOGW(TAG, "restored '%s' after an interrupted save", script);
            continue;
        }
        unlink(leftover);
    }
}

bool ensure_mounted() {
    static bool mounted = false;
    if (mounted) return true;
    const esp_vfs_spiffs_conf_t conf = {
        .base_path              = BASE_PATH,
        .partition_label        = PARTITION_LABEL,
        .max_files              = 8,
        .format_if_mount_failed = true,
    };
    const esp_err_t e = esp_vfs_spiffs_register(&conf);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mount '%s' failed: %s",
                 PARTITION_LABEL, esp_err_to_name(e));
        return false;
    }
    size_t total = 0, used = 0;
    if (esp_spiffs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted at %s — %zu/%zu bytes used",
                 BASE_PATH, used, total);
    }
    recover_interrupted_saves();
    mounted = true;
    return true;
}

}  // namespace

extern "C" uint16_t lua_script_cache_list(LuaScriptEntry* out, uint16_t max) {
    std::lock_guard<std::mutex> lock(s_mtx);
    if (!out || max == 0 || !ensure_mounted()) return 0;
    DIR* d = opendir(BASE_PATH);
    if (!d) return 0;
    uint16_t count = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) && count < max) {
        const char* fname = ent->d_name;
        const size_t flen = std::strlen(fname);
        if (flen < 5) continue;
        if (std::strcmp(fname + flen - 4, ".lua") != 0) continue;
        const size_t base_len = flen - 4;
        if (base_len == 0 || base_len > LUA_SCRIPT_NAME_MAX) continue;

        std::memcpy(out[count].name, fname, base_len);
        out[count].name[base_len] = '\0';

        // `dirent::d_name` can be up to NAME_MAX (255) bytes. Size
        // the path buffer for that worst case so -Wformat-truncation
        // stays happy.
        char path[BASE_PATH_MAX];
        std::snprintf(path, sizeof(path), "%s/%s", BASE_PATH, fname);
        struct stat st;
        const bool have = stat(path, &st) == 0;
        out[count].size  = have ? (uint16_t)st.st_size : 0;
        // SPIFFS keeps the time of the last write (CONFIG_SPIFFS_USE_MTIME): a real date only when the
        // hub's clock was set then; callers drop values under 1,600,000,000.
        out[count].mtime = have ? (uint32_t)st.st_mtime : 0;
        count++;
    }
    closedir(d);
    return count;
}

extern "C" int lua_script_cache_read(const char* name, char* out, size_t cap) {
    std::lock_guard<std::mutex> lock(s_mtx);
    if (!name_ok(name) || !out || cap == 0 || !ensure_mounted()) return -1;
    char path[96];
    if (!build_path(path, sizeof(path), name)) return -1;
    // T20: stat the file BEFORE reading and FAIL (-1) if it can't fit in
    // `cap` (less the NUL). Previously fread() silently capped at cap-1,
    // so an oversize stored script was handed to the compiler truncated
    // mid-statement — it either failed to parse or, worse, parsed a
    // half-script and ran the wrong code. The write path enforces
    // LUA_SCRIPT_SRC_MAX, so a correctly-stored file always fits a
    // (LUA_SCRIPT_SRC_MAX + 1) buffer; a larger file on disk is
    // corruption and must be rejected, not truncated.
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if ((size_t)st.st_size > cap - 1) {
        ESP_LOGE(TAG, "script '%s' too large for buffer: %ld > %zu — refusing truncated read",
                 name, (long)st.st_size, cap - 1);
        return -1;
    }
    FILE* fp = std::fopen(path, "r");
    if (!fp) return -1;
    const size_t n = std::fread(out, 1, cap - 1, fp);
    std::fclose(fp);
    out[n] = '\0';
    return (int)n;
}

extern "C" bool lua_script_cache_write(const char* name, const char* src) {
    std::lock_guard<std::mutex> lock(s_mtx);
    if (!name_ok(name) || !src || !ensure_mounted()) return false;
    const size_t src_len = std::strlen(src);
    if (src_len > LUA_SCRIPT_SRC_MAX) {   // T20: unified cap (was 16*1024 literal)
        ESP_LOGW(TAG, "script '%s' too large: %zu bytes (max %d)",
                 name, src_len, LUA_SCRIPT_SRC_MAX);
        return false;
    }

    // F42 (FINDINGS.md): enforce LUA_SCRIPT_MAX. Overwriting an existing script
    // is always allowed (count unchanged); only a NEW file is capped, so the
    // scripts partition can't be exhausted by unbounded script count.
    char path[96], tmp[104], bak[104];
    if (!build_path(path, sizeof(path), name)) return false;
    if (!exists_unlocked(path) && count_scripts() >= LUA_SCRIPT_MAX) {
        ESP_LOGW(TAG, "script cap reached (%d files) — '%s' rejected",
                 LUA_SCRIPT_MAX, name);
        return false;
    }

    // The temp file and the set-aside old version are /<name>.tmp and /<name>.bak, not /<name>.lua.tmp:
    // SPIFFS object names hold at most 31 characters (CONFIG_SPIFFS_OBJ_NAME_LEN=32 with the NUL), so a
    // 23- or 24-character name could not be saved (spec 2026-10-05 §3.8). list/count only look at *.lua,
    // so neither is ever a script.
    const int n = std::snprintf(tmp, sizeof(tmp), "%s/%s.tmp", BASE_PATH, name);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return false;
    std::snprintf(bak, sizeof(bak), "%s/%s.bak", BASE_PATH, name);   // same length as tmp

    FILE* fp = std::fopen(tmp, "w");
    if (!fp) {
        ESP_LOGE(TAG, "fopen '%s' failed", tmp);
        return false;
    }
    const size_t w = std::fwrite(src, 1, src_len, fp);
    // T20: a buffered fwrite can report the full count yet still fail to
    // commit at flush/close time when the SPIFFS partition is full. Check
    // fflush AND fclose return codes BEFORE the rename so a failed flush
    // can't promote a truncated/empty .tmp into place reporting success.
    const bool flush_ok = (std::fflush(fp) == 0);
    const bool close_ok = (std::fclose(fp) == 0);   // closes fp on success AND failure
    if (w != src_len || !flush_ok || !close_ok) {
        ESP_LOGE(TAG, "write '%s' failed: w=%zu/%zu flush=%d close=%d",
                 tmp, w, src_len, (int)flush_ok, (int)close_ok);
        unlink(tmp);
        return false;
    }
    // SPIFFS's rename() does NOT overwrite an existing destination (unlike
    // POSIX). The old version steps aside as .bak instead of being deleted,
    // so a power cut at any point leaves the old or the new script, and
    // recover_interrupted_saves() sorts it out at the next mount (review F21).
    const bool had = exists_unlocked(path);
    if (had) {
        unlink(bak);   // leftover, if any
        if (rename(path, bak) != 0) {
            ESP_LOGE(TAG, "rename '%s' → '%s' failed", path, bak);
            unlink(tmp);
            return false;
        }
    }
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "rename '%s' → '%s' failed", tmp, path);
        if (had) rename(bak, path);   // the old version stays
        unlink(tmp);
        return false;
    }
    if (had) unlink(bak);
    return true;
}

extern "C" bool lua_script_cache_delete(const char* name) {
    std::lock_guard<std::mutex> lock(s_mtx);
    if (!name_ok(name) || !ensure_mounted()) return false;
    char path[96];
    if (!build_path(path, sizeof(path), name)) return false;
    unlink(path);
    return true;
}

extern "C" bool lua_script_cache_exists(const char* name) {
    std::lock_guard<std::mutex> lock(s_mtx);
    if (!name_ok(name) || !ensure_mounted()) return false;
    char path[96];
    if (!build_path(path, sizeof(path), name)) return false;
    return exists_unlocked(path);
}

#endif
