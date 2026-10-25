// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM   0x0400
#define MALLOC_CAP_8BIT     0x0004
#define MALLOC_CAP_INTERNAL 0x0800
static inline void* heap_caps_malloc(size_t s, unsigned) { return malloc(s); }
static inline void* heap_caps_calloc(size_t n, size_t s, unsigned) { return calloc(n, s); }
static inline void  heap_caps_free(void* p) { free(p); }
