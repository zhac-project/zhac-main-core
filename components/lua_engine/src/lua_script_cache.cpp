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
    if (src_len > 16 * 1024) {
        ESP_LOGW(TAG, "script '%s' too large: %zu bytes", name, src_len);
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
    std::fflush(fp);
    std::fclose(fp);
    if (w != src_len) {
        ESP_LOGE(TAG, "short write '%s': %zu/%zu", tmp, w, src_len);
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
