// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host stand-in: no partitions. The stub mount always succeeds, so the
// blank-partition check behind a failed mount is never reached here.
#pragma once
#include <cstddef>
#include <cstdint>
typedef enum { ESP_PARTITION_TYPE_DATA = 0x01 } esp_partition_type_t;
typedef enum { ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82 } esp_partition_subtype_t;
typedef struct { size_t size; } esp_partition_t;
inline const esp_partition_t* esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char*) {
    return nullptr;
}
inline int esp_partition_read(const esp_partition_t*, size_t, void*, size_t) { return -1; }
