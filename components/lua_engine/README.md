# lua_engine — Sandboxed Lua VM (P4)

The project's single scripting runtime. PUC-Lua 5.5 vendored via the
`georgik/lua` ESP-IDF managed component, wrapped in a custom
allocator, sandbox, scheduler, and `zhac.*` native module. One
FreeRTOS task (`TaskLua`) owns the master state; scripts run as
coroutines off that state. Designed to coexist with `simple_rules`:
the rules engine is the everyday surface, Lua is the escape hatch
for logic that doesn't fit in a four-action DSL.

## Where it sits

```
SPIFFS /scripts/*.lua
        │
        ▼
lua_script_cache (mount, list, read, write, exists, delete)
        │
        ▼
lua_engine_init ─► lua_alloc + sandbox + require + zhac module
        │                                      │
        ▼                                      ▼
TaskLua (priority 4)            ◄── lua_engine_event_bridge ─◄ event_bus
   ▲     ▲                                                       (ATTR/MQTT/BOOT)
   │     │
   │     └── lua_engine_rules_hook ◄── simple_rules SCRIPT action
   │                  (queues MSG_RUN_NAMED on s_resume_q)
   │
   └── esp_timer pool (zhac.sleep) ──► MSG_RESUME on s_resume_q
```

P4 only. The S3 proxies REST `/scripts/*` over HAP but never hosts
the VM. All call paths into Lua go through `s_resume_q` —
`TaskLua` is the only task that ever touches `g_L`.

### Dependencies (`CMakeLists.txt` REQUIRES)

`zap_common` `event_bus` `simple_rules` `device_shadow`
`zigbee_mgr` `mqtt_client` `nvs_flash` `spiffs` `esp_timer`
`freertos` `metrics` + the managed component `georgik/lua ^5.5.0~7`.

## Public API

### `include/lua_engine.h`

| Symbol | Notes |
|---|---|
| `bool lua_engine_init()` | Brings up custom allocator, sandbox, native modules, `TaskLua`. Idempotent. **Must** be called after `nvs_flash_init`. |
| `void lua_engine_load_all()` | Scan `/spiffs/scripts/*.lua` and spawn a top-level coroutine per file. Registrations (`on_attr_change`, `on_mqtt`, `on_cron`, `on_boot`) are recorded so subsequent EventBus events can dispatch into them. |
| `void lua_engine_dispatch(const Event*)` | **No-op C-callable stub — no in-tree callers.** Real event→coroutine dispatch is done by the C++ EventBus bridge (`lua_engine_event_bridge.cpp`), which subscribes to `ATTR_CHANGE`/`MQTT_MSG`/`CTRL_BOOT`/`ZCL_RAW` directly. Kept only as a stable C entry point. |
| `size_t lua_engine_heap_used_bytes()` / `_peak_bytes()` | Live + high-water allocator counters (charged to `kBudgetBytes`). |
| `uint16_t lua_engine_live_coroutines()` | Currently registered coroutine refs. |
| `uint32_t lua_engine_error_count()` / `_yield_count()` | Lifetime counters. |
| `bool lua_engine_check_syntax(const char* name, const char* src, char* err_out, size_t err_cap, int* line_out)` | Throwaway-state syntax check (`luaL_loadbufferx` mode `"t"`). Safe to call off TaskLua. Used by REST upload to surface errors before save. |

### `include/lua_engine_scripts.h` — script cache

| Symbol | Notes |
|---|---|
| `lua_script_cache_list(LuaScriptEntry* out, uint16_t max)` | Enumerate `/scripts/*.lua`. Lazy mount + format-on-empty. |
| `lua_script_cache_read(name, out, cap)` | UTF-8 source into `out`; `cap` includes NUL. Returns bytes written or `-1`. |
| `lua_script_cache_write(name, src, len)` | Atomic write (tmp + rename). |
| `lua_script_cache_delete(name)` / `_exists(name)` | Self-explanatory. |
| `lua_engine_run_script(const char* name)` | Enqueue a fresh coroutine invocation with empty trigger context — used by the WebUI **Run** button. |
| `LUA_SCRIPT_NAME_MAX` | 24 chars (excl. NUL) |
| `LUA_SCRIPT_MAX` | 16 — soft cap on distinct files |

## Important constants & sizes

| Symbol | Default | Source |
|---|---|---|
| `LUA_RESUME_DEADLINE_US` | **50 000 µs** (50 ms) per resume | `lua_scheduler.cpp` (LUA-F1 guard) |
| `LUA_RESUME_HOOK_COUNT` | 1000 instructions between hook checks | `lua_scheduler.cpp` |
| `CONFIG_LUA_ENGINE_HEAP_KB` | 4096 KB total Lua heap | Kconfig (range 256–16384) |
| `CONFIG_LUA_ENGINE_INTERNAL_SMALL_THRESHOLD` | 512 B (small allocs try internal RAM first) | Kconfig |
| `CONFIG_LUA_ENGINE_MAX_COROUTINES` | 64 | Kconfig (range 4–256) |
| `CONFIG_LUA_ENGINE_TIMER_POOL` | 16 `esp_timer_t` slots for `zhac.sleep` | Kconfig |
| `CONFIG_LUA_ENGINE_RESUME_QUEUE_DEPTH` | 32 messages | Kconfig |
| `CONFIG_LUA_ENGINE_TASK_STACK_BYTES` | TaskLua stack (default 8 KB) | Kconfig |
| `CONFIG_LUA_ENGINE_TASK_PRIORITY` | TaskLua priority (default 4) | Kconfig |
| Script file cap | 16 KB per file | `lua_script_cache.cpp` |
| Script storage | SPIFFS partition `scripts`, mount `/scripts` | `lua_script_cache.cpp` |

## Threading & concurrency

- **Single-threaded TaskLua.** The only task that touches `g_L`. All
  external callers push a `LuaMsg` onto `s_resume_q`:

  | `LuaMsg.kind` | Source |
  |---|---|
  | `MSG_RESUME` | `esp_timer` after `zhac.sleep`, or external `lua_scheduler_push_resume` |
  | `MSG_EVENT` | `lua_engine_event_bridge` (ATTR / MQTT / BOOT) |
  | `MSG_RUN_NAMED` | `simple_rules` `DO script.run "<name>"` action |

- **CPU runaway guard (LUA-F1, 2026-04-25).** Every `lua_resume` is
  preceded by `arm_resume_deadline()`, which sets a count hook
  (`lua_sethook(co, hook, LUA_MASKCOUNT, 1000)`). The hook compares
  `esp_timer_get_time()` against the deadline and raises
  `"lua_engine: resume exceeded 50000 us budget"`. A misbehaving
  handler (e.g. `while true do end`) can no longer starve other
  coroutines for more than ~50 ms.
- **`zhac.sleep(ms)`** is a `lua_yield` + `esp_timer_start_once`;
  the timer fires `lua_scheduler_push_resume`. The VM only ever
  sees plain yield/resume — caller never blocks outside Lua.
- **Queue full** = silent drop + `METRIC_LUA_QUEUE_DROPS_TOTAL`
  bump. Producers (event bridge, rules hook, REST Run) log a warning.

## Sandbox

- **No filesystem.** `io.*` nil-ed; `dofile`, `loadfile`,
  `package.path/cpath/loadlib/searchpath` removed.
- **No process calls.** `os.execute`, `os.exit`, `os.getenv` removed.
- **No bytecode loading.** All `require` / `load` paths use
  `mode="t"` (text only).
- **`print` → `ESP_LOGI("lua", …)`.**
- **Locked stdlib metatables (LUA-F10, 2026-04-25).** Every public
  Lua sandbox CVE chain begins with `setmetatable(string, …)`. We
  install a string-typed `__metatable` on `string`, `table`, `math`,
  `os`, `coroutine`, `io` so `getmetatable` returns `"locked"` and
  `setmetatable` raises. User-table metatables are unaffected.
- **`require` resolves only:** (1) preloaded native modules
  (`zhac` always; `cjson` / `lpeg` / `miniz` if Kconfig'd), (2) the
  SPIFFS script cache.

### Allocator routing

`lua_engine_alloc` is the lua_Alloc plugged into `lua_newstate`.

- Allocations < `CONFIG_LUA_ENGINE_INTERNAL_SMALL_THRESHOLD` try
  internal RAM via `MALLOC_CAP_INTERNAL`, fall back to PSRAM on
  ENOMEM.
- Larger allocations go straight to PSRAM
  (`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`).
- Hard cap at `CONFIG_LUA_ENGINE_HEAP_KB * 1024` — exceeding
  returns `NULL`, which Lua converts to `not enough memory` and
  unwinds cleanly.
- Atomic counters track `s_used_bytes` + `s_peak_bytes`.

## On-disk / wire layout

Scripts are UTF-8 Lua source on the SPIFFS `scripts` partition,
mounted at `/scripts`. Atomic write via `tmp` + `rename`. Lazy
mount + format-on-empty inside `ensure_mounted()`. No bytecode
caches — every load recompiles.

## Failure modes

| Condition | Behaviour |
|---|---|
| `lua_newstate` fails (PSRAM exhausted) | `lua_engine_init` returns false; engine disabled |
| Resume budget exceeded (50 ms) | `luaL_error` raised inside script; coroutine unwinds; counted |
| `zhac.sleep` with timer pool exhausted | `zhac.sleep` returns error to script |
| Resume queue full | Message dropped, `METRIC_LUA_QUEUE_DROPS_TOTAL` bumped, warning logged |
| Heap budget exceeded | `lua_Alloc` returns `NULL`, Lua raises `not enough memory` |
| Syntax error during `lua_engine_check_syntax` | `err_out` filled, `line_out` set, returns false |
| SPIFFS mount fails | `lua_script_cache_*` returns error; format attempted on empty |

## Integration example

```c
// Boot order:
nvs_flash_init();
event_bus_init();
simple_rules_init();      // installs script hook in lua_engine_init()
device_shadow_init();
mqtt_client_start();

if (!lua_engine_init()) {
    ESP_LOGE(TAG, "lua engine disabled");
} else {
    lua_engine_load_all();   // /scripts/*.lua compiled + boot-handlers fired
}

// REST upload path:
char err[128]; int line = 0;
if (!lua_engine_check_syntax("motion", src, err, sizeof(err), &line))
    return rest_reply_error_line(line, err);
lua_script_cache_write("motion", src, len);

// Run from the WebUI button:
lua_engine_run_script("motion");
```

```lua
-- /scripts/heartbeat.lua
zhac.on_cron("*/5 * * * *", function()
    zhac.publish("home/heartbeat", tostring(zhac.millis()))
end)

zhac.on_attr_change(nil, "occupancy", function(v, key, ieee)
    if v then
        zhac.set_attr("0xCAFE", "state", 1)
        zhac.sleep(5 * 60 * 1000)
        zhac.set_attr("0xCAFE", "state", 0)
    end
end)
```

## Recent changes

- **2026-04-25 LUA-F1 — instruction-count CPU guard.** 50 ms
  per-resume deadline via `lua_sethook(LUA_MASKCOUNT, 1000)`.
- **2026-04-25 LUA-F10 — sandbox metatable lock.** `string`,
  `table`, `math`, `os`, `coroutine`, `io` all carry a string-typed
  `__metatable`.
- Earlier: routed allocator with internal-RAM-first / PSRAM
  fallback, SPIFFS-backed script cache replacing the prior NVS
  store, syntax-check entry point for REST.

## Cross-references

- `docs/LUA_API.md` — full `zhac.*` reference, lifecycle, examples
- `docs/FINDINGS.md` — LUA-F1, LUA-F4, LUA-F8, LUA-F10
- `components/simple_rules/README.md` — script hook caller
- `components/event_bus/README.md` — events bridged into Lua
- `components/cron_parser/README.md` — `zhac.on_cron` evaluator
- `firmware/zhac-main-core/main/idf_component.yml` — `georgik/lua`
  managed-component pin
