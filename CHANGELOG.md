# Changelog

All notable changes to `zhac-main-core` (ESP32-P4 firmware) are documented
in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
the platform-wide `vYYYYMMDDVV` scheme tagged from `zhac-platform`.

## [Unreleased]

### Added

- **hap_dispatch**: `handle_device_options_set` applies the new per-device
  `throttle_ms` shadow rate-limit (clamped ≤ 600000 ms) alongside
  `occupancy_timeout` / `debounce_ms`. Lets the user throttle flood-prone
  Tuya-DP sensors from the S3 API. (#84)
- **ezsp_backend**: the EZSP radio path now emits a ZCL Default Response for
  unicast frames whose disable-default-response bit is clear, reaching parity
  with the live ZNP path (`zigbee_mgr`). Same gate — skips frames that are
  themselves responses and honors the opt-out bit; builds the DR via
  `EZSP_SEND_UNICAST`. EZSP is not the primary NCP today
  (`CONFIG_ZHAC_NCP_ZNP=y`), so this only takes effect if the backend is
  switched to EZSP — added so the two backends behave identically.

### Fixed — Critical (P0 findings review)

- **lua_engine**: coroutine registry refs are now released on `LUA_YIELD`,
  not only on finish/error. `zhac.sleep` takes a FRESH registry ref on every
  call before yielding, so the ref the resumer came in with was already
  redundant the moment the coroutine yielded — but `resume_coroutine` kept
  it, leaking one registry slot per sleep iteration (`while true do
  zhac.sleep(1000) end` exhausted the 4 MB Lua heap budget over days), and
  `dispatch_event` / `run_named_script` kept their spawn-time ref after a
  yielding handler / `script.run`, leaking a ref and pinning the finished
  coroutine forever. The resume/status/unref logic — previously three
  drifting copies — now lives in a single `resume_and_settle()` helper (plus
  a shared `spawn_coroutine()` for the newthread/xmove/ref sequence) that
  enforces the invariant: exactly one live registry ref per suspended
  coroutine, owned by whatever will resume it.
  (`lua_scheduler.cpp:326,480,558`)

- **lua_engine**: `lua_engine_load_all()` no longer compiles + spawns the
  stored scripts on the calling task (app_main). With ≥2 stored scripts,
  script 1's spawn pushes an immediate resume that TaskLua — unpinned,
  genuinely parallel on the dual-core P4 — could be executing on `g_L`
  while app_main was still compiling script 2: two tasks inside one
  unlocked `lua_State`, i.e. VM corruption at boot. The public entry now
  just enqueues a new `MSG_LOAD_ALL` message (fire-and-forget; the sole
  caller never consumed a result) and the pass executes inside the
  TaskLua queue handler, serialising every `lua_State` touch on the one
  task that owns it. In-handler, each script's first resume runs inline
  via `spawn_coroutine()`/`resume_and_settle()` (the Task-2 spawn path,
  same as `dispatch_event`/`run_named_script`) rather than detouring
  through `lua_scheduler_spawn`'s push-to-self — which would have eaten
  up to `LUA_SCRIPT_MAX` (16) queue slots mid-handler (deterministic
  overflow at the Kconfig-minimum depth of 8, silently dropping scripts)
  and let an already-queued event dispatch before the scripts' top-level
  registrations existed. Per-script failure tolerance is unchanged: a
  script that fails to read/compile/spawn is logged and skipped, never
  aborting the loop (no boot-loop from one broken file).
  (`lua_engine.c:118`, now `lua_scheduler.cpp`)

### Fixed — Medium (HAP stack review, 02-hap-stack.md)

- **hap_dispatch**: guard every handler that uses static-local scratch
  (`handle_rule_list_req` / `handle_script_write` /
  `handle_script_check_req` / `handle_script_list_req` /
  `handle_script_read_req` / `handle_get_device_by_id`) with a
  lazy-capture helper `hap_dispatch_assert_single_task()`. The actual
  dispatcher is `hap_slave_task` (the `on_frame` callback registered
  via `hap_session_init`), not `task_hap` — the first guarded call
  captures whichever task is dispatching, subsequent calls assert
  match. Eager-capturing `task_hap`'s handle panicked the P4 on the
  first SCRIPT_LIST_REQ after boot. Stack-allocating the buffers was
  the alternative — assertion preferred to avoid the additional ~9 KB
  stack pressure on the dispatcher task. (HAP F-08)

### Changed — Important (Zigbee/persistence review, 03-zigbee-persistence.md)

- **main.cpp**: reorder `task_zigbee` boot so the device pool comes up
  before the shadow consumer. `zigbee_pool_init` +
  `zigbee_pool_restore_persisted` now run explicitly, followed by
  `device_shadow_init` (init-only) and `device_shadow_restore_from_pool`
  under the pool lock. Removes the second NVS namespace open + 84 KB
  scratch alloc that the shadow used to perform when the pool was not
  yet available. `zigbee_mgr_init` no longer owns pool bootstrap. Boot
  is flat instead of ~4 s for a 200-device fleet. (F-06)

### Compatibility note

- **hap_dispatch**: no protocol or behaviour change on the P4 side
  associated with the S3-side `s_p4_device_count` cleanup (HAP F-10 in
  `zhac-net-core`). The P4 already emits `pool_count_active()` in the
  HEARTBEAT payload; the S3 now treats HEARTBEAT as the single writer.
</content>
