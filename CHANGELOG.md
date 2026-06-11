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

- **build**: `main` now REQUIREs `zigbee_backend ezsp_backend ezsp_driver`
  unconditionally — the previous `if(CONFIG_ZHAC_NCP_EZSP)` never fired
  because ESP-IDF expands component requirements before sdkconfig is
  generated, so fresh EZSP-enabled builds failed on `ezsp_backend.h`.
  Config selection stays in the preprocessor (TU-gated ezsp sources), so
  ZNP images carry no EZSP object code. Also `-Wno-error=cpp` so the F14
  "EZSP is experimental" `#warning` stays loud without breaking the F17
  `-Werror` build in EZSP configs. (`main/CMakeLists.txt`)

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

- **ezsp_driver / ezsp_backend**: the experimental EZSP/ASH backend is now
  compile-gated OFF by default. New `CONFIG_ZHAC_EZSP_BACKEND_ENABLE`
  (default `n`) whole-TU-gates both components (same mechanism as
  `CONFIG_HAP_BENCHMARK`) and hides `ZHAC_NCP_EZSP` in the NCP Protocol
  choice — default builds ship none of this code. The link layer has
  structural defects (no retransmit path, conflated ACK counters, C3/C4
  response-pump deadlock) deferred to a full rework
  (`extra/docs/EZSP_ASH_REWORK_PLAN.md`, workspace-local). The gated code
  itself was made wire-safe: `ash_encode_data` rejects payloads that
  overflow its `raw[EZSP_MAX_PAYLOAD+4]` frame buffer (a 255-byte
  `payload_len` overflowed it by ~56 bytes), `ezsp_sreq` rejects
  `payload_len > EZSP_MAX_PAYLOAD - 5` before building the frame, and the
  SREQ response path no longer calls FreeRTOS APIs
  (`xSemaphoreGive`/`Take`) or runs a ~200-byte memcpy inside
  `portENTER_CRITICAL` — forbidden on the dual-core P4. Only the
  match/arm decision stays under the spinlock; copy and semaphore
  give/drain happen after `portEXIT_CRITICAL`, give strictly after the
  copy. (`ezsp_driver.cpp:134,226,435,446`)

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

### Fixed — High (P1 findings review, zigbee_pool callers)

- **zigbee_backend**: every pool access now follows the locked contract
  (`pool_find_*` pointers are only valid under `zigbee_pool_lock()`;
  swap-with-last `pool_remove` retargets slots). `zb_write_attr`
  snapshots the device under the lock and runs the blocking radio sends
  on the copy; `zb_get_device` performs the `*out = *dev` copy under the
  lock; `zb_remove_device` copies `nwk_addr` out before the blocking
  leave request; `zb_rename_device` mutates via the new
  `zigbee_pool_with_device` visitor and NVS-marks a detached snapshot
  outside the mutex (`zigbee_backend.cpp:47,103,110,119` pre-fix —
  the `:93-95` comment documented the hazard). (F6/F35)
- **hap_dispatch**: unlocked pool access fixed across the dispatcher —
  `emit_attr_update` copies lqi/last_seen under the lock (`:153`
  pre-fix); `handle_get_devices` holds the lock across the
  `pool_all()` iteration inside the CPU-only JSON encode, matching the
  `handle_configure_req` house pattern (`:314` pre-fix);
  `handle_get_device_by_id` (`:387`) and `handle_set_attribute`'s
  cluster path (`:426`) snapshot under the lock and encode/send from
  the copy; `handle_device_set_name` renames via the
  `zigbee_pool_with_device` visitor; `handle_bind_req` and
  `handle_device_delete` copy `nwk_addr` (+ soft-remove mark/snapshot)
  under one lock before the blocking ZDO/leave calls. (F6/F35)
- **lua_engine**: `zhac.set_attr` snapshots the device under the pool
  lock before the blocking adapter sends (`zhac_lua_module.cpp:139`
  pre-fix). (F6/F35)

Note: the `ezsp_backend` pool call-sites are intentionally untouched —
the component is compile-gated off (`CONFIG_ZHAC_NCP_ZNP=y`) and its
rework is tracked in `extra/docs/EZSP_ASH_REWORK_PLAN.md`.

### Fixed — High (P1 findings review, hap_dispatch NVS)

- **hap_dispatch**: `handle_zigbee_cfg_set` ignored its
  `nvs_set_u8`/`nvs_set_blob`/`nvs_commit` returns and acked `ok=true`
  regardless (:1085 pre-fix) — a failed channel/network-key persist was
  reported to the S3/UI as accepted. NVS errors now propagate into the
  ack (`nvs_seq` from `zap_common/nvs_checked.h`). An out-of-range
  channel (outside 11-26) used to be silently dropped while the request
  was still acked ok; it now NAKs the whole request with no partial
  write, so the caller sees the rejection (the ack still carries the
  current stored values).
- **hap_dispatch**: polish fold — the "regenerated random network key"
  INFO fired before `set_blob`/`commit`, claiming an outcome that could
  still fail; reworded to intent ("generating random network key") — the
  post-commit `net_key=updated` line remains the persisted-success
  confirmation.

### Compatibility note

- **hap_dispatch**: no protocol or behaviour change on the P4 side
  associated with the S3-side `s_p4_device_count` cleanup (HAP F-10 in
  `zhac-net-core`). The P4 already emits `pool_count_active()` in the
  HEARTBEAT payload; the S3 now treats HEARTBEAT as the single writer.
