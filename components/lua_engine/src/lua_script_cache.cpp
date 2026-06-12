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
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_spiffs.h"

#include "lua_engine_scripts.h"

static const char* TAG             = "lua_script";
static const char* BASE_PATH       = "/scripts";
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
    mounted = true;
    return true;
}

}  // namespace

extern "C" uint16_t lua_script_cache_list(LuaScriptEntry* out, uint16_t max) {
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
        out[count].size = (stat(path, &st) == 0) ? (uint16_t)st.st_size : 0;
        count++;
    }
    closedir(d);
    return count;
}

extern "C" int lua_script_cache_read(const char* name, char* out, size_t cap) {
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
    if (!lua_script_cache_exists(name) && count_scripts() >= LUA_SCRIPT_MAX) {
        ESP_LOGW(TAG, "script cap reached (%d files) — '%s' rejected",
                 LUA_SCRIPT_MAX, name);
        return false;
    }

    char path[96], tmp[104];
    if (!build_path(path, sizeof(path), name)) return false;
    const int n = std::snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return false;

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
    // SPIFFS's rename() does NOT overwrite an existing destination
    // (unlike POSIX). Unlink the target first if present. This opens
    // a small power-loss window where the destination is gone and the
    // .tmp hasn't been renamed yet — on crash the script reverts to
    // "not present" on next boot, which is the safer failure mode
    // than leaving a corrupt half-written file.
    unlink(path);   // no-op if absent
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "rename '%s' → '%s' failed", tmp, path);
        unlink(tmp);
        return false;
    }
    return true;
}

extern "C" bool lua_script_cache_delete(const char* name) {
    if (!name_ok(name) || !ensure_mounted()) return false;
    char path[96];
    if (!build_path(path, sizeof(path), name)) return false;
    unlink(path);
    return true;
}

extern "C" bool lua_script_cache_exists(const char* name) {
    if (!name_ok(name) || !ensure_mounted()) return false;
    char path[96];
    if (!build_path(path, sizeof(path), name)) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

#endif
