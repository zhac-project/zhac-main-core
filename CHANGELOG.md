# Changelog

All notable changes to `zhac-main-core` (ESP32-P4 firmware) are documented
in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
the platform-wide `vYYYYMMDDVV` scheme tagged from `zhac-platform`.

## [Unreleased]

### Fixed

- **hap_slave (P4-T31, FINDINGS HAP, `hap_slave.cpp`)** — documented the
  in-place DMA-buffer dispatch lifetime. On the verified-payload path
  `peer.payload` is set to `s_rx_buf`, the LIVE DMA receive buffer, and is valid
  ONLY for the duration of the synchronous `s_cb` call (the next exchange
  overwrites it). Added a comment making explicit that a handler MUST NOT stash
  `peer.payload` for async use — copy out if needed. The slave dispatches in
  place (vs the master's copy-to-dispatch-buffer) precisely because the P4 HAP
  dispatcher is single-task (F-08) and consumes synchronously. Comment only; no
  copy added.
- **hap_dispatch (P4-T31, FINDINGS HAP, `hap_dispatch.cpp` `handle_metrics_req`)**
  — routed the METRICS_RSP reply through the shared `hap_send` wrapper instead
  of hand-rolling the `HapFrame` (`hap_make_reply` + manual `seq`/`payload`/
  `payload_len` + `hap_session_send`). It was the only handler that open-coded
  its reply, a drift risk. Verified field-for-field equivalence first
  (`hap_make_reply` sets `ack_seq = f.seq`; `hap_send` reproduces seq/ack_seq/
  flags/type/payload identically), then collapsed the 5 lines to one
  `hap_send(METRICS_RSP, tx_buf, n, HAP_FLAG_NO_ACK, f.seq)` matching the
  RULE_LIST_RSP site. Behaviour unchanged.
- **hap_dispatch (P4-T31, FINDINGS HAP, `hap_dispatch.cpp` `handle_script_write`)**
  — commented the `static char src[HAP_SCRIPT_MAX_SRC + 1]` scratch: it is
  static (too big for the stack) and safe to share across calls ONLY because the
  `hap_dispatch_assert_single_task()` (F-08) guard above guarantees a single
  dispatch task; a 2nd dispatch task would alias it. Comment only.
- **hap_dispatch (P4-T28, FINDINGS §8)** — updated the heartbeat for the
  caller-owned `sys_metrics` CPU% baseline. `zap_common/sys_metrics.h` dropped
  its shared per-translation-unit `static` baseline (which two call sites or
  racing tasks could corrupt) in favour of a caller-supplied
  `sys_metrics_cpu_ctx_t`. `send_heartbeat` now keeps a private file-scope
  `s_hb_cpu_ctx` (touched only by the single heartbeat task, so no lock) and
  passes it to `sys_metrics_sample_cpu_pct(ctx, c0, c1)`.
- **hap_dispatch (HOTFIX)** — `handle_get_devices` is now PAGED. The device
  list previously timed out for anyone with ~15+ devices: a full fleet's JSON
  exceeds one SPI frame (`HAP_MAX_PAYLOAD` = 4096; overflow confirmed at 16
  devices), the encoder returned `false`, and the handler logged
  "GET_DEVICES encode failed" and **returned without sending** — so the S3's
  `GET_DEVICES` roundtrip always timed out. The handler now parses a
  `uint16` LE `start_index` cursor from the request payload (empty/legacy
  payload → 0), encodes one page from there via the paged
  `hap_json_encode_device_list`, and emits a `DEVICE_LIST` whose envelope
  carries the `next` cursor for the S3 to follow. `hap_send` now returns
  `bool` (propagating `hap_session_send`) and the handler **checks it** —
  a dropped reply is logged instead of silently leaving the S3 to time out.
  `sdkconfig` — it previously baked `mqtt://localhost`, a stray dev default with
  no business in a published image. The broker URL is set at runtime; defaults
  (`sdkconfig.defaults` / `sdkconfig.prod.defaults`) do not set the key, so the
  empty value is not reintroduced.

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

### Fixed — High/Medium (P2 findings review, T20 — P4 dispatch/OTA + Lua)

- **lua_engine**: `on_attr_change` handlers received **0 / garbage ints
  and truncated key/value strings** since event_bus schema v6. The Lua
  event-arg unpacker read the value union at a hardcoded offset 36 (the
  v5 layout, when `key` was 20 B) and sized the named-trigger struct
  `key`/`str_val` at 20/32 — but v6 widened `ATTR_KEY_MAX` 20→28 and
  `ATTR_STR_MAX` 32→48, sliding the value union to offset 44. The
  `LUA_EVT_ATTR` marshalling now derives every field offset from
  `offsetof(ZclAttrEvent, …)` against the canonical `zcl_attribute.h` /
  `event_bus.h` structs (no literals left), so the layout tracks the
  schema automatically; `RunNamedArgs.key`/`.str_val` are sized from
  `ATTR_KEY_MAX`/`ATTR_STR_MAX`. (`lua_scheduler.cpp` `push_event_args`
  + `RunNamedArgs`)
- **lua_engine**: installed a `lua_atpanic` backstop so an unprotected
  out-of-memory raise no longer reboots the chip. The scheduler touches
  the VM (`lua_newthread`/`luaL_ref`/`lua_pushstring`) from frames with no
  `pcall` above them; when the budgeted allocator returned NULL at the cap
  there, Lua's default panic = `abort()` → every such event became a
  reboot. The panic handler now `longjmp`s to a `setjmp` frame wrapped
  around each dispatch step in `task_lua` — the bad step is abandoned and
  counted, the engine survives. Paired with a 64 KB allocator headroom
  reserve (`lua_alloc.c`) so the scheduler's own bookkeeping is far less
  likely to hit NULL mid-dispatch. (`lua_scheduler.cpp`, `lua_alloc.c`)
- **lua_engine**: `on_timer_fire` cleared the timer slot's `in_use` flag
  BEFORE its `coroutine_ref`; a cross-task `slot_acquire` (TaskLua) landing
  between the two stores could write a fresh ref that this store then
  clobbered to -1, orphaning the newly-parked coroutine. Now clears
  `coroutine_ref` first, so the slot is never claimable with a stale ref.
  (`lua_scheduler.cpp:on_timer_fire`)
- **lua_engine**: script cache hardening. The read path now `stat`s the
  file first and FAILs (rather than silently truncating at the buffer cap)
  if a stored script is larger than the destination — an oversize file no
  longer half-compiles and runs the wrong code. The write path now checks
  `fflush` AND `fclose` returns BEFORE the tmp→final rename, so a
  SPIFFS-full flush failure can't promote a truncated `.tmp` into place
  reporting success. Source-size caps unified under one
  `LUA_SCRIPT_SRC_MAX` (was divergent 8 KB / 16 KB literals across the
  write limit and the two read buffers). (`lua_script_cache.cpp`,
  `lua_engine_scripts.h`, `lua_scheduler.cpp`)
- **hap_dispatch**: `CONFIGURE_REQ` no longer blocks the HAP dispatcher.
  `zhac_adapter_configure` runs a radio pipeline that blocks ~2.5 s/step
  on AF_DATA_CONFIRM; running it synchronously on the dispatch task stalled
  heartbeats long enough to risk a link-dead/retry storm. The handler now
  snapshots the device identity under the pool lock and enqueues the work
  to a dedicated low-priority `cfg_worker` task, returning immediately
  (CONFIGURE_REQ is fire-and-forget per `hap_protocol.h` — no ACK type
  exists and the configure result still reaches S3 via the normal
  attr-report path). On queue-full the request is logged + dropped
  (user-retriable). (`hap_dispatch.cpp:handle_configure_req` + `task_hap`)
- **p4_ota**: OTA flashing no longer multi-second-stalls the dispatcher.
  `esp_ota_begin` now uses `OTA_WITH_SEQUENTIAL_WRITES` (lazy per-write
  sector erase) instead of passing the image size, which forced a
  whole-partition erase up front. Added a 60 s idle-abort watchdog
  (`esp_timer`) that frees an abandoned OTA session/handle if S3 stops
  sending, and a persistence flush (`rule_store_flush_now` +
  `zap_store_flush_now`) before `esp_restart()` so an OTA reboot can't drop
  freshly-edited rules/device state still in the writeback caches.
  (`p4_ota.cpp`)
- **main / watchdog**: replaced the tautological task-watchdog feeder (a
  dedicated always-healthy task that pet the dog regardless of whether the
  real loops were alive) with genuine coverage. `TaskHAP`, `TaskLua`, and
  `TaskEventBus` now subscribe themselves (`esp_task_wdt_add`) and reset
  from their own iterations; `TaskLua` uses a finite (2 s) queue-receive
  timeout so an idle engine still pets the dog. `TaskZigbee` is covered in
  its 100 ms steady-state monitor but UNSUBSCRIBES around the
  crash-recovery backoff (delays up to 60 s) so a legitimate long backoff
  can't false-trip the 10 s window. (`main.cpp`, `hap_dispatch.cpp`,
  `lua_scheduler.cpp`)
- **main / security**: the boot-time permit-join auto-open (~250 s on
  every boot AND after every ZNP recovery) is now gated behind
  `CONFIG_ZHAC_PERMIT_JOIN_ON_BOOT` (default **n**). Opening the network
  for joining unattended on every power cycle was a standing exposure;
  pairing remains available on demand via the SPA / `PERMIT_JOIN` message /
  join button. (`main.cpp`, `main/Kconfig.projbuild`)
- **hap_dispatch**: `SET_ATTRIBUTE` now ALWAYS sends `SET_ACK`, including on
  the zhc-adapter (`tz_converter`) path. The previous `if (!sent)` guard
  suppressed the ack when the adapter handled the write — but the adapter
  path emits a Zigbee command, not a HAP reply, so S3's seq-correlated
  waiter timed out on every adapter-routed SET (pairs with T14).
  (`hap_dispatch.cpp:handle_set_attribute`)
- **hap_dispatch**: `PERMIT_JOIN` duration is clamped to ≤254 — 255 is
  "permanently open" per ZDO Mgmt_Permit_Joining and would leave the
  network open with no auto-close. `TIME_SYNC` now rejects timestamps
  before 2020-01-01 UTC (0/garbage from an S3 that hasn't acquired time);
  accepting them set the clock to ~1970 and broke every cron rule.
  (`hap_dispatch.cpp:handle_permit_join`, `handle_time_sync`)

### Compatibility note

- **hap_dispatch**: no protocol or behaviour change on the P4 side
  associated with the S3-side `s_p4_device_count` cleanup (HAP F-10 in
  `zhac-net-core`). The P4 already emits `pool_count_active()` in the
  HEARTBEAT payload; the S3 now treats HEARTBEAT as the single writer.
