// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// lua_alloc.c — routed allocator for the Lua VM.
//
// Small (< CONFIG_LUA_ENGINE_INTERNAL_SMALL_THRESHOLD) allocations try
// internal RAM first; larger go straight to PSRAM. Hard cap at
// CONFIG_LUA_ENGINE_HEAP_KB * 1024 bytes — exceeding it returns NULL,
// which Lua turns into `not enough memory` and unwinds cleanly.

#include "sdkconfig.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "lua_alloc.h"

static const char* TAG = "lua_alloc";

static atomic_uint_fast32_t s_used_bytes;
static atomic_uint_fast32_t s_peak_bytes;

static size_t const kBudgetBytes =
    (size_t)CONFIG_LUA_ENGINE_HEAP_KB * 1024u;
static size_t const kSmallThreshold =
    (size_t)CONFIG_LUA_ENGINE_INTERNAL_SMALL_THRESHOLD;

// T20 (headroom): hold the script-visible budget a slice below the hard
// cap so the scheduler's own per-dispatch bookkeeping — lua_newthread /
// luaL_ref / pushstring that runs OUTSIDE any pcall frame — is far less
// likely to hit NULL when a script has driven s_used_bytes up toward the
// cap. Pre-T20 an alloc-fail in that unprotected window raised a Lua
// error with no handler → lua panic → abort() → reboot per event. The
// headroom shrinks that window (scripts get a clean `not enough memory`
// once they cross cap-headroom); the lua_atpanic guard in
// lua_scheduler.cpp is the backstop for any fail that still lands
// outside a protected frame. Clamp to a positive value if the budget is
// configured smaller than the headroom (degenerate, test-only).
#define LUA_ALLOC_HEADROOM_BYTES (64u * 1024u)
static size_t const kEffectiveCap =
    (kBudgetBytes > LUA_ALLOC_HEADROOM_BYTES + 4096u)
        ? (kBudgetBytes - LUA_ALLOC_HEADROOM_BYTES)
        : kBudgetBytes;

static inline void bump_used(size_t delta) {
    uint32_t now = atomic_fetch_add(&s_used_bytes, (uint32_t)delta) + delta;
    uint32_t peak = atomic_load(&s_peak_bytes);
    while (now > peak) {
        if (atomic_compare_exchange_weak(&s_peak_bytes, &peak, now)) break;
    }
}

static inline void drop_used(size_t delta) {
    // Saturate at 0. Any accounting bug that drops more than was
    // bumped would otherwise wrap to ~4 GB and trip every subsequent
    // budget check until the VM aborts. CAS loop is fine; this is
    // not a hot path compared to the actual heap op.
    uint32_t cur = atomic_load(&s_used_bytes);
    for (;;) {
        uint32_t next = (cur > (uint32_t)delta) ? (cur - (uint32_t)delta) : 0;
        if (atomic_compare_exchange_weak(&s_used_bytes, &cur, next)) break;
    }
}

// Lua's allocator contract:
//   size == 0 && ptr != NULL  → free(ptr); return NULL
//   size >  0 && ptr == NULL  → malloc(size)
//   size >  0 && ptr != NULL  → realloc(ptr, size)
void* lua_engine_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    (void)ud;

    // Free. Charge `osize` — that's what we tracked when this block
    // was allocated. `heap_caps_get_allocated_size()` returns the
    // allocator's bucket size (e.g. 80-byte alloc rounds to 96), which
    // is ≥ osize and made the counter drift negative under load until
    // it wrapped to ~4 GB and starved the VM ("budget exceeded:
    // cur=4294966420").
    if (nsize == 0) {
        if (ptr) {
            drop_used(osize);
            heap_caps_free(ptr);
        }
        return NULL;
    }

    // Budget check — refuse if this alloc would blow the cap. For a
    // realloc we charge only the delta.
    const size_t delta = (nsize > osize) ? (nsize - osize) : 0;
    if (delta) {
        const uint32_t cur = atomic_load(&s_used_bytes);
        if ((size_t)cur + delta > kEffectiveCap) {
            ESP_LOGW(TAG, "budget exceeded: cur=%u delta=%zu cap=%zu (hard=%zu)",
                     (unsigned)cur, delta, kEffectiveCap, kBudgetBytes);
            return NULL;
        }
    }

    // New alloc.
    if (ptr == NULL) {
        void* out = NULL;
        if (nsize < kSmallThreshold) {
            out = heap_caps_malloc(nsize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!out) {
            out = heap_caps_malloc(nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (out) bump_used(nsize);
        return out;
    }

    // Realloc. heap_caps_realloc preserves the backing heap of the
    // original allocation. Keep that preference; if it can't grow in
    // place we'll get a fresh allocation from the same heap.
    void* out = heap_caps_realloc(ptr, nsize, MALLOC_CAP_8BIT);
    if (!out) {
        // Last-resort: try the other heap explicitly.
        out = heap_caps_malloc(nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) return NULL;
        memcpy(out, ptr, osize < nsize ? osize : nsize);
        heap_caps_free(ptr);
    }

    if (nsize > osize) bump_used(nsize - osize);
    else               drop_used(osize - nsize);
    return out;
}

size_t lua_engine_heap_used_bytes(void) {
    return (size_t)atomic_load(&s_used_bytes);
}

size_t lua_engine_heap_peak_bytes(void) {
    return (size_t)atomic_load(&s_peak_bytes);
}
