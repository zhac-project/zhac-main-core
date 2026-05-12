# ezsp_backend — DeviceBackend Adapter for EZSP / EFR32

`DeviceBackend` adapter that wraps `ezsp_driver` for Silicon Labs EFR32 NCPs (EmberZNet over EZSP/ASH). Sits in the same registry slot family as `zigbee_backend` (both report `PROTO_ZIGBEE`); only one of the two is active at a time.

> **Status:** partial. The driver itself lacks ASH retransmit / NAK / sliding-window handling (see `docs/FINDINGS.md` ZB-F1) — the backend can boot a coordinator and route incoming frames, but the adapter is not yet ready to ship as the primary radio path.

## Where it sits

```
api_handlers.cpp / ws_bridge / hap_dispatch / simple_rules
                            │  device_backend_find(PROTO_ZIGBEE)
                            ▼
                    device_backend (vtable registry)
                            │
                            ▼
                    ezsp_backend  (THIS COMPONENT)
                            │
                ┌───────────┼─────────────┐
                ▼           ▼             ▼
         ezsp_driver  zigbee_pool   device_shadow
            (UART)        (PSRAM)        (NVS cache)
```

The adapter shares `zigbee_pool` and `zap_store` with the ZNP path so an
operator switching radios doesn't lose paired devices.

## CMakeLists

```cmake
idf_component_register(
    SRCS         "ezsp_backend.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     ezsp_driver zigbee_mgr zap_common zap_store device_backend
                 zhc_adapter device_shadow esp_timer
)
```

## Public API (`include/ezsp_backend.h`)

```cpp
DeviceBackend* ezsp_backend_instance();   // singleton
bool           ezsp_backend_register();   // device_backend_register(instance())
```

## DeviceBackend vtable bindings

| Method            | Implementation |
|-------------------|----------------|
| `protocol`        | `PROTO_ZIGBEE` |
| `name`            | `"EZSP"` |
| `init`            | EZSP coordinator startup sequence (see below) |
| `poll`            | `ezsp_driver_poll()` — EZSP driver doesn't own its own RX task |
| `is_running`      | coordinator-state check + EZSP `NETWORK_STATE` query |
| `start_discovery` | `EZSP_PERMIT_JOINING` |
| `stop_discovery`  | `EZSP_PERMIT_JOINING` with duration=0 |
| `interview`       | TODO — currently returns false |
| `write_attr`      | `EZSP_SEND_UNICAST` with built-in mappings for `state`/`brightness`/`color_temp` |
| `read_attr`       | TODO — returns false |
| `get_device_list` | `pool_lock` + memcpy snapshot (shared with ZNP) |
| `get_device`      | `pool_find_by_ieee` |
| `remove_device`   | `EZSP_LEAVE_NETWORK` request + pool remove + `zap_store_delete_device` |
| `rename_device`   | mutate name + `zap_store_save_device` |

## EZSP coordinator startup

1. `ezsp_hw_reset()` — drive nRESET.
2. `ezsp_ash_reset()` — send ASH `RST`, wait for `RSTACK`.
3. `EZSP_VERSION` — protocol handshake.
4. `EZSP_GET_EUI64` — coordinator IEEE.
5. `EZSP_SET_INITIAL_SECURITY` — install HA well-known link key.
6. `EZSP_NETWORK_INIT` (resume) → if `EMBER_NOT_JOINED`, `EZSP_FORM_NETWORK`.
7. `EZSP_ADD_ENDPOINT` — register endpoint 1 + cluster list.
8. `EZSP_SET_CONCENTRATOR` — many-to-one route announcer.

Subscribed callbacks (registered with `ezsp_register_callback`):

| Callback ID                  | Handler                  | Purpose |
|------------------------------|--------------------------|---------|
| `EZSP_CB_STACK_STATUS` 0x0019| `on_stack_status`        | network up/down/opened/closed |
| `EZSP_CB_TRUST_CENTER_JOIN` 0x0024 | `on_trust_center_join` | device join/leave (seed/remove pool) |
| `EZSP_CB_INCOMING_MESSAGE` 0x0045  | `on_incoming_message`  | route ZCL via `zhc_adapter_try_decode` → `device_shadow_process` |

## `write_attr` dispatch

```
write_attr(ieee, ep, key, val)
  │
  ├── pool_find_by_ieee → ZapDevice* (shared with ZNP path)
  │
  ├── ZHC adapter try (planned) — currently bypassed
  │
  └── direct mappings:
        "state"      → On/Off cluster 0x0006   (cmd 0x00/0x01)
        "brightness" → Level    cluster 0x0008 (cmd 0x04 MoveToLevel)
        "color_temp" → ColorCtl cluster 0x0300 (cmd 0x0A MoveToColorTemp)
        else         → log W, return false
```

Built using `EmberApsFrame` + `EZSP_SEND_UNICAST`. APS retry option
(`0x0040`) is set; APS-level acknowledgement is whatever the EFR32
firmware emits in its `EZSP_CB_MESSAGE_SENT` callback.

## Failure modes

| Condition | Behaviour |
|-----------|-----------|
| ASH `RSTACK` timeout | `init` returns false |
| EZSP version mismatch | log E, abort init |
| `interview` invoked | returns false (TODO) |
| Driver-level NAK / retransmit | **not yet implemented** — frames may be dropped under load (ZB-F1) |
| `read_attr` invoked | returns false |

## Threading

`ezsp_driver_poll` is meant to be called from a dedicated task (the
backend installs its own polling task during `init`). Pool access uses
`zigbee_pool_lock`; NVS access goes through `zap_store`.

## Integration

```cpp
#include "ezsp_backend.h"

void boot() {
    ezsp_backend_register();
    DeviceBackend* zb = device_backend_find(PROTO_ZIGBEE);
    if (zb && zb->init()) {
        zb->start_discovery(60);
        zb->write_attr(0x00124B0012345678ULL, /*ep=*/1, "state", 1);
    }
}
```

## EZSP vs ZNP

| Aspect            | ZNP (`zigbee_backend`)         | EZSP (`ezsp_backend`)          |
|-------------------|--------------------------------|--------------------------------|
| NCP vendor / chip | TI CC2652 / CC2530             | Silicon Labs EFR32MG           |
| Driver            | `znp_driver` (MT serial)       | `ezsp_driver` (EZSP/ASH)       |
| Default UART pins | TX=16 RX=17 RST=28 BSL=29      | TX=26 RX=27 RST=28             |
| Default timeout   | 2000 ms                        | 3000 ms                        |
| Interview         | Full                           | TODO                           |
| Bind / unbind     | Full                           | Partial                        |
| ZHC integration   | Full (decode + send)           | Decode only (send is direct)   |
| Production-ready  | Yes                            | No (ZB-F1)                     |

## Cross-references

- `components/ezsp_driver/README.md` — UART / ASH / EZSP framing details
- `components/zigbee_backend/README.md` — sibling ZNP adapter
- `components/device_backend/README.md` — vtable definition + registry
- `components/zhc_adapter/README.md` — incoming-message decode path
- `docs/FINDINGS.md` — ZB-F1 (driver gaps), ZB-F2 (`ezsp_sreq` ignores sequence), ZB-F3 (manager leaks abstraction)
