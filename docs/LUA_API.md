# ZHAC Lua Scripting API Reference

This document describes the Lua scripting runtime shipped with ZHAC
firmware. It supersedes the retired Berry API reference — the Berry
engine was removed on 2026-04-21 (see `CHANGELOG.md` and
`docs/plans/2026-04-21-lua-engine-plan.md`) and Lua is now the only
scripting runtime.

Source of truth: every function and behaviour below is grounded in
`components/lua_engine/` and the P4/S3 firmware handlers. When a
feature is scoped but not wired, that's called out explicitly.

---

## Table of contents

1. [Overview](#1-overview)
2. [Lifecycle](#2-lifecycle)
3. [Execution model](#3-execution-model)
4. [Triggers](#4-triggers)
5. [`zhac.*` module reference](#5-zhac-module-reference)
6. [Sandbox](#6-sandbox)
7. [Storage and size limits](#7-storage-and-size-limits)
8. [REST endpoints](#8-rest-endpoints)
9. [WebUI](#9-webui)
10. [Examples](#10-examples)
11. [Troubleshooting](#11-troubleshooting)
12. [Limitations and deferred features](#12-limitations-and-deferred-features)

---

## 1. Overview

The Lua engine is a sandboxed scripting runtime that lets you attach
custom logic to Zigbee attribute changes, cron schedules, MQTT
messages, firmware boot, and rule-dispatched script calls. It runs on
the P4 core next to the rules engine and the device shadow, with a
one-way link into them via the `zhac.*` module.

**Version.** PUC-Lua 5.5 (vendored via the
`georgik/lua ^5.5.0~7` ESP-IDF managed component — see
`firmware/p4_core/main/idf_component.yml`).

**Guarantees.**

- **Sandboxed stdlib.** `io.*` is fully nil'd, dangerous `os.*`
  entries removed, filesystem loaders (`dofile`, `loadfile`,
  `package.path/cpath/loadlib/searchpath`) disabled. Bytecode load is
  blocked — all `require`/`load` paths use `mode="t"` (text only).
  See §6. Implementation: `components/lua_engine/src/lua_sandbox.c`.
- **Cooperative scheduling.** All Lua execution runs on one FreeRTOS
  task (`TaskLua`, priority 4, 8 KB stack by default). Scripts yield
  via `zhac.sleep(ms)`; no preemption. Implementation:
  `components/lua_engine/src/lua_scheduler.cpp`.
- **Hard memory budget.** Default 4 MB total Lua heap (tunable via
  `CONFIG_LUA_ENGINE_HEAP_KB`). Allocations above the cap fail — Lua
  raises `not enough memory` and unwinds safely. Small (<512 B)
  allocations try internal RAM first; larger go straight to PSRAM.
  See `components/lua_engine/src/lua_alloc.c`.
- **No global timeout.** A script that doesn't yield can monopolise
  `TaskLua` indefinitely. There is currently no watchdog kill on
  long-running scripts — this is called out in §12.
- **Single master state.** One `lua_State*` (`g_L`) shared by the
  whole engine. Individual scripts run as coroutines off this state.
  No per-script VM isolation.
- **Text-only sources.** Scripts are stored as UTF-8 Lua source on
  SPIFFS and compiled on load. Bytecode caches are not used.

**Where it runs.** P4 core only. The S3 side proxies REST calls to P4
over HAP but does not host the VM.

**Storage.** SPIFFS partition labelled `scripts`, mounted at
`/scripts`. Filenames are `<name>.lua`. Writes are atomic (`tmp`
file + rename). Mount is lazy and idempotent — the first call to any
`lua_script_cache_*` function mounts + format-on-empty.
Implementation: `components/lua_engine/src/lua_script_cache.cpp`.

---

## 2. Lifecycle

A script's journey, from upload to execution:

```
Browser / curl
       │
       ▼  POST /api/scripts/<name>     (raw Lua text)
S3 REST handler (firmware/zhac-net-core/main/rest_rules.cpp)
       │  SCRIPT_WRITE HAP frame
       ▼
P4 hap_dispatch (firmware/p4_core/main/hap_dispatch.cpp)
       │  lua_script_cache_write(name, src)
       ▼
SPIFFS /scripts/<name>.lua   (atomic tmp+rename)
       │
       ▼  lua_engine_load_all() on boot
       │  luaL_loadbufferx(..., mode="t")
       │  lua_scheduler_spawn(L)
       ▼
Top-level coroutine runs; registers on_* handlers
```

### Naming and size rules

- **Filename pattern.** `[a-zA-Z0-9_-]{1,24}` — enforced by
  `is_valid_name` in `lua_script_cache.cpp`. The WebUI further
  restricts new-script creation to lowercase-leading
  `^[a-z][a-z0-9_-]{0,23}$`. Extensions are added internally; do
  not include `.lua` in the name.
- **Max source size.** 16 KB per file (`LUA_SCRIPT_MAX_SRC`). Writes
  that exceed this are rejected.
- **Soft max file count.** 16 distinct scripts
  (`LUA_SCRIPT_MAX` = 16). The cache's list APIs iterate at most this
  many entries. Beyond it, scripts still exist on disk but are not
  auto-loaded.

### Storage invariants

- **Atomic write.** `lua_script_cache_write` writes to
  `/scripts/<name>.tmp`, fsync, then renames over `/scripts/<name>.lua`.
- **Lazy mount.** The first read / write / list call registers the
  SPIFFS VFS. Subsequent calls are no-ops.
- **Format on empty.** `format_if_mount_failed = true` in the VFS
  config — a freshly-wiped partition comes up formatted on first
  boot.

### Upload channels

- **REST.** `POST /api/scripts/<name>` with raw Lua body. See §8.
- **Bulk REST.** `POST /api/scripts` with JSON
  `[{"name":"...","src":"..."}, ...]`.
- **WebUI Scripts tab.** See §9.
- **HAP.** `SCRIPT_WRITE` frames directly over the S3↔P4 link. See
  `HapMsgType::SCRIPT_WRITE` in `firmware/p4_core/main/hap_dispatch.cpp`.

### Delete

- **REST.** `DELETE /api/scripts/<name>` — idempotent.
- **HAP.** `SCRIPT_DELETE`. Source file is removed from SPIFFS. Any
  coroutine already running from that source continues to completion;
  registered `on_*` handlers referenced by registry refs survive
  until the master state is rebuilt (i.e. reboot).

### Rename

Not a first-class operation. Workflow: read the source, write it
under the new name, delete the old one. Callers may simply issue
`POST /api/scripts/<new>` followed by `DELETE /api/scripts/<old>`.

---

## 3. Execution model

### 3.1 Coroutines + resume queue

`TaskLua` is the one task that ever touches the master state. External
code (REST handlers, rule engine, event bridge) reaches the VM by
pushing a `LuaMsg` onto a FreeRTOS queue (`s_resume_q`). TaskLua
dequeues messages one at a time and dispatches them:

| `LuaMsg.kind` | Triggered by | Action |
|---------------|--------------|--------|
| `MSG_RESUME` | `esp_timer` firing after `zhac.sleep`, or `lua_scheduler_push_resume` | Resume the coroutine whose registry ref is carried in the message |
| `MSG_EVENT` | `lua_engine_event_bridge.cpp` subscribing to `EventBus` | Walk the `REG_ON_*` handler table for the event kind and spawn one coroutine per registered handler, passing unpacked payload fields |
| `MSG_RUN_NAMED` | `DO script.run "<name>"` rule action via `simple_rules_set_script_hook` → `lua_scheduler_push_run_named` | Read `/scripts/<name>.lua`, compile, spawn a coroutine, pass the full trigger-context table (`{value, key, ieee, cluster, attr_id, val_type, int_val, str_val}`) as the single call argument |

Queue depth defaults to 32 (`CONFIG_LUA_ENGINE_RESUME_QUEUE_DEPTH`).
A full queue drops messages silently and logs a warning from
`lua_engine_rules_hook` or the event bridge.

### 3.2 Sleeping (`zhac.sleep`)

`zhac.sleep(ms)` is the only cooperative yield point exposed to Lua.
It's implemented as:

1. Consume the caller thread (via `lua_pushthread`); fail if called
   from the main thread (not a coroutine).
2. Stash a `luaL_ref` to the current coroutine in the Lua registry.
3. Acquire a free slot from an `esp_timer` pool of size
   `CONFIG_LUA_ENGINE_TIMER_POOL` (default 16).
4. Start the one-shot timer for `ms` milliseconds.
5. `lua_yield` — TaskLua moves on to the next message.

When the timer fires (in `esp_timer` task context), its callback
pushes `MSG_RESUME` onto `s_resume_q` from ISR-safe code
(`xQueueSendFromISR`). TaskLua picks it up, resumes the coroutine,
and the call returns normally inside Lua.

**Pool saturation.** If all 16 slots are occupied when a script calls
`zhac.sleep`, the call raises `zhac.sleep: timer pool exhausted`.
Scripts that might race should handle this with `pcall`.

### 3.3 Error handling

A coroutine that raises an uncaught error is logged via `ESP_LOGE`
with the `lua_sched` tag and the error metric
(`METRIC_LUA_ERRORS_TOTAL`) is incremented. The coroutine is dropped —
there is **no** automatic retry, and **no** auto-disable of the
script that registered it. Other registered handlers for the same
event continue to fire as usual on the next event.

`print(...)` is redirected to `ESP_LOGI("lua", ...)` so `print("hi")`
in a script emits `I (...) lua: hi` on the serial console.

### 3.4 Observability

Metrics exported via `metric_registry.def` (Prometheus scrape prefix
`zhac_p4_lua_*`):

| Metric | Kind | Meaning |
|--------|------|---------|
| `METRIC_LUA_HEAP_USED_BYTES` | Value | Current allocator-charged bytes |
| `METRIC_LUA_HEAP_PEAK_BYTES` | Value | All-time high watermark |
| `METRIC_LUA_COROUTINES_LIVE` | Value | Sampled on coroutine enter/exit |
| `METRIC_LUA_ERRORS_TOTAL` | Counter | Uncaught errors since boot |
| `METRIC_LUA_YIELDS_TOTAL` | Counter | `lua_yield` calls since boot |

Public C API mirrors the metrics:

```c
size_t   lua_engine_heap_used_bytes(void);
size_t   lua_engine_heap_peak_bytes(void);
uint16_t lua_engine_live_coroutines(void);
uint32_t lua_engine_error_count(void);
uint32_t lua_engine_yield_count(void);
```

See `components/lua_engine/include/lua_engine.h`.

---

## 4. Triggers

Four entry points invoke script code. All dispatch through TaskLua,
so handler bodies always run on the same thread — no external
synchronisation needed between handlers.

### 4.1 `DO script.run "<name>"` from a simple rule

A rule action dispatches a named Lua script. Grammar:

```
ON <trigger> DO script.run "<name>" ENDON
```

`<name>` must match a stored script (same pattern as §2 — 24-char
alphanumeric + `_` + `-`). The rule engine hands the script a **single
table argument** carrying the trigger context:

| Field       | Type    | Populated for                          | Notes |
|-------------|---------|----------------------------------------|-------|
| `value`     | string  | every trigger                           | stringified trigger value (what legacy scripts used to get as `...`) |
| `key`       | string  | `DEVICE_ATTR`                           | attr name (`"state"`, `"action"`, …); `""` for non-device triggers |
| `ieee`      | integer | `DEVICE_ATTR`                           | source device IEEE as u64; `0` otherwise |
| `cluster`   | integer | `DEVICE_ATTR`                           | ZCL cluster id; `0` otherwise |
| `attr_id`   | integer | `DEVICE_ATTR`                           | ZCL attribute id; `0` otherwise |
| `val_type`  | integer | `DEVICE_ATTR`                           | `ValType` enum — 0 bool / 1 int / 2 str |
| `int_val`   | integer | `DEVICE_ATTR` with numeric value        | raw int (unstringified) |
| `str_val`   | string  | `DEVICE_ATTR` with string value         | raw string; `""` otherwise |

Example — attr-specific trigger, value arrives as `ev.value`:

```lua
-- /scripts/notify.lua — invoked by `ON kitchen_motion#occupancy=1 DO script.run "notify" ENDON`
local ev = ...
zhac.log("I", "rule fired with value=" .. ev.value)
```

Example — **wildcard form** that routes every attribute of a device
into a single script:

```lua
-- /scripts/kitchen_motion.lua — invoked by `ON kitchen_motion DO script.run "kitchen_motion" ENDON`
local ev = ...
if ev.key == "occupancy" and ev.int_val == 1 then
    zhac.set_attr("0x00158DFFFE…", "state", true)
elseif ev.key == "illuminance" then
    zhac.log("I", string.format("lux=%d", ev.int_val))
end
```

The wildcard trigger is documented in `docs/RULES_DSL.md#wildcard-any-attribute-on-a-device`.

Registration: `ActionType::SCRIPT` (value 6) in
`components/simple_rules/include/simple_rules.h`, dispatched via
`simple_rules_set_script_hook(hook)`. The Lua engine wires the hook
at init in `lua_engine_rules_hook.cpp`.

**Semantics.** Fire-and-forget. The rule action returns as soon as
the `MSG_RUN_NAMED` message is queued. A full queue drops the request
and logs `script.run '<name>' dropped — scheduler queue full`.

See `docs/RULES_DSL.md` for the full DSL grammar.

### 4.2 `zhac.on_attr_change(ieee_hex, key, fn)`

Register `fn` for device attribute updates. `fn` is invoked by the
event bridge when a `ZclAttrEvent` carrying the matching `(ieee, key)`
lands on the EventBus.

**Signature.**

```lua
zhac.on_attr_change(ieee_hex, key, function(ieee, key, value)
    -- ...
end)
```

**Arguments.**

- `ieee_hex` — 16-character IEEE address (`"0x001234567890ABCD"` or
  `"001234567890ABCD"`, both accepted by `parse_ieee_hex`).
- `key` — semantic attribute key (e.g. `"state"`, `"occupancy"`,
  `"temperature"`). See the ZHC exposes for valid keys per device.
- `fn(ieee, key, value)` — value is `boolean`, `integer`, or
  `string` depending on the attribute's shadow `ValType`.

**Threading.** Runs on TaskLua. Long operations should `zhac.sleep`
to let other events interleave.

### 4.3 `zhac.on_cron(expr, fn)`

Register `fn` for cron-driven firings. The expression is a 6-field
cron spec (`sec min hour mday month wday`), same format as
`ON Time#Cron=<expr>` rule triggers.

**Signature.**

```lua
zhac.on_cron("0 0 22 * * *", function()
    zhac.log("I", "22:00 chime")
end)
```

**Dispatch.** The simple_rules cron task walks registered cron
triggers once per minute. When one fires it publishes a `RULE_EVENT`
on EventBus, which the Lua scheduler consumes and routes to the
matching handler. Sub-minute granularity is not guaranteed.

### 4.4 `zhac.on_mqtt(topic, fn)`

Register `fn` for MQTT ingress on the given topic.

**Signature.**

```lua
zhac.on_mqtt("home/cmd/lights", function(topic, payload)
    if payload == "off" then
        zhac.set_attr("0x00158D0001020304", "state", false)
    end
end)
```

Dispatch: EventBus `MQTT_MSG` → event bridge → TaskLua. The MQTT
gateway must be connected (`mqtt_gw.cpp`) and subscribed to a parent
of the topic for messages to flow.

### 4.5 `zhac.on_boot(fn)`

Register `fn` for a one-shot firing at firmware boot. Dispatched via
EventBus `CTRL_BOOT` during P4 init.

**Signature.**

```lua
zhac.on_boot(function()
    zhac.log("I", "boot: Lua scripts up")
end)
```

### 4.6 `zhac.on_zcl_raw(fn)`

Register `fn` for every ZCL frame the ZHC library failed to decode
(no FZ converter matched). Dispatched via EventBus `ZCL_RAW` from
`zigbee_mgr::zcl_publish_raw_fallback`. Use this to inspect / log
unknown vendor traffic and route it to MQTT, Telegram, etc. with
arbitrary filter logic in Lua.

**Signature.** Handler receives a single table:

```lua
zhac.on_zcl_raw(function(ev)
    -- ev.ieee     "0xa4c138fffe010203"
    -- ev.nwk      uint16
    -- ev.ep       uint8
    -- ev.cluster  ZCL cluster id
    -- ev.command  ZCL command id (high bit 0x80 set when server→client)
    -- ev.len      raw payload length (0..40)
    -- ev.hex      raw ZCL bytes hex-encoded (max 80 chars)
    if ev.cluster == 0x0500 and ev.command == 0x00 then
        zhac.mqtt_publish("zhac/raw/ias/" .. ev.ieee, ev.hex)
    end
end)
```

**Volume warning.** Some devices spam unmatched frames every few
seconds (manuf-specific heartbeats, vendor diag). Filter aggressively
in Lua before forwarding — the scheduler queue drops on overflow.

### Registration flow

All five `on_*` functions share `register_handler` in
`zhac_lua_module.cpp`. They stash the callback in a per-kind Lua
registry table (`REG_ON_ATTR` / `REG_ON_CRON` / `REG_ON_MQTT` /
`REG_ON_BOOT` / `REG_ON_RAW`). On event, the scheduler looks up the
table, iterates its entries, and spawns one coroutine per registered
handler with the unpacked payload fields.

---

## 5. `zhac.*` module reference

Every function exported by `kZhacLib` in
`components/lua_engine/src/zhac_lua_module.cpp`. The module is
preloaded via `luaL_requiref(L, "zhac", luaopen_zhac, 1)` in the
sandbox init, so `zhac` is a live global **and** available via
`require "zhac"`. Use either style.

### 5.1 `zhac.log([level,] msg)`

Write to the ESP-IDF log.

- `level` — optional single-char string. `'E'`/`'W'`/`'D'` pick the
  corresponding ESP log level; anything else (and the no-level form)
  use `ESP_LOGI`.
- `msg` — string.

Log tag is `lua_script`. The one-arg form
(`zhac.log("hello")`) emits at INFO level.

**Returns.** nothing.

**Example.**

```lua
zhac.log("something happened")
zhac.log("W", "queue nearly full")
```

Source: `l_zhac_log` at `zhac_lua_module.cpp`.

### 5.2 `zhac.millis() → integer`

Monotonic milliseconds since boot. Derived from `esp_timer_get_time()
/ 1000`. Rolls over at ~49 days if firmware stays up that long; for
shorter intervals use the delta between two calls.

**Example.**

```lua
local t0 = zhac.millis()
zhac.sleep(500)
zhac.log("I", "elapsed: " .. (zhac.millis() - t0) .. " ms")
```

### 5.3 `zhac.sleep(ms)`

Yield the current coroutine for approximately `ms` milliseconds. See
§3.2 for the full implementation details.

**Arguments.** `ms` — non-negative integer.

**Failure modes.**

- Called outside a coroutine: raises `zhac.sleep must be called from a coroutine`.
- Negative delay: raises `zhac.sleep: negative delay`.
- Out of registry refs: raises `zhac.sleep: out of registry`.
- Timer pool exhausted: raises `zhac.sleep: timer pool exhausted`.
  This is the most common failure in practice — wrap in `pcall` if
  your script might race against others.

### 5.4 `zhac.set_attr(ieee_hex, key, value) → bool`

Write a Zigbee attribute on a paired device. The value type drives
the underlying wire encoding:

| Lua type | Route |
|----------|-------|
| `boolean` | `zhac_adapter_send_bool` |
| `integer` | `zhac_adapter_send_uint` |
| `string` | `zhac_adapter_send_string` |

**Arguments.**

- `ieee_hex` — device IEEE as `"0x001234567890ABCD"` or
  `"001234567890ABCD"`.
- `key` — semantic attribute key from the device's exposes (e.g.
  `"state"`, `"brightness"`, `"color_temp"`).
- `value` — `boolean`, `integer`, or `string`. Other types raise
  `zhac.set_attr: value must be bool/integer/string`.

**Returns.** `true` if the adapter accepted the write, `false`
otherwise. A `false` return is also produced if the IEEE parses but
isn't in the device pool, or if the adapter has no definition for
the device (logged via `ESP_LOGW`).

**Example.**

```lua
local ok = zhac.set_attr("0x00158D0001020304", "state", true)
if not ok then zhac.log("W", "write failed") end
```

Source: `l_zhac_set_attr`.

### 5.5 `zhac.get_attr(ieee_hex, key) → int | string | bool | nil`

Read the last-known attribute value from the device shadow. Does not
issue a ZCL read — the shadow is populated by the incoming report
stream.

**Arguments.** Same `ieee_hex` + `key` as `set_attr`.

**Returns.** `integer` / `string` / `boolean` depending on the
shadow's `ValType` tag; `nil` if the device isn't in the pool, the
IEEE is malformed, or the attribute has never been reported.

**Example.**

```lua
local t = zhac.get_attr("0x00158D0001234567", "temperature")
if t and t > 2500 then
    zhac.publish("home/alert", "hot")
end
```

### 5.6 `zhac.publish(topic, payload [, qos [, retain]])`

Publish to the MQTT broker via `mqtt_gw_publish`. No-op if the
gateway isn't connected (no return value surfaces that state — use
`GET /api/status` / the WebUI to confirm MQTT).

**Arguments.**

- `topic` — string.
- `payload` — string.
- `qos` — optional integer, default `0`.
- `retain` — optional boolean, default `false`.

**Alias.** `zhac.mqtt_publish(...)` is the same function (see
`kZhacLib`).

### 5.7 `zhac.event(name)`

Publish a `RULE_EVENT` on the P4 internal EventBus. This fires every
simple rule with an `ON Event#<name>` trigger and every Lua handler
registered via `zhac.on_cron` (cron triggers use RULE_EVENT under the
hood — arbitrary `zhac.event` emissions don't reach `on_cron`
handlers unless the name matches a configured cron slot, which in
normal use it won't).

**Arguments.** `name` — string (maximum length truncated to the
`RuleEventPayload.name` field size; long names are clipped).

**Example.**

```lua
-- Fire a named event whenever a cube is rolled
zhac.on_attr_change("0x00158D0001111111", "action", function(_, _, v)
    if v == "rotate_right" then
        zhac.event("volume_up")
    end
end)
```

### 5.8 `zhac.on_attr_change(ieee_hex, key, fn)` — see §4.2

### 5.9 `zhac.on_cron(expr, fn)` — see §4.3

### 5.10 `zhac.on_mqtt(topic, fn)` — see §4.4

### 5.11 `zhac.on_boot(fn)` — see §4.5

### 5.12 Full `kZhacLib` list

The canonical export table (from `zhac_lua_module.cpp`):

```c
static const luaL_Reg kZhacLib[] = {
    {"log",              l_zhac_log},
    {"millis",           l_zhac_millis},
    {"sleep",            l_zhac_sleep},
    {"set_attr",         l_zhac_set_attr},
    {"get_attr",         l_zhac_get_attr},
    {"publish",          l_zhac_publish},
    {"mqtt_publish",     l_zhac_publish},
    {"event",            l_zhac_event},
    {"on_attr_change",   l_zhac_on_attr_change},
    {"on_cron",          l_zhac_on_cron},
    {"on_mqtt",          l_zhac_on_mqtt},
    {"on_boot",          l_zhac_on_boot},
    {NULL, NULL},
};
```

Nothing else is exposed. Direct binding / unbinding, group
multicasts, raw ZCL writes, and KV storage are **not** available from
Lua today (§12).

### 5.13 `zhac.telegram_settoken(token) → bool`

Persist the Telegram bot token on S3. Required before `telegram_send` works.
Token comes from BotFather (`/newbot`). Stored in S3 NVS at `zhac/tg_token`,
masked in all logs. Returns true if forwarded to S3 successfully.

```lua
zhac.telegram_settoken("1234567890:AAH...")
```

### 5.14 `zhac.telegram_setchat(chat_id_str) → bool`

Persist the default chat ID. String form so negative group IDs work
(`"-1001234567890"`). Stored at `zhac/tg_chat`. Optional — `telegram_send`
also accepts an explicit `chat_id` per call.

```lua
zhac.telegram_setchat("1234567890")
zhac.telegram_setchat("-1001234567890")
```

### 5.15 `zhac.telegram_send(text [, chat_id [, parse_mode]]) → bool`

Send a Telegram message. Async / fire-and-forget — return value indicates
the HAP frame was queued, not the HTTP outcome (see S3 logs for that).

- `text` — string, ≤ 3 KB.
- `chat_id` — optional override of the default chat.
- `parse_mode` — `"Markdown"`, `"MarkdownV2"`, `"HTML"`, or a full
  query-string fragment for advanced features (e.g. `reply_markup`).

```lua
zhac.telegram_send("door opened")
zhac.telegram_send("temp=" .. zhac.get_attr("0x...", "temperature"))
zhac.telegram_send("**bold**", nil, "Markdown")
```

The `secure()` toggle from SLS does not exist — ZHAC always uses HTTPS.

---

## 6. Sandbox

Applied once per master state by `lua_engine_sandbox_apply` in
`components/lua_engine/src/lua_sandbox.c`. Runs after
`luaL_openlibs` and before any user code.

### 6.1 Nil'd (fully removed)

- **`io`** — the entire table is set to `nil`. No file access of any
  kind.
- **Top-level loaders** — `dofile` and `loadfile` set to `nil`.

### 6.2 Redacted (module kept, entries removed)

- **`os.execute`** — no shell escapes.
- **`os.exit`** — scripts can't kill the VM.
- **`os.getenv`** — no environment leak.
- **`os.remove`**, **`os.rename`**, **`os.tmpname`** — no filesystem
  write.
- **`os.setlocale`** — locale not settable.
- **`package.loadlib`** — no dynamic library load.
- **`package.searchpath`**, **`package.cpath`**, **`package.path`** —
  filesystem loader state cleared.
- **`debug.debug/gethook/getinfo/getlocal/getregistry/getupvalue/getuservalue/sethook/setlocal/setmetatable/setupvalue/setuservalue/upvaluejoin/upvalueid`** —
  everything except `debug.traceback` (which is kept for error formatting).

### 6.3 Retained

The usual base library (`type`, `tostring`, `pairs`, `ipairs`,
`select`, `pcall`, `xpcall`, `error`, `assert`, `next`, `rawget`,
`rawset`, `setmetatable`, `getmetatable`, …), plus `table.*`,
`string.*`, `math.*`, `coroutine.*`. `os.time`, `os.clock`,
`os.date`, `os.difftime` stay. `debug.traceback` stays.

### 6.4 Replaced

- **`print(...)`** — replaced with a handler that formats arguments
  the same way as stock Lua (tab-separated), then emits the result
  via `ESP_LOGI("lua", ...)`. See `sandboxed_print` in
  `lua_sandbox.c`.

### 6.5 `require`

`require` keeps working but only resolves two sources:

1. **Preloaded native modules.** `zhac` always.
   `cjson` / `lpeg` / `miniz` if their respective Kconfig flags are
   set and the matching `luaopen_*` symbol is linked in. See
   `lua_require.c`.
2. **SPIFFS script cache.** `require "motion"` reads
   `/scripts/motion.lua`, compiles in text-only mode, returns the
   module table.

No filesystem outside `/scripts`, no C-loader shared-object path, no
bytecode.

### 6.6 Why

The sandbox exists because scripts run in the same process as the
Zigbee stack, rule engine, HAP protocol handler, and metrics engine.
A script that could shell out or mmap arbitrary files could brick the
device, and because scripts are editable over REST with a single
token, the attack surface extends wherever the token reaches. Keeping
the Lua stdlib's file and process escape hatches closed keeps the
blast radius bounded to "script uses its PSRAM budget then OOMs".

---

## 7. Storage and size limits

Implementation: `components/lua_engine/src/lua_script_cache.cpp`.

| Limit | Value | Source |
|-------|-------|--------|
| SPIFFS base path | `/scripts` | `BASE_PATH` |
| Partition label | `scripts` | `PARTITION_LABEL` |
| Max open files | 8 | `esp_vfs_spiffs_conf_t.max_files` |
| Max name length | 24 characters (excl. NUL) | `LUA_SCRIPT_NAME_MAX` |
| Name charset | `[a-zA-Z0-9_-]` | `is_valid_name` |
| Max source size per script | 16 KB | `LUA_SCRIPT_MAX_SRC` in `hap_dispatch` and the 16 KB buffer in `load_one_script` |
| Soft file count cap | 16 | `LUA_SCRIPT_MAX` |

Beyond the soft file cap, additional `.lua` files on the partition
survive but `lua_engine_load_all` won't spawn coroutines for them at
boot — the list API stops iterating after 16.

---

## 8. REST endpoints

All endpoints are served by the S3 core and proxied to P4 over HAP.
Handlers in `firmware/zhac-net-core/main/rest_rules.cpp`. All require the
`X-Api-Key` token (`REQUIRE_AUTH`).

See `docs/REST_API.md` for the full API surface; the section below
focuses on script endpoints only.

### 8.1 `GET /api/scripts`

List all scripts.

**Response:**

```json
{
  "scripts": [
    { "name": "motion",   "size": 256 },
    { "name": "schedule", "size": 431 }
  ]
}
```

If the P4 SCRIPT_LIST_REQ round-trip times out, the endpoint returns
`{"scripts":[]}` (empty list) rather than an error — the Scripts page
renders cleanly even before the script cache is initialised.

### 8.2 `GET /api/scripts/{name}`

Read one script.

**Response:**

```json
{ "name": "motion", "src": "zhac.log('hi')\n" }
```

**Errors.** `400` on invalid name; `500` on P4 timeout.

### 8.3 `POST /api/scripts/{name}`

Create or overwrite a script.

**Request body:** raw Lua source text, up to 16 KB. Content-Type
`text/plain`. There is **no** JSON wrapper.

**Response:** `201 Created` + `{"ok":true}`. `429` when the HAP
channel is busy; `500` on P4 timeout or encode failure.

### 8.4 `POST /api/scripts`

Bulk save.

**Request:**

```json
[
  { "name": "motion",   "src": "..." },
  { "name": "schedule", "src": "..." }
]
```

**Response:** `{"ok":true,"written":N,"failed":M}`. A busy channel
returns `429` for the whole batch — callers must retry the full
array; partial writes are not observable.

### 8.5 `DELETE /api/scripts/{name}`

Delete. Idempotent — returns `{"ok":true}` even if the script did
not exist.

### 8.6 `POST /api/scripts/{name}/run`

Fire-and-forget: synchronously enqueues `<name>` onto the Lua
scheduler and returns. **Auth required.**

Backed by the `SCRIPT_RUN_REQ = 0x58` HAP message — the S3 handler
(`api_script_run`) frames the name, the P4 handler
(`handle_script_run_req`) calls the public
`lua_engine_run_script(const char*)` API in
`components/lua_engine/include/lua_engine_scripts.h`. The same path
backs the SPA "Run" button and the WS `script.run` command.

**Response:** `{"ok":true}` when the enqueue succeeds; runtime
errors surface through the log pipeline (see §3.3), not the HTTP
response.

---

## 9. WebUI

The Scripts tab lives at `#scripts` in the Preact SPA
(`www-spa/src/pages/Scripts.jsx`). The UI speaks WebSocket only —
every action maps to a command in the `api_*` registry.

1. **List** — the page calls WS cmd `script.list`, renders a
   three-column table (name, size, actions).
2. **Create** — the **+ New Script** toolbar button opens a modal
   with a name input and a Lua source `<textarea>` (gutter-numbered
   via `CodeEditor`). Name validation regex:
   `^[a-z][a-z0-9_-]{0,23}$`. Save issues `script.write`.
3. **Edit** — **Edit** button issues `script.read`, fills the modal,
   saves via `script.write`.
4. **Delete** — **Del** prompts then calls `script.delete`.
5. **Run** — **Run** button calls `script.run`, fire-and-forget.

No in-browser syntax highlighting today (TODO). Runtime errors
surface through the S3 log ring; the SPA Logs page polls
`logs.get` every 5 s to display them (the live `log.entry` WS
event is currently disabled by default — see §11).

---

## 10. Examples

All four examples are paste-ready — save under `/scripts/<name>.lua`
and restart (or re-upload to pick up on the next `load_all` cycle).

### 10.1 Attr-change → MQTT publish

```lua
-- /scripts/motion_publish.lua
-- Republish hallway occupancy to MQTT when it changes.

local MOTION_IEEE = "0x00158D0001234567"

zhac.on_attr_change(MOTION_IEEE, "occupancy", function(ieee, key, val)
    zhac.publish("home/sensors/hallway", val and "1" or "0", 0, true)
    zhac.log("I", "hallway occupancy=" .. tostring(val))
end)
```

### 10.2 Cron → set_attr (morning wake-up)

```lua
-- /scripts/morning.lua
-- At 07:00 on weekdays, lift the blind and ramp brightness.

local BLIND  = "0x00158D0001AAAA01"
local LIGHT  = "0x00158D0001BBBB02"

zhac.on_cron("0 0 7 * * 1-5", function()
    zhac.log("I", "morning routine")
    zhac.set_attr(BLIND, "lift", 100)

    -- Ramp up over 10 seconds so the room isn't instantly blinding.
    for level = 10, 80, 10 do
        zhac.set_attr(LIGHT, "brightness", level)
        zhac.sleep(1500)
    end
    zhac.set_attr(LIGHT, "state", true)
end)
```

### 10.3 Rule-triggered fire-and-forget

```lua
-- /scripts/siren_pulse.lua
-- Invoked from a rule via:  DO script.run "siren_pulse"
-- The rule engine passes an event-context table as the single arg.

local SIREN = "0x00158D0001CCCC03"
local ev = ...   -- { value=..., key=..., ieee=..., ... } — see §4.1

zhac.log("I", "siren_pulse triggered key=" .. ev.key .. " value=" .. ev.value)

for i = 1, 3 do
    zhac.set_attr(SIREN, "state", true)
    zhac.sleep(500)
    zhac.set_attr(SIREN, "state", false)
    zhac.sleep(500)
end
```

Matching rule (in `docs/RULES_DSL.md` grammar):

```
ON front_door#contact=0 DO script.run "siren_pulse" ENDON
```

### 10.4 MQTT listener with state-tracking via local

```lua
-- /scripts/scene_remote.lua
-- Select a scene via MQTT messages to home/cmd/scene.
-- Values: "day", "night", "away".

local LIVING = "0x00158D0001DDDD04"
local BEDROOM = "0x00158D0001EEEE05"

local last_scene = "day"   -- in-memory; lost on reboot

zhac.on_mqtt("home/cmd/scene", function(topic, payload)
    if payload == last_scene then return end
    last_scene = payload

    if payload == "day" then
        zhac.set_attr(LIVING, "brightness", 200)
        zhac.set_attr(BEDROOM, "state", false)
    elseif payload == "night" then
        zhac.set_attr(LIVING, "brightness", 60)
        zhac.set_attr(BEDROOM, "state", true)
    elseif payload == "away" then
        zhac.set_attr(LIVING, "state", false)
        zhac.set_attr(BEDROOM, "state", false)
    else
        zhac.log("W", "unknown scene: " .. tostring(payload))
    end
end)
```

### 10.5 MQTT JSON command parser

Parse a structured command off an MQTT topic using the bundled
`cjson` module (enabled by default via `CONFIG_LUA_ENGINE_WITH_CJSON`).
The inbound payload is a JSON object `{"target":"...","brightness":N,
"transition":seconds}`; the script validates it and dispatches a
`set_attr` with a transition ramp.

```lua
-- /scripts/cmd_light.lua
-- Subscribes to: home/cmd/light
-- Payload:       {"target":"0x00158D...","brightness":120,"transition":3}

local cjson = require("cjson")

local function parse_cmd(payload)
    -- Decode can raise on bad JSON; pcall contains it.
    local ok, obj = pcall(cjson.decode, payload)
    if not ok then
        zhac.log("W", "cmd_light: bad JSON: " .. tostring(obj))
        return nil
    end
    if type(obj)            ~= "table"  then return nil end
    if type(obj.target)     ~= "string" then return nil end
    if type(obj.brightness) ~= "number" then return nil end
    return obj
end

zhac.on_mqtt("home/cmd/light", function(topic, payload)
    local cmd = parse_cmd(payload)
    if not cmd then return end

    -- Simple ramp: N samples over `transition` seconds, linear.
    local steps   = 10
    local step_ms = math.floor((cmd.transition or 0) * 1000 / steps)
    if step_ms <= 0 then
        zhac.set_attr(cmd.target, "brightness", cmd.brightness)
        return
    end

    local cur = zhac.get_attr(cmd.target, "brightness") or 0
    for i = 1, steps do
        local v = math.floor(cur + (cmd.brightness - cur) * i / steps)
        zhac.set_attr(cmd.target, "brightness", v)
        zhac.sleep(step_ms)
    end
end)
```

Notes:

- `pcall(cjson.decode, …)` is the defensive pattern. A malformed
  payload raises — without `pcall` the whole coroutine dies and no
  further messages on the topic are processed until script reload.
- `cjson.null` is the sentinel for JSON `null`. Test with
  `value == cjson.null`, not `value == nil`, when you need to
  distinguish "key missing" from "key present but null".
- Decode depth is capped at 64 (see `CONFIG_LUA_ENGINE_CJSON_DECODE_MAX_DEPTH`).
  Deeper payloads raise `"found too many nested data structures"`.

### 10.6 Publish a JSON state snapshot on cron

Roll up a handful of attributes into one JSON object and publish it
every minute — convenient for dashboards that want a single topic
rather than N separate attr updates.

```lua
-- /scripts/snapshot.lua
local cjson = require("cjson")

local SENSORS = {
    { ieee = "0x00158D0001AAAA01", name = "kitchen" },
    { ieee = "0x00158D0001BBBB02", name = "bedroom" },
    { ieee = "0x00158D0001CCCC03", name = "hallway" },
}

zhac.on_cron("0 * * * * *", function()   -- every minute
    local snap = {
        ts       = os.time(),
        sensors  = {},
    }
    for _, s in ipairs(SENSORS) do
        snap.sensors[s.name] = {
            temperature = zhac.get_attr(s.ieee, "temperature"),
            humidity    = zhac.get_attr(s.ieee, "humidity"),
            battery     = zhac.get_attr(s.ieee, "battery"),
        }
    end
    zhac.publish("home/snapshot", cjson.encode(snap))
end)
```

Tips:

- `cjson.encode_sparse_array(true)` — if a sensor's value is missing,
  `get_attr` returns `nil`; Lua skips the key in the emitted JSON.
  Call `cjson.encode_sparse_array` once at startup if you want
  fixed-shape arrays with `cjson.null` gaps instead.
- Wrap `cjson.encode(snap)` in `pcall` if any value might be a
  non-JSON-safe type (function, userdata). The encoder raises on
  those; a malformed publish would be silently dropped.

### 10.7 Device-wildcard router (one script per device)

Use the wildcard DSL form (`ON <device> DO script.run "..."` with
no `#<attr>` suffix) to funnel every attribute change on a single
device into one Lua script. The event table handed to the script
carries `ev.key` / `ev.int_val` / `ev.str_val` / `ev.cluster` /
`ev.attr_id` / `ev.ieee`, so the handler can dispatch however it
wants — see [RULES_DSL.md](RULES_DSL.md#wildcard-any-attribute-on-a-device).

Matching rule:

```
ON kitchen_motion DO script.run "kitchen_motion" ENDON
```

Handler:

```lua
-- /scripts/kitchen_motion.lua
local ev = ...   -- { key=..., value=..., int_val=..., str_val=...,
                 --   ieee=..., cluster=..., attr_id=..., val_type=... }

local KITCHEN_LIGHT = "0x00158D0001DEAD01"

if ev.key == "occupancy" then
    -- Aqara returns 0/1 for boolean occupancy.
    if ev.int_val == 1 then
        zhac.set_attr(KITCHEN_LIGHT, "state", true)
    end

elseif ev.key == "illuminance" then
    -- Log lux readings so the dimming policy can be tuned later.
    zhac.log("I", string.format("kitchen lux=%d", ev.int_val))

elseif ev.key == "battery" and ev.int_val < 20 then
    zhac.publish("home/alert/battery",
                  string.format("kitchen motion low battery: %d%%", ev.int_val))

else
    -- Everything else: cheap visibility for unexpected attrs the
    -- device starts emitting after a firmware update.
    zhac.log("D", string.format("kitchen_motion: %s=%s (cluster=0x%04X attr=0x%04X)",
                                 ev.key, ev.value, ev.cluster, ev.attr_id))
end
```

This keeps the rule table tiny (one line per device) and moves the
branching into Lua where it's natural. Good fit when a device exposes
many attributes and you don't want to maintain five separate rules.

### 10.8 Round-trip JSON through cjson + MQTT (echo test)

Quick sanity check that the stack is wired end-to-end:

```lua
-- /scripts/echo.lua — DO script.run "echo" from any rule.
local cjson = require("cjson")

local payload = cjson.encode({
    msg = "hello from zhac",
    up  = zhac.millis(),
    ev  = (...) or {},   -- trigger context passed by simple_rules, if any
})
zhac.publish("home/echo", payload)
```

Subscribe `home/echo` with `mosquitto_sub -t home/echo` to watch it
fire on every trigger.

---

## 11. Troubleshooting

### Parse error on load

Symptom: boot log shows `compile 'motion': ...parse error near ...`.
The script isn't spawned. Cause: Lua syntax error in the source.

Fix: fix the syntax. The error message and line number come from
Lua's own parser. Re-upload the script; next reboot (or immediate
HAP-triggered re-spawn via `script.run`) picks up the fix.

### `zhac.sleep: timer pool exhausted`

Symptom: ERROR-level `lua_sched` log with that message, usually from
a busy-looping handler.

Cause: all 16 timer slots are in use. Multiple scripts are sleeping
concurrently and the pool hit its cap.

Mitigations:
- Reduce concurrent sleepers.
- Raise `CONFIG_LUA_ENGINE_TIMER_POOL` (bounded at 64 by the Kconfig
  range) via `menuconfig` + rebuild.
- Wrap calls in `pcall` to recover gracefully:

```lua
local ok, err = pcall(zhac.sleep, 500)
if not ok then
    zhac.log("W", "sleep saturated: " .. err)
    -- fallback: busy-wait is NOT advised; just skip this iteration
end
```

### Script runs but `zhac.set_attr` returns `false`

Symptom: handler fires, `ESP_LOGW` line like
`set_attr: device 0x... not in pool` or `set_attr: bad ieee '...'`.

Cause: the IEEE string doesn't parse, or the device has left the
network.

Fix: double-check the IEEE via `GET /api/devices`. Valid formats are
`"0x001234567890ABCD"` and `"001234567890ABCD"` — pasted with a
stray space or the wrong case, they silently hash-mismatch.

### SPIFFS full when writing a new script

Symptom: `POST /api/scripts/<name>` returns 500 with
`lua_script: mount 'scripts' failed: ESP_ERR_INVALID_STATE` in the
log, or the write succeeds but the file is 0 bytes.

Cause: the `scripts` partition is full or its FS is corrupted.

Fix: delete unused scripts; if the partition is actually empty and
still rejecting writes, erase the SPIFFS partition via OTA-recovery
or `esptool` and let the lazy mounter reformat it.

### Script edit doesn't take effect

Symptom: you edited `/scripts/foo.lua` via REST, but `on_*` handlers
you registered in the old version still run.

Cause: `on_*` registrations persist in the Lua registry until the
master state is recreated. Editing a script doesn't kill handlers
the old version already installed.

Fix: reboot the P4 core. The next `lua_engine_load_all` brings up a
fresh state with only the current sources.

### TaskLua stack overflow

Symptom: a panic with "TaskLua" in the backtrace, or a FreeRTOS
stack-overflow notification.

Cause: deeply nested Lua calls in a handler. Default stack is 8 KB
(`CONFIG_LUA_ENGINE_TASK_STACK_BYTES`).

Fix: raise the Kconfig value (bounded at 32 KB) and rebuild. Or
reduce handler depth / move work into multiple coroutines via
`zhac.on_*`.

---

## 12. Limitations and deferred features

None of the items below are currently wired. Call-site TODOs live in
the source or the plans directory — no code here invents a missing
surface.

### 12.1 Not exposed from Lua

- **ZDO bind / unbind.** The `zhc_adapter` has no bind entry point
  and `zhac_lua_module.cpp` exports no such function. Use REST
  `POST /api/devices/:ieee/bind` instead.
- **Group multicast.** No `zhac.group_*` functions. Use REST
  `POST /api/groups/:id/cmd` or script multiple per-device
  `zhac.set_attr` calls.
- **Raw ZCL write.** `zhac.set_attr_raw` is not exported. For
  unmapped attributes you need a ZHC port (see `embedded/zhc/`).
- **Persistent KV.** No `zhac.kv_*` module. Persist state via the
  device shadow on a real device, or wait for the KV store plan
  (`docs/FEATURES.md` §7.2).
- **Raw frame capture (`ZCL_RAW`).** Not wired into `zhac.on_*`.
  Unhandled cluster frames still hit the EventBus but no Lua hook
  routes them in. Workaround: bridge to MQTT and subscribe from
  outside.
- **Alerts API.** Scripts cannot emit HAP `ALERT` frames.

### 12.2 Not enforced

- **No per-script timeout.** A handler that never yields hangs
  TaskLua until the next reboot.
- **No error-rate auto-disable.** An infinitely-crashing handler
  keeps firing; only the error counter bumps.
- **No script-to-script calls.** Coroutines spawned from different
  registrations share the master state but there is no convention
  for calling one from another. `require "<other_name>"` works (via
  the SPIFFS loader) but evaluates the other script top-to-bottom
  into a module table each time — it's not a cheap operation.

### 12.3 Bundled and optional library modules

**Bundled (default on):**

- **`cjson`** (`CONFIG_LUA_ENGINE_WITH_CJSON`, default **y**). JSON
  encode/decode from openresty/lua-cjson 2.1.0.14, vendored under
  `components/lua_cjson/src/`. `require "cjson"` returns the module;
  `cjson.encode(tbl)` / `cjson.decode(str)` are the core entry points.
  Decode-depth cap is set at init from
  `CONFIG_LUA_ENGINE_CJSON_DECODE_MAX_DEPTH` (default 64) to harden
  against stack blow-ups on attacker-controlled payloads. See
  [lua-cjson manual](https://github.com/openresty/lua-cjson) for the
  full API (`encode_sparse_array`, `encode_max_depth`, `null`, etc.).

**Off-by-default** — flip the Kconfig flag, drop the sources under
`components/<module>/`, rebuild:

- **`lpeg`** (`CONFIG_LUA_ENGINE_WITH_LPEG`). Pattern matching.
  Expects `luaopen_lpeg`.
- **`miniz`** (`CONFIG_LUA_ENGINE_WITH_MINIZ`). zlib
  compress/decompress. Expects `luaopen_miniz`.

---

## See also

- `docs/RULES_DSL.md` — rule engine grammar, including the
  `DO script.run "<name>"` action used to dispatch into Lua.
- `docs/REST_API.md` — REST surface for script CRUD.
- `docs/web/zhac_web_ui.md` — WebUI walkthrough.
- `docs/plans/2026-04-21-lua-engine-plan.md` — historical design
  brief; marked DONE.
- `components/lua_engine/README.md` — component-local notes and
  phase tracker.
