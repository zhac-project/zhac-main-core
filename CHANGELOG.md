# Changelog

All notable changes to `zhac-main-core` (ESP32-P4 firmware) are documented
in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
the platform-wide `vYYYYMMDDVV` scheme tagged from `zhac-platform`.

## [Unreleased]

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
