// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host stand-in: the "partition" is a plain directory (LUA_SCRIPT_BASE_PATH).
#pragma once
#include <cstddef>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
typedef struct {
    const char* base_path; const char* partition_label; size_t max_files; bool format_if_mount_failed;
} esp_vfs_spiffs_conf_t;
inline esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t*) { return ESP_OK; }
inline esp_err_t esp_spiffs_info(const char*, size_t* total, size_t* used) { *total = 1 << 20; *used = 0; return ESP_OK; }
inline const char* esp_err_to_name(esp_err_t) { return "err"; }
