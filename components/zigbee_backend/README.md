# zigbee_backend — DeviceBackend Adapter for ZNP

Thin one-file adapter that exposes `zigbee_mgr` (the orchestrator over the TI CC2652 ZNP path) to the protocol-agnostic `DeviceBackend` registry. No new logic — pure delegation. Other higher layers (REST/WS handlers, rules engine, HAP dispatch) only see the abstract vtable; the ZNP-specific code never leaks past this file.

## Where it sits

```
api_handlers.cpp / ws_bridge.cpp / simple_rules / hap_dispatch
                       │
                       │  device_backend_find(PROTO_ZIGBEE)->write_attr(...)
                       ▼
              ┌────────────────────┐
              │  device_backend    │  registry (max 4 entries)
              └────────┬───────────┘
                       │ vtable
                       ▼
              ┌────────────────────┐
              │  zigbee_backend    │  THIS COMPONENT
              └────────┬───────────┘
                       │ direct calls
                       ▼
   zigbee_mgr · zigbee_pool · zap_store · zhc_adapter · znp_transport
```

`is_running` consults **both** the manager crash flag and the underlying
ZNP transport state — a transport stuck in `Recovering` / `Error`
previously looked healthy to higher layers (CODEX §5).

## CMakeLists

```cmake
idf_component_register(
    SRCS         "zigbee_backend.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     zigbee_mgr zap_common zap_store device_backend zhc_adapter
)
```

`znp_transport.h` is pulled in privately for the `znp_get_state()` /
`ZnpTransportState` query inside `is_running`.

## Public API (`include/zigbee_backend.h`)

```cpp
DeviceBackend* zigbee_backend_instance();   // singleton
bool           zigbee_backend_register();   // device_backend_register(instance())
```

## DeviceBackend vtable bindings

| Method            | Implementation                                                                 |
|-------------------|---------------------------------------------------------------------------------|
| `protocol`        | `PROTO_ZIGBEE`                                                                 |
| `name`            | `"Zigbee"`                                                                     |
| `init`            | `zigbee_mgr_init()`                                                            |
| `poll`            | `nullptr` — `znp_driver` runs its own RX/worker tasks                          |
| `is_running`      | `!zigbee_mgr_crashed() && znp_get_state() == ZnpTransportState::Up`            |
| `start_discovery` | `zigbee_permit_join(duration_s)`                                               |
| `stop_discovery`  | `zigbee_permit_join(0)`                                                        |
| `interview`       | `zigbee_interview_trigger(ieee)` — propagates real scheduling result (CODEX §7)|
| `write_attr`      | ZHC adapter first, then direct `zigbee_zcl_*` fallback for `state` / `brightness` / `color_temp` |
| `read_attr`       | TODO — returns `false`                                                         |
| `get_device_list` | `pool_lock` + `memcpy(pool_all(), …)` + `pool_unlock` (atomic snapshot)        |
| `get_device`      | `pool_find_by_ieee` + struct copy                                              |
| `remove_device`   | `zigbee_leave_req` + `zigbee_pool_remove` + `zap_store_delete_device`          |
| `rename_device`   | mutate `friendly_name` in pool + `zap_store_save_device`                       |

### `write_attr` dispatch

The endpoint resolution rule changed (QWEN §11): callers must pass the
specific endpoint from the request. `ep == 0` falls back to the device's
first registered endpoint — never to a hard-coded `1`.

```
write_attr(ieee, ep, key, val)
  │
  ├── pool_find_by_ieee → ZapDevice*
  │      └── ep := (ep != 0) ? ep : (dev->endpoint_count ? dev->endpoints[0] : 1)
  │
  ├── zhac_adapter_send_uint(ieee, model, manuf, nwk, ep, key, (uint64_t)val)
  │      └── true → done
  │
  └── direct fallback (legacy compatibility):
          "state"      → zigbee_zcl_on_off(nwk, ep, val ? 0x01 : 0x00)
          "brightness" → zigbee_zcl_level (nwk, ep, (uint8_t)val, 0)
          "color_temp" → zigbee_zcl_color_temp(nwk, ep, (uint16_t)val, 0)
          else         → log "no handler for key=…", return false
```

## Threading

No locks of its own. Inherits the recursive pool mutex (`zigbee_pool_lock`)
for `get_device_list`, the ZNP transport's internal serialization for the
ZCL helpers, and the NVS mutex for `remove_device` / `rename_device`.

## Integration

```cpp
#include "zigbee_backend.h"
#include "device_backend.h"

void boot() {
    zigbee_backend_register();   // installs into device_backend registry

    DeviceBackend* zb = device_backend_find(PROTO_ZIGBEE);
    if (zb && zb->init()) {
        zb->start_discovery(60);
        zb->write_attr(0x00124B0012345678ULL, /*ep=*/1, "state", 1);
    }
}
```

## Failure modes

| Condition | Behaviour |
|-----------|-----------|
| `device_backend_register` returns false | registry full or duplicate `protocol` — fatal at startup, log E |
| `pool_find_by_ieee` miss in `write_attr` | log W, return false |
| ZHC adapter miss + key not in fallback table | log W with key + IEEE, return false |
| `read_attr` called | always returns false (TODO) |

## Cross-references

- `components/device_backend/README.md` — registry + vtable definition
- `components/zigbee_mgr/README.md` — orchestration target of every delegation
- `components/zhc_adapter/README.md` — ZHC TzConverter resolution used by `write_attr`
- `components/ezsp_backend/README.md` — sibling adapter for the EFR32 path
- `docs/FINDINGS.md` — CODEX §5 (`is_running` transport check), §7 (interview return propagation), §11 (per-call endpoint), ZB-F3 (`zigbee_mgr` still calls ZNP-direct in places)
