// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stand-in: every capability is plain malloc.
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_8BIT   (1 << 2)
static inline void* heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
