// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// lua_scheduler.c — TaskLua + resume queue + timer pool.
//
// Single-threaded: TaskLua is the only task that ever touches g_L —
// including the boot-time script load, which rides MSG_LOAD_ALL.
// External code reaches Lua by pushing a ResumeMsg onto s_resume_q;
// the scheduler dequeues, resumes the target coroutine, and on
// yield/finish updates the registry-held reference.
//
// Sleep is implemented via `zhac.sleep(ms)` → lua_yield + esp_timer →
// queue push on fire. The VM sees a plain yield/resume; the caller
// never blocks outside Lua.

#include "sdkconfig.h"

#if CONFIG_LUA_ENGINE_ENABLED

#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

extern "C" {
#include "lua_alloc.h"
#include "lua_engine.h"
#include "lua_engine_scripts.h"
#include "lua_internal.h"
#include "lua_scheduler.h"
}
#include "metrics/metrics_macros.h"
// T20: derive the ZclAttrEvent payload layout from the canonical schema
// constants instead of hardcoded offsets. event_bus schema v6 widened
// ATTR_KEY_MAX 20→28, which slid the value union from offset 36 to 44;
// the old literals silently fed on_attr_change handlers 0/garbage ints
// and truncated key/value strings. Pull the header so the offsets track
// the struct automatically if the schema moves again.
#include "zcl_attribute.h"
#include "event_bus.h"

static const char* TAG = "lua_sched";

// ── Panic backstop (T20) ──────────────────────────────────────────────
// The scheduler touches the VM from unprotected frames on TaskLua:
// lua_newthread / luaL_ref / lua_pushstring in spawn_coroutine and the
// push_event_args / run_named_script marshalling run with NO pcall above
// them. If the budget allocator returns NULL there (script drove the
// heap to the cap), Lua raises "not enough memory" with no handler to
// catch it and calls the panic function — whose default is abort(), i.e.
// every such event becomes a chip reboot.
//
// Fix: install a lua_atpanic handler that longjmps back to a setjmp
// frame wrapped around each dispatch step in task_lua (the standard
// embedded "panic = unwind one step, keep the VM" pattern). The bad
// dispatch is abandoned and counted; TaskLua loops and serves the next
// message. The VM stays up. NOTE: longjmp out of a panic leaves the
// offending lua_State's stack unbalanced, but we only ever longjmp on a
// genuinely unrecoverable raise (OOM/internal) where that thread is
// being discarded anyway — g_L itself is not left mid-API-call because
// the panic fires synchronously inside the C API call we abandon.
static std::jmp_buf s_panic_jmp;
static volatile bool s_panic_armed = false;

static int lua_panic_handler(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    ESP_LOGE(TAG, "lua panic (unprotected error) — abandoning dispatch step: %s",
             msg ? msg : "(no message)");
    lua_scheduler_error_bump();
    if (s_panic_armed) {
        s_panic_armed = false;
        std::longjmp(s_panic_jmp, 1);   // unwind to task_lua's per-step frame
    }
    // No frame armed (shouldn't happen on TaskLua) — fall through to the
    // default behaviour (abort) rather than returning, which Lua treats
    // as "panic handler returned" and aborts anyway.
    return 0;
}

// ── CPU runaway guard (LUA-F1) ────────────────────────────────────────
// Single-threaded TaskLua ⇒ one tight loop wedges every coroutine and
// the event bridge. We arm a count hook before each lua_resume; the
// hook compares esp_timer_get_time() against a deadline and raises a
// Lua error when the deadline passes. The script's per-resume budget
// is bounded by LUA_RESUME_DEADLINE_US — generous enough to allow real
// work, tight enough that a misbehaving handler cannot starve other
// coroutines for more than ~50 ms.
static constexpr int64_t LUA_RESUME_DEADLINE_US = 50 * 1000;
// F5 (FINDINGS.md): hard ceiling. Once a resume blows this (e.g. a script
// wrapping a tight loop in pcall to swallow the soft-deadline error), the
// hook switches to firing every instruction and keeps raising WITHOUT
// resetting the deadline, so the runaway makes essentially no forward
// progress and the error keeps being logged/counted. NOTE: stock Lua
// cannot make a hook error truly uncatchable, so a pcall-guarded infinite
// loop within a SINGLE resume still occupies TaskLua until it returns — a
// known residual (a true abort needs a Lua patch or a supervised kill-task).
static constexpr int64_t LUA_RESUME_HARD_US     = 250 * 1000;
static constexpr int     LUA_RESUME_HOOK_COUNT  = 1000;
static int64_t           s_resume_deadline_us   = 0;
static int64_t           s_resume_hard_us       = 0;
static bool              s_resume_killed        = false;

static void lua_resume_count_hook(lua_State* L, lua_Debug*) {
    const int64_t now = esp_timer_get_time();
    if (now < s_resume_deadline_us) return;
    if (!s_resume_killed && now >= s_resume_hard_us) {
        // Escalate: fire every instruction and keep raising. Not resetting
        // the deadline means a pcall that catches the error can no longer
        // buy another full LUA_RESUME_HOOK_COUNT window of progress.
        s_resume_killed = true;
        lua_sethook(L, lua_resume_count_hook, LUA_MASKCOUNT, 1);
    }
    luaL_error(L, "lua_engine: resume exceeded %lld us budget",
               (long long)LUA_RESUME_DEADLINE_US);
}

static void arm_resume_deadline(lua_State* co) {
    const int64_t now    = esp_timer_get_time();
    s_resume_deadline_us = now + LUA_RESUME_DEADLINE_US;
    s_resume_hard_us     = now + LUA_RESUME_HARD_US;
    s_resume_killed      = false;
    lua_sethook(co, lua_resume_count_hook,
                LUA_MASKCOUNT, LUA_RESUME_HOOK_COUNT);
}

// ── Timer pool ────────────────────────────────────────────────────────
typedef struct {
    esp_timer_handle_t handle;
    int                coroutine_ref;   // -1 = free
    bool               in_use;
} TimerSlot;

static TimerSlot s_timer_pool[CONFIG_LUA_ENGINE_TIMER_POOL];

// ── Resume queue ──────────────────────────────────────────────────────
//
// Message kinds sharing one queue:
//  - MSG_RESUME:    resume an existing coroutine by registry ref.
//  - MSG_EVENT:     iterate a REG_ON_* table and spawn a coroutine per
//                   handler, passing unpacked event fields.
//  - MSG_RUN_NAMED: compile + run one named script from the cache.
//  - MSG_LOAD_ALL:  boot-time pass — compile + start every stored
//                   script. No payload.
enum LuaMsgKind : uint8_t {
    MSG_RESUME    = 0,
    MSG_EVENT     = 1,
    MSG_RUN_NAMED = 2,
    MSG_LOAD_ALL  = 3,
};

enum LuaEventKind : uint8_t {
    LUA_EVT_BOOT = 0,
    LUA_EVT_ATTR = 1,
    LUA_EVT_MQTT = 2,
    LUA_EVT_RAW  = 3,   // ZCL_RAW — frame ZHC didn't decode
};

typedef struct {
    int coroutine_ref;
    int argc;
    int argv[4];
} ResumeArgs;

typedef struct {
    LuaEventKind kind;
    uint8_t      payload[96];   // mirrors EventBus Event::data layout
} EventArgs;

typedef struct {
    char     name[32];            // script identifier (no extension)
    char     value[64];           // stringified trigger value; "" when unused
    // T20: size key/str_val from the schema-v6 constants, not the old
    // 20/32 literals — an attr key up to ATTR_KEY_MAX or a VAL_STR up to
    // ATTR_STR_MAX was being truncated before reaching the handler.
    char     key[ATTR_KEY_MAX];   // attr name; "" for non-attr triggers
    char     str_val[ATTR_STR_MAX]; // raw VAL_STR value; "" otherwise
    uint64_t ieee;                // source device IEEE; 0 when absent
    uint16_t cluster;             // ZCL cluster id; 0 when absent
    uint16_t attr_id;             // ZCL attribute id; 0 when absent
    uint8_t  val_type;            // ValType enum
    int32_t  int_val;             // raw VAL_INT/BOOL value
} RunNamedArgs;

typedef struct {
    LuaMsgKind kind;
    union {
        ResumeArgs   resume;
        EventArgs    event;
        RunNamedArgs named;
    };
} LuaMsg;

static QueueHandle_t s_resume_q;

// ── Counters (published via metrics + public API) ─────────────────────
static std::atomic<std::uint_fast16_t> s_live_coroutines;
static std::atomic<std::uint_fast32_t> s_error_count;
static std::atomic<std::uint_fast32_t> s_yield_count;

extern "C" void lua_scheduler_coroutine_enter(void) {
    const uint32_t n = s_live_coroutines.fetch_add(1) + 1;
    _METRIC_VALUE(METRIC_LUA_COROUTINES_LIVE, (int64_t)n);
}

// F5 (FINDINGS.md): enforce CONFIG_LUA_ENGINE_MAX_COROUTINES, which was
// previously decorative (referenced only in a log line). Checked at every
// spawn site before lua_newthread so unbounded coroutine growth can't
// exhaust the Lua heap.
extern "C" bool lua_scheduler_at_capacity(void) {
    return s_live_coroutines.load() >= (uint32_t)CONFIG_LUA_ENGINE_MAX_COROUTINES;
}

extern "C" void lua_scheduler_coroutine_exit(void)  {
    const uint32_t n = s_live_coroutines.fetch_sub(1) - 1;
    _METRIC_VALUE(METRIC_LUA_COROUTINES_LIVE, (int64_t)n);
    // Also sample heap usage on every exit — cheap and captures the
    // common "script ran, freed everything" signal.
    _METRIC_VALUE(METRIC_LUA_HEAP_USED_BYTES, (int64_t)lua_engine_heap_used_bytes());
    _METRIC_VALUE(METRIC_LUA_HEAP_PEAK_BYTES, (int64_t)lua_engine_heap_peak_bytes());
}

extern "C" uint16_t lua_scheduler_live_count(void)  { return (uint16_t)s_live_coroutines.load(); }
extern "C" uint32_t lua_scheduler_error_count(void) { return (uint32_t)s_error_count.load(); }
extern "C" uint32_t lua_scheduler_yield_count(void) { return (uint32_t)s_yield_count.load(); }

extern "C" void lua_scheduler_error_bump(void) {
    s_error_count.fetch_add(1);
    _METRIC_COUNTER_INC(METRIC_LUA_ERRORS_TOTAL, 1);
}
extern "C" void lua_scheduler_yield_bump(void) {
    s_yield_count.fetch_add(1);
    _METRIC_COUNTER_INC(METRIC_LUA_YIELDS_TOTAL, 1);
}

// ── Timer callback → resume queue push ────────────────────────────────
static void on_timer_fire(void* arg) {
    TimerSlot* slot = (TimerSlot*)arg;
    if (!slot || !slot->in_use) return;
    LuaMsg m = {};
    m.kind                 = MSG_RESUME;
    m.resume.coroutine_ref = slot->coroutine_ref;
    // Runs on the esp_timer TASK (dispatch_method = ESP_TIMER_TASK above), not
    // an ISR — a plain xQueueSend is correct (the old xQueueSendFromISR was a
    // latent mismatch). F29 (FINDINGS.md): only release the slot once the
    // resume is actually enqueued. If the queue is full, DON'T drop it — that
    // would leak the coroutine's registry ref (never resumed, never unref'd)
    // and pin a timer slot + coroutine forever. Keep the slot and re-arm to
    // retry shortly; the ref rides along in the slot until TaskLua drains.
    if (xQueueSend(s_resume_q, &m, 0) == pdTRUE) {
        // T20 (slot store order): clear coroutine_ref BEFORE releasing
        // in_use. on_timer_fire runs on the esp_timer task while
        // slot_acquire runs on TaskLua; if in_use dropped first, a
        // cross-task slot_acquire between the two stores would write the
        // new ref and then this store would clobber it back to -1,
        // orphaning the freshly-parked coroutine. Ref-then-flag closes
        // the window (the slot isn't claimable until in_use is false).
        slot->coroutine_ref = -1;
        slot->in_use        = false;
    } else {
        ESP_LOGD(TAG, "resume queue full — re-arming timer for ref %d",
                 slot->coroutine_ref);
        // P5 (FINDINGS §5): if the re-arm itself fails the slot + registry
        // ref strand forever (never resumed, never unref'd) — and the old
        // code dropped the esp_err silently. We cannot unref here (the
        // ref belongs to TaskLua's lua_State, not the esp_timer task), so
        // the safe recovery is to make the strand loud. Escalate to ERROR
        // so a wedged sleep slot is diagnosable rather than invisible.
        const esp_err_t rearm = esp_timer_start_once(slot->handle, 10 * 1000ULL);
        if (rearm != ESP_OK) {
            ESP_LOGE(TAG, "timer re-arm failed (%s) — sleep slot stranded for "
                          "ref %d", esp_err_to_name(rearm), slot->coroutine_ref);
        }
    }
}

static TimerSlot* slot_acquire(int coroutine_ref) {
    for (size_t i = 0; i < CONFIG_LUA_ENGINE_TIMER_POOL; ++i) {
        if (!s_timer_pool[i].in_use) {
            s_timer_pool[i].in_use        = true;
            s_timer_pool[i].coroutine_ref = coroutine_ref;
            if (!s_timer_pool[i].handle) {
                const esp_timer_create_args_t args = {
                    .callback         = on_timer_fire,
                    .arg              = &s_timer_pool[i],
                    .dispatch_method  = ESP_TIMER_TASK,
                    .name             = "lua_sleep",
                    .skip_unhandled_events = false,
                };
                if (esp_timer_create(&args, &s_timer_pool[i].handle) != ESP_OK) {
                    s_timer_pool[i].in_use = false;
                    return NULL;
                }
            }
            return &s_timer_pool[i];
        }
    }
    return NULL;
}

// Exposed to zhac.sleep so the native binding can schedule without
// looking at scheduler internals.
extern "C" bool lua_scheduler_sleep(int coroutine_ref, uint32_t delay_ms) {
    TimerSlot* s = slot_acquire(coroutine_ref);
    if (!s) return false;
    esp_timer_start_once(s->handle, (uint64_t)delay_ms * 1000ULL);
    return true;
}

// Exposed to anyone wanting to push a resume from outside TaskLua.
// Safe from ISR or normal task.
extern "C" bool lua_scheduler_push_resume(int coroutine_ref) {
    LuaMsg m = {};
    m.kind                 = MSG_RESUME;
    m.resume.coroutine_ref = coroutine_ref;
    bool ok = xQueueSend(s_resume_q, &m, 0) == pdTRUE;
    if (!ok) _METRIC_COUNTER_INC(METRIC_LUA_QUEUE_DROPS_TOTAL, 1);
    return ok;
}

// Public one-arg entry for clients that don't need trigger context
// (e.g. the UI Run button). Declared in `lua_engine_scripts.h`.
extern "C" bool lua_engine_run_script(const char* name) {
    return lua_scheduler_push_run_named(name, nullptr);
}

extern "C" bool lua_scheduler_push_run_named(const char* name,
                                               const LuaScriptInvokeArgs* args) {
    if (!name) return false;
    LuaMsg m = {};
    m.kind = MSG_RUN_NAMED;
    std::strncpy(m.named.name, name, sizeof(m.named.name) - 1);
    if (args) {
        if (args->value)
            std::strncpy(m.named.value, args->value,
                         sizeof(m.named.value) - 1);
        if (args->key)
            std::strncpy(m.named.key, args->key,
                         sizeof(m.named.key) - 1);
        if (args->str_val)
            std::strncpy(m.named.str_val, args->str_val,
                         sizeof(m.named.str_val) - 1);
        m.named.ieee     = args->ieee;
        m.named.cluster  = args->cluster;
        m.named.attr_id  = args->attr_id;
        m.named.val_type = args->val_type;
        m.named.int_val  = args->int_val;
    }
    bool ok = xQueueSend(s_resume_q, &m, 0) == pdTRUE;
    if (!ok) _METRIC_COUNTER_INC(METRIC_LUA_QUEUE_DROPS_TOTAL, 1);
    return ok;
}

extern "C" bool lua_scheduler_push_event(uint8_t event_kind,
                                           const void* payload_96) {
    LuaMsg m = {};
    m.kind = MSG_EVENT;
    m.event.kind = (LuaEventKind)event_kind;
    if (payload_96) {
        memcpy(m.event.payload, payload_96, 96);
    }
    // From EventBus context — the event_bus task isn't an ISR, so a
    // normal send is correct. Use 0 timeout; if TaskLua is behind we
    // drop the event rather than blocking the publisher.
    bool ok = xQueueSend(s_resume_q, &m, 0) == pdTRUE;
    if (!ok) _METRIC_COUNTER_INC(METRIC_LUA_QUEUE_DROPS_TOTAL, 1);
    return ok;
}

// Boot-time script load. lua_engine_load_all() pushes this instead of
// compiling + spawning on its caller's task: TaskLua may already be
// resuming earlier coroutines on the same lua_State, and the VM has no
// internal locking — every touch must come from TaskLua. Called once
// from app_main strictly after lua_engine_init() succeeded (which is
// what creates s_resume_q), the same init-before-push ordering every
// other push_* relies on. Returns false when the queue is full.
extern "C" bool lua_scheduler_push_load_all(void) {
    LuaMsg m = {};
    m.kind = MSG_LOAD_ALL;
    bool ok = xQueueSend(s_resume_q, &m, 0) == pdTRUE;
    if (!ok) _METRIC_COUNTER_INC(METRIC_LUA_QUEUE_DROPS_TOTAL, 1);
    return ok;
}

// ── Coroutine driver ──────────────────────────────────────────────────
//
// Registry-ref ownership map — keep in sync with zhac_lua_module.cpp:
//
//   spawn ref   taken by spawn_coroutine() (dispatch_event /
//               run_named_script / load_one_script /
//               lua_scheduler_spawn (currently unused)) to anchor the
//               fresh thread while it runs. Settled by
//               resume_and_settle() on every outcome of the resume it
//               drives (for lua_scheduler_spawn the ref rides a
//               MSG_RESUME and is settled by resume_coroutine).
//   sleep ref   zhac.sleep takes a FRESH ref to the running thread on
//               every call, parks it in a TimerSlot, then yields. From
//               that point it is the single owner of the suspended
//               coroutine; on timer fire it rides MSG_RESUME into
//               resume_coroutine and is settled there.
//
// Invariant: exactly ONE live registry ref per suspended coroutine —
// the one held by whatever will resume it. Whoever calls lua_resume
// must release the ref it came in with on ALL paths: LUA_OK / error
// (thread finished — unref + live-count exit) and LUA_YIELD too,
// because the continuation armed during the yield holds its own fresh
// ref. Known exception: a script calling bare `coroutine.yield()`
// with no sleep slot armed gets its ref dropped all the same — the
// thread becomes GC-able and is never resumed (orphaned by design;
// its live-count slot stays — pre-existing drift; see lua_sandbox.c,
// which keeps coroutine.yield exposed).

// Drives one lua_resume step of `co` (nargs args already pushed on co)
// and settles `ref`, the registry ref the caller holds on the thread.
// The ref is released on EVERY path; after a LUA_YIELD the thread
// stays anchored by the fresh ref the yield path (zhac.sleep) took
// before yielding. `err_ctx` prefixes the error log so each call site
// keeps its existing message.
static void resume_and_settle(lua_State* L, lua_State* co, int ref,
                              int nargs, const char* err_ctx) {
    int nres = 0;
    arm_resume_deadline(co);
    const int status = lua_resume(co, L, nargs, &nres);

    if (status == LUA_YIELD) {
        lua_pop(co, nres);
        lua_scheduler_yield_bump();
        // The continuation (sleep timer slot) owns its own ref now;
        // ours is redundant — dropping it is what keeps the registry
        // at one slot per suspended coroutine.
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return;
    }
    if (status != LUA_OK) {
        const char* err = lua_tostring(co, -1);
        ESP_LOGE(TAG, "%s: %s", err_ctx, err ? err : "(no message)");
        lua_scheduler_error_bump();
    }
    lua_pop(co, (status == LUA_OK) ? nres : 1);
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    lua_scheduler_coroutine_exit();
}

// Spawns a coroutine from the function on top of L's stack (consumed)
// and anchors it with a fresh registry ref stored in *out_ref. Returns
// NULL on registry exhaustion (function gone, no live-count taken).
// Capacity checks stay at the call sites — message and control flow
// differ per site.
static lua_State* spawn_coroutine(lua_State* L, int* out_ref) {
    lua_State* co = lua_newthread(L);   // [..., fn, co]
    lua_insert(L, -2);                  // [..., co, fn]
    lua_xmove(L, co, 1);                // fn → co. L: [..., co]
    *out_ref = luaL_ref(L, LUA_REGISTRYINDEX);   // consumes co
    if (*out_ref == LUA_NOREF) return NULL;
    lua_scheduler_coroutine_enter();
    return co;
}

static void resume_coroutine(lua_State* L, const ResumeArgs* r) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, r->coroutine_ref);
    lua_State* co = lua_tothread(L, -1);
    lua_pop(L, 1);
    if (!co) {
        ESP_LOGW(TAG, "resume: ref %d is not a thread", r->coroutine_ref);
        luaL_unref(L, LUA_REGISTRYINDEX, r->coroutine_ref);
        return;
    }

    for (int i = 0; i < r->argc; ++i) lua_pushinteger(co, r->argv[i]);

    resume_and_settle(L, co, r->coroutine_ref, r->argc, "coroutine error");
}

// Marshal event fields onto a coroutine's stack. Returns argc.
static int push_event_args(lua_State* co, const EventArgs* e) {
    switch (e->kind) {
        case LUA_EVT_BOOT:
            // Push an empty table so handlers written as
            //   function(ev) print(ev.foo) end
            // get a real first arg and can index it safely (yields nil)
            // instead of throwing "attempt to index a nil value". The
            // boot event carries no payload, so the table is empty by
            // design — handlers wanting boot-specific fields should
            // expect to find none.
            lua_newtable(co);
            return 1;
        case LUA_EVT_ATTR: {
            // ZclAttrEvent layout (packed, 96 B, schema v6): ieee(8),
            // nwk(2), ep(1), val_type(1), cluster(2), attr_id(2),
            // key[ATTR_KEY_MAX=28], union{int32_t int_val | char
            // str_val[ATTR_STR_MAX=48]}, _pad(4). T20: read every field
            // through offsetof on the canonical struct so the value union
            // is at offset 44 (v6) — the previous hardcoded 36 was the v5
            // offset (key was [20]) and fed handlers garbage after the
            // schema widened. The struct is the single source of truth.
            const uint8_t* p = e->payload;
            uint64_t ieee;    memcpy(&ieee,    p + offsetof(ZclAttrEvent, ieee),    8);
            uint16_t cluster; memcpy(&cluster, p + offsetof(ZclAttrEvent, cluster), 2);
            uint16_t attr_id; memcpy(&attr_id, p + offsetof(ZclAttrEvent, attr_id), 2);
            const char* key = (const char*)(p + offsetof(ZclAttrEvent, key));
            const uint8_t val_type = p[offsetof(ZclAttrEvent, val_type)];
            const size_t  val_off  = offsetof(ZclAttrEvent, int_val);  // == str_val (union)
            // val_type: 0=INT, 1=BOOL, 2=STR (see attr_keys.h).
            char ieee_hex[20];
            snprintf(ieee_hex, sizeof(ieee_hex), "0x%016llx",
                     (unsigned long long)ieee);
            lua_pushstring(co, ieee_hex);
            // P5 (FINDINGS §2, T2): `key` and the VAL_STR value are read
            // straight out of the raw 96-byte event-bus payload. The
            // canonical producers NUL-terminate (zcl_attribute.h), but the
            // event-bus transport is generic — a non-canonical producer
            // that fills the whole field would leave lua_pushstring (which
            // walks to a NUL) over-reading past the field into adjacent
            // payload bytes. Bound both pushes by the field size.
            lua_pushlstring(co, key, strnlen(key, ATTR_KEY_MAX));
            if (val_type == 2) {
                const char* sv = (const char*)(p + val_off);
                lua_pushlstring(co, sv, strnlen(sv, ATTR_STR_MAX));
            } else if (val_type == 1) {
                int32_t v; memcpy(&v, p + val_off, 4);
                lua_pushboolean(co, v != 0);
            } else {
                int32_t v; memcpy(&v, p + val_off, 4);
                lua_pushinteger(co, v);
            }
            lua_pushinteger(co, cluster);
            lua_pushinteger(co, attr_id);
            return 5;
        }
        case LUA_EVT_MQTT: {
            // MqttMsgEvent: topic[64], payload[32].
            const char* topic   = (const char*)(e->payload + 0);
            const char* payload = (const char*)(e->payload + 64);
            lua_pushstring(co, topic);
            lua_pushstring(co, payload);
            return 2;
        }
        case LUA_EVT_RAW: {
            // ZclRawEvent (packed, 96 B): ieee(8), nwk(2), ep(1),
            // command(1), cluster(2), payload_len(1), _pad(1),
            // payload_hex[80].
            const uint8_t* p = e->payload;
            uint64_t ieee;    memcpy(&ieee,    p + 0,  8);
            uint16_t nwk;     memcpy(&nwk,     p + 8,  2);
            const uint8_t  ep      = p[10];
            const uint8_t  command = p[11];
            uint16_t cluster; memcpy(&cluster, p + 12, 2);
            const uint8_t  plen    = p[14];
            const char*    hex     = (const char*)(p + 16);
            char ieee_hex[20];
            snprintf(ieee_hex, sizeof(ieee_hex), "0x%016llx",
                     (unsigned long long)ieee);
            // Single table arg — too many fields for positional. Lua
            // handler reads `ev.cluster`, `ev.hex`, etc.
            lua_newtable(co);
            lua_pushstring(co, ieee_hex);   lua_setfield(co, -2, "ieee");
            lua_pushinteger(co, nwk);       lua_setfield(co, -2, "nwk");
            lua_pushinteger(co, ep);        lua_setfield(co, -2, "ep");
            lua_pushinteger(co, cluster);   lua_setfield(co, -2, "cluster");
            lua_pushinteger(co, command);   lua_setfield(co, -2, "command");
            lua_pushinteger(co, plen);      lua_setfield(co, -2, "len");
            lua_pushstring(co, hex);        lua_setfield(co, -2, "hex");
            return 1;
        }
    }
    return 0;
}

static const char* reg_key_for(LuaEventKind k) {
    switch (k) {
        case LUA_EVT_BOOT: return "zhac_on_boot_refs";
        case LUA_EVT_ATTR: return "zhac_on_attr_refs";
        case LUA_EVT_MQTT: return "zhac_on_mqtt_refs";
        case LUA_EVT_RAW:  return "zhac_on_raw_refs";
    }
    return NULL;
}

// Iterate the registered handler table for this event, spawning one
// coroutine per handler with event-specific args pushed.
static void dispatch_event(lua_State* L, const EventArgs* e) {
    const char* key = reg_key_for(e->kind);
    if (!key) return;

    lua_getfield(L, LUA_REGISTRYINDEX, key);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    const lua_Integer n = luaL_len(L, -1);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, -1, i);    // push handler fn
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }

        if (lua_scheduler_at_capacity()) {
            lua_pop(L, 1);     // drop handler fn
            ESP_LOGW(TAG, "lua: coroutine cap %d reached — skipping handlers",
                     CONFIG_LUA_ENGINE_MAX_COROUTINES);
            break;
        }

        int ref = LUA_NOREF;
        lua_State* co = spawn_coroutine(L, &ref);   // consumes fn
        if (!co) continue;

        const int argc = push_event_args(co, e);

        // Run the coroutine inline. Args are already on co; pass argc
        // as nargs so Lua hands them to the function. Our spawn ref is
        // settled on every outcome — on yield, zhac.sleep's own ref
        // takes over and the timer slot pushes the resume when ready.
        resume_and_settle(L, co, ref, argc, "handler error");
    }
    lua_pop(L, 1);                       // handler table
}

// Load a named script from the SPIFFS cache, compile as text, run it as
// a fresh coroutine. The coroutine always receives a single table arg
// with the trigger context ({value, key, ieee, cluster, attr_id,
// val_type, int_val, str_val}); fields are zero / "" for non-device
// triggers. Returns after the coroutine either yields or terminates.
static void run_named_script(lua_State* L, const RunNamedArgs* r) {
    // Script source scratch lives in PSRAM. Lazy one-shot init. T20:
    // size to LUA_SCRIPT_SRC_MAX (+1 NUL) — the unified store cap — so a
    // legitimately large stored script isn't silently truncated at an 8
    // KB buffer that was smaller than the write limit.
    constexpr size_t kBufSz = LUA_SCRIPT_SRC_MAX + 1;
    static char* buf = nullptr;
    if (!buf) {
        buf = static_cast<char*>(
            heap_caps_malloc(kBufSz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!buf) buf = static_cast<char*>(heap_caps_malloc(kBufSz, MALLOC_CAP_8BIT));
        if (!buf) { ESP_LOGE(TAG, "run_named: buf alloc failed"); return; }
    }
    const int n = lua_script_cache_read(r->name, buf, kBufSz);
    if (n < 0) {
        ESP_LOGW(TAG, "script.run '%s' — script not found in cache", r->name);
        return;
    }
    if (luaL_loadbufferx(L, buf, (size_t)n, r->name, "t") != LUA_OK) {
        ESP_LOGE(TAG, "script.run '%s' compile error: %s",
                 r->name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    // Stack: [fn]. Wrap in a coroutine so yields (zhac.sleep etc.)
    // work and the caller doesn't block TaskLua.
    if (lua_scheduler_at_capacity()) {
        ESP_LOGW(TAG, "lua: coroutine cap %d reached — dropping script.run '%s'",
                 CONFIG_LUA_ENGINE_MAX_COROUTINES, r->name);
        lua_pop(L, 1);                                 // drop compiled fn
        return;
    }
    int ref = LUA_NOREF;
    lua_State* co = spawn_coroutine(L, &ref);          // consumes fn
    if (!co) return;

    // Push a single event-context table as the call argument. Callers
    // that only care about the stringified value still get it via
    // `ev.value`; `ON device DO script.run "..."` handlers can inspect
    // `ev.key` / `ev.int_val` etc. to route on the firing attr.
    lua_newtable(co);
    lua_pushstring(co, r->value);       lua_setfield(co, -2, "value");
    lua_pushstring(co, r->key);         lua_setfield(co, -2, "key");
    lua_pushstring(co, r->str_val);     lua_setfield(co, -2, "str_val");
    lua_pushinteger(co, (lua_Integer)r->ieee);     lua_setfield(co, -2, "ieee");
    lua_pushinteger(co, (lua_Integer)r->cluster);  lua_setfield(co, -2, "cluster");
    lua_pushinteger(co, (lua_Integer)r->attr_id);  lua_setfield(co, -2, "attr_id");
    lua_pushinteger(co, (lua_Integer)r->val_type); lua_setfield(co, -2, "val_type");
    lua_pushinteger(co, (lua_Integer)r->int_val);  lua_setfield(co, -2, "int_val");
    const int argc = 1;

    char err_ctx[48];   // "lua.run '<name≤31>' error" — worst case 47 + NUL
    snprintf(err_ctx, sizeof(err_ctx), "lua.run '%s' error", r->name);
    resume_and_settle(L, co, ref, argc, err_ctx);
}

// ── Boot-time load-all (MSG_LOAD_ALL) ─────────────────────────────────
//
// Body of lua_engine_load_all(). It ran on app_main until the P0
// findings review: app_main compiled + spawned stored scripts on g_L
// while TaskLua — unpinned, genuinely parallel on the dual-core P4 —
// could already be resuming script 1's coroutine (its spawn pushed an
// immediate MSG_RESUME). Two tasks inside one unlocked lua_State
// corrupt the VM as soon as a second stored script exists. The pass
// now rides MSG_LOAD_ALL and executes here, on TaskLua, like every
// other lua_State touch.

// Compile + run one cached script as a top-level coroutine. Each file
// gets its own coroutine so an error or `zhac.sleep` in one script
// doesn't block the others.
static bool load_one_script(lua_State* L, const char* name) {
    // Park script-load scratch in PSRAM. One-shot lazy init, never freed
    // — same lifetime as the original BSS-static array. T20: size from
    // LUA_SCRIPT_SRC_MAX (+1 NUL), the unified store cap (was a bare
    // 16 KB literal).
    constexpr size_t kBufSz = LUA_SCRIPT_SRC_MAX + 1;
    static char* buf = nullptr;
    if (!buf) {
        buf = static_cast<char*>(
            heap_caps_malloc(kBufSz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!buf) buf = static_cast<char*>(heap_caps_malloc(kBufSz, MALLOC_CAP_8BIT));
        if (!buf) {
            ESP_LOGE(TAG, "load_one_script: buf alloc failed");
            return false;
        }
    }
    const int n = lua_script_cache_read(name, buf, kBufSz);
    if (n < 0) {
        ESP_LOGW(TAG, "script '%s' not readable", name);
        return false;
    }
    if (luaL_loadbufferx(L, buf, (size_t)n, name, "t") != LUA_OK) {
        ESP_LOGE(TAG, "compile '%s': %s", name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    if (lua_scheduler_at_capacity()) {
        ESP_LOGW(TAG, "lua: coroutine cap %d reached — skipping '%s'",
                 CONFIG_LUA_ENGINE_MAX_COROUTINES, name);
        lua_pop(L, 1);                                 // drop compiled fn
        return false;
    }
    int ref = LUA_NOREF;
    lua_State* co = spawn_coroutine(L, &ref);          // consumes fn
    if (!co) {
        ESP_LOGE(TAG, "spawn '%s' failed", name);
        return false;
    }
    // First resume runs inline — we ARE TaskLua here, so detouring
    // through lua_scheduler_spawn's push-MSG_RESUME-to-self would burn
    // up to LUA_SCRIPT_MAX queue slots while this handler holds the
    // task (deterministic overflow at the Kconfig-minimum depth of 8)
    // and would let an already-queued event dispatch BEFORE the
    // scripts' top-level registrations run. Inline keeps the original
    // ordering: once MSG_LOAD_ALL is handled, every script has run to
    // its first yield/finish, so events behind it see all handlers.
    char err_ctx[48];   // "script '<name≤24>' error" — worst case 39 + NUL
    // %.24s: cache names are char[25], but the compiler can't see that
    // bound through the const char* param (-Werror=format-truncation).
    snprintf(err_ctx, sizeof(err_ctx), "script '%.24s' error", name);
    resume_and_settle(L, co, ref, 0, err_ctx);
    return true;   // loaded = compiled + started; runtime errors are
                   // logged/counted by resume_and_settle, as before
}

// Iterate the SPIFFS script cache, compile + start each entry. A bad
// script is logged and skipped, NEVER allowed to abort the loop — one
// broken file must not take the rest down or boot-loop the node.
static void load_all_scripts(lua_State* L) {
    LuaScriptEntry entries[LUA_SCRIPT_MAX];
    const uint16_t n = lua_script_cache_list(entries, LUA_SCRIPT_MAX);
    uint16_t loaded = 0;
    for (uint16_t i = 0; i < n; ++i) {
        if (load_one_script(L, entries[i].name)) loaded++;
    }
    ESP_LOGI(TAG, "lua scripts loaded: %u/%u", loaded, n);
}

static void task_lua(void* arg) {
    lua_State* L = (lua_State*)arg;
    ESP_LOGI(TAG, "TaskLua started");
    // T20: real watchdog coverage. esp_task_wdt_add(NULL) subscribes
    // THIS task; the loop resets it every iteration. The receive timeout
    // (below) keeps the period under the configured WDT window even when
    // the engine is idle, so a genuinely wedged TaskLua (tight in-C loop,
    // deadlock) trips the WDT while normal idle never false-trips.
    esp_task_wdt_add(nullptr);
    LuaMsg m;
    for (;;) {
        // T20: feed the task watchdog from the loop head. The receive
        // below uses a finite timeout (not portMAX_DELAY) so an idle
        // engine still wakes periodically to pet the WDT instead of
        // looking wedged. main.cpp subscribes TaskLua to the TWDT.
        esp_task_wdt_reset();
        if (xQueueReceive(s_resume_q, &m, pdMS_TO_TICKS(2000)) != pdTRUE) continue;

        // T20 (panic backstop): wrap every VM-touching dispatch step in a
        // setjmp frame so an unprotected OOM/internal raise (e.g. inside
        // lua_newthread / luaL_ref / pushstring, which run with no pcall
        // above them) longjmps here via lua_panic_handler instead of
        // abort()ing the chip. On a panic we just drop this one message
        // and loop — the engine survives a single bad dispatch.
        if (setjmp(s_panic_jmp) != 0) {
            // Returned from a panic longjmp — step already logged/counted.
            s_panic_armed = false;
            continue;
        }
        s_panic_armed = true;

        if (m.kind == MSG_RESUME) {
            resume_coroutine(L, &m.resume);
        } else if (m.kind == MSG_EVENT) {
            dispatch_event(L, &m.event);
        } else if (m.kind == MSG_RUN_NAMED) {
            run_named_script(L, &m.named);
        } else if (m.kind == MSG_LOAD_ALL) {
            load_all_scripts(L);
        }

        s_panic_armed = false;
    }
}

// Spawn a coroutine from a Lua function on top of L's stack. Consumes
// the function value. Returns the registry ref, or LUA_NOREF on OOM.
// NOTE: no in-tree callers since MSG_LOAD_ALL (load_all now spawns
// inline on TaskLua); kept for off-TaskLua use (mono-core port may
// need it) — removal candidate.
extern "C" int lua_scheduler_spawn(lua_State* L) {
    if (!lua_isfunction(L, -1)) return LUA_NOREF;
    if (lua_scheduler_at_capacity()) {
        ESP_LOGW(TAG, "lua: coroutine cap %d reached — refusing spawn",
                 CONFIG_LUA_ENGINE_MAX_COROUTINES);
        return LUA_NOREF;
    }

    int ref = LUA_NOREF;
    if (!spawn_coroutine(L, &ref)) return LUA_NOREF;   // consumes fn

    // The ref rides the MSG_RESUME; resume_coroutine settles it on
    // every outcome (resume_and_settle).
    lua_scheduler_push_resume(ref);
    return ref;
}

extern "C" bool lua_engine_scheduler_start(lua_State* L) {
    s_resume_q = xQueueCreate(CONFIG_LUA_ENGINE_RESUME_QUEUE_DEPTH,
                               sizeof(LuaMsg));
    if (!s_resume_q) {
        ESP_LOGE(TAG, "resume queue create failed");
        return false;
    }

    // T20: route VM panics (unprotected raises — OOM at the budget cap,
    // internal errors) through our handler so they unwind to task_lua's
    // per-step setjmp frame instead of abort()ing the chip.
    lua_atpanic(L, lua_panic_handler);

    BaseType_t ok = xTaskCreate(task_lua, "TaskLua",
                                 CONFIG_LUA_ENGINE_TASK_STACK_BYTES,
                                 L,
                                 CONFIG_LUA_ENGINE_TASK_PRIORITY,
                                 NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "TaskLua create failed");
        vQueueDelete(s_resume_q);
        s_resume_q = NULL;
        return false;
    }
    return true;
}

#endif
