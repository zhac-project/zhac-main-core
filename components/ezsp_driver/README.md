# ezsp_driver — Silicon Labs EZSP / ASH UART Transport

Single-file UART transport for Silicon Labs EFR32MG NCPs (EmberZNet) using EZSP (EmberZNet Serial Protocol) over ASH (Asynchronous Serial Host) framing. Mirrors the `znp_driver` API surface so `zigbee_mgr` (via `ezsp_backend`) treats Silabs and TI radios interchangeably.

> **Status:** **incomplete** — flagged in `docs/FINDINGS.md` as **ZB-F1**.
> The driver currently lacks ASH retransmit, NAK handling, sliding-window
> ACK accounting, and a dedicated RX scheduler task. Synchronous calls
> work in steady state; under packet loss the link will hang. **Do not
> ship as the primary radio backend yet.**

## Where it sits

```
ezsp_backend (DeviceBackend adapter) ─── zigbee_mgr-shaped surface
        │  ezsp_sreq / ezsp_register_callback / ezsp_driver_poll
        ▼
┌───────────────── ezsp_driver ─────────────────┐
│  ezsp_driver.cpp  (single TU, ~19 KB)         │
│  - ASH state (frame numbers, RSTACK detect)   │
│  - ASH framing (CRC16, randomization, stuff)  │
│  - EZSP encode / decode / dispatch            │
│  - UART_NUM_1, polled from ezsp_driver_poll   │
└───────────────────────────────────────────────┘
        │  UART_NUM_1, 115200 8N1, no flow control
        ▼
   EFR32MG12 / MG21 NCP running NCP-UART firmware
```

## CMakeLists / Kconfig

```cmake
idf_component_register(
    SRCS         "ezsp_driver.cpp"
    INCLUDE_DIRS "include"
    REQUIRES     esp_driver_uart esp_driver_gpio freertos log
)
```

```Kconfig
menu "ZHAC EZSP NCP"
    config ZHAC_EZSP_UART_TX_GPIO   default 26
    config ZHAC_EZSP_UART_RX_GPIO   default 27
    config ZHAC_EZSP_UART_RST_GPIO  default 28
endmenu
```

## ASH wire format

```
[FLAG=0x7E]  [...frame body, byte-stuffed...]  [FLAG=0x7E]
```

Frame body for a DATA frame (before stuffing, after randomization):

```
[Control(1)] [randomized EZSP payload...] [CRC16 high] [CRC16 low]
```

| Byte / field | Notes |
|--------------|-------|
| `FLAG`   `0x7E` | frame delimiter |
| `ESCAPE` `0x7D` | escape byte; following byte is XOR-0x20 |
| `CANCEL` `0x1A` | mid-frame cancel — restart |
| `SUB`    `0x18` | substitute — frame error |
| `XON` / `XOFF` `0x11` / `0x13` | software flow control (escaped if seen in payload) |

### Byte stuffing (escape)

| Original | Escaped seq |
|----------|-------------|
| 0x7E (FLAG)   | `0x7D 0x5E` |
| 0x7D (ESCAPE) | `0x7D 0x5D` |
| 0x11 (XON)    | `0x7D 0x31` |
| 0x13 (XOFF)   | `0x7D 0x33` |
| 0x18 (SUB)    | `0x7D 0x38` |
| 0x1A (CANCEL) | `0x7D 0x3A` |

`needs_escape(b)` covers the same set; encoder writes `0x7D` followed
by `b ^ 0x20`.

### Randomization (LFSR)

DATA payloads are XOR-randomized before CRC and after CRC verification
on the receive side, with a Galois LFSR seeded `rand=0x42`,
polynomial `0xB8`. Suppresses long runs of identical bytes that would
otherwise confuse the UART line. XOR is its own inverse — the same
function is used for `derandomize`.

### CRC16-CCITT

Polynomial `0x1021`, initial value `0xFFFF`, computed over the control
byte plus the **randomized** payload.

```cpp
uint16_t ash_crc16(const uint8_t* data, size_t len);
```

### Frame types

| Type   | Encoder                          | Direction |
|--------|----------------------------------|-----------|
| DATA   | `ash_encode_data(frame_num, ack_num, retransmit, ...)` | both |
| ACK    | `ash_encode_ack(ack_num, ...)`   | both |
| RST    | `ash_encode_rst(...)`            | host → NCP |
| RSTACK | (decoded only)                   | NCP → host |
| NAK    | (decoded only) — currently logs W and continues; **does not retransmit** (ZB-F1) |
| ERROR  | (decoded only) — logs E |

## EZSP frame

```cpp
struct EzspFrame {
    uint8_t        sequence;       // EZSP transaction sequence
    uint16_t       frame_control;  // direction, extended flags
    uint16_t       command_id;     // EZSP command / response id
    const uint8_t* payload;        // non-owning
    uint8_t        payload_len;
};

EzspFrame ezsp_make_req(uint16_t command_id, const uint8_t* payload = nullptr,
                        uint8_t payload_len = 0);
```

> **Caveat (ZB-F2):** `ezsp_sreq` currently matches replies by
> `command_id` only and uses a shared response frame buffer. A second
> caller hitting the same command_id while the first is in flight can
> race. Caller-side serialization is required until the per-call
> reply-queue rework lands.

## EZSP commands implemented (declared in `include/ezsp_driver.h`)

| Constant                          | ID      |
|-----------------------------------|---------|
| `EZSP_VERSION`                    | 0x0000 |
| `EZSP_GET_CONFIG_VALUE`           | 0x0052 |
| `EZSP_SET_CONFIG_VALUE`           | 0x0053 |
| `EZSP_SET_VALUE`                  | 0x00AB |
| `EZSP_GET_VALUE`                  | 0x00AA |
| `EZSP_SET_POLICY`                 | 0x0055 |
| `EZSP_NETWORK_INIT`               | 0x0017 |
| `EZSP_NETWORK_STATE`              | 0x0018 |
| `EZSP_FORM_NETWORK`               | 0x001E |
| `EZSP_LEAVE_NETWORK`              | 0x0020 |
| `EZSP_PERMIT_JOINING`             | 0x0022 |
| `EZSP_GET_NETWORK_PARAMS`         | 0x0028 |
| `EZSP_GET_EUI64`                  | 0x0026 |
| `EZSP_GET_NODE_ID`                | 0x0027 |
| `EZSP_SEND_UNICAST`               | 0x0034 |
| `EZSP_SEND_BROADCAST`             | 0x0036 |
| `EZSP_SET_CONCENTRATOR`           | 0x0010 |
| `EZSP_SET_SOURCE_ROUTE`           | 0x00AE |
| `EZSP_ADD_ENDPOINT`               | 0x0002 |
| `EZSP_SET_INITIAL_SECURITY`       | 0x0068 |
| `EZSP_CLEAR_KEY_TABLE`            | 0x00B1 |
| `EZSP_START_SCAN`                 | 0x001A |

### EZSP callbacks (unsolicited)

| Constant                     | ID      | Purpose |
|------------------------------|---------|---------|
| `EZSP_CB_STACK_STATUS`       | 0x0019 | network up/down/opened/closed |
| `EZSP_CB_TRUST_CENTER_JOIN`  | 0x0024 | device join / leave |
| `EZSP_CB_INCOMING_MESSAGE`   | 0x0045 | incoming APS data |
| `EZSP_CB_MESSAGE_SENT`       | 0x003F | APS-level message-sent confirmation |
| `EZSP_CB_CHILD_JOIN`         | 0x0023 | end-device child join |

### EmberStatus values

```cpp
EMBER_SUCCESS         0x00
EMBER_NETWORK_UP      0x90
EMBER_NETWORK_DOWN    0x91
EMBER_NETWORK_OPENED  0x9C
EMBER_NETWORK_CLOSED  0x98
```

### EmberApsFrame (for `EZSP_SEND_UNICAST`)

```cpp
struct EmberApsFrame {
    uint16_t profile_id;     // 0x0104 = HA
    uint16_t cluster_id;
    uint8_t  src_endpoint;   // usually 1
    uint8_t  dst_endpoint;
    uint16_t options;        // 0x0040 = APS retry
    uint16_t group_id;
    uint8_t  sequence;
};
```

## Public API

```cpp
// ASH primitives
uint16_t ash_crc16    (const uint8_t* data, size_t len);
size_t   ash_encode_data(uint8_t frame_num, uint8_t ack_num, bool retransmit,
                         const uint8_t* ezsp_data, size_t ezsp_len,
                         uint8_t* out, size_t out_cap);
size_t   ash_encode_ack (uint8_t ack_num, uint8_t* out, size_t out_cap);
size_t   ash_encode_rst (uint8_t* out, size_t out_cap);

// Lifecycle
void ezsp_driver_init();                                  // UART + GPIO bring-up
void ezsp_hw_reset();                                     // pulse nRESET
bool ezsp_ash_reset(uint32_t timeout_ms = 5000);          // RST → wait for RSTACK

// Synchronous request
bool ezsp_sreq      (const EzspFrame& req, EzspFrame& rsp_out, uint32_t timeout_ms = 3000);
bool ezsp_sreq_retry(const EzspFrame& req, EzspFrame& rsp_out,
                     uint32_t timeout_ms = 3000, int max_attempts = 3);

// Async callbacks
using EzspCallbackFn = std::function<void(const EzspFrame&)>;
void ezsp_register_callback(uint16_t command_id, EzspCallbackFn cb);

// RX pump — call from a dedicated task loop
void ezsp_driver_poll();

// Helpers
EzspFrame ezsp_make_req(uint16_t command_id,
                        const uint8_t* payload = nullptr, uint8_t payload_len = 0);
```

## Hardware

| Signal | Default GPIO | Kconfig key                   |
|--------|--------------|--------------------------------|
| TX     | GPIO26       | `CONFIG_ZHAC_EZSP_UART_TX_GPIO`  |
| RX     | GPIO27       | `CONFIG_ZHAC_EZSP_UART_RX_GPIO`  |
| nRESET | GPIO28       | `CONFIG_ZHAC_EZSP_UART_RST_GPIO` |

UART `UART_NUM_1`, 115200 baud, 8N1, no flow control.
RX buffer `UART_BUF_SZ = 4096`, ASH frame buffer `ASH_MAX_FRAME = 512`,
EZSP payload cap `EZSP_MAX_PAYLOAD = 200`.

## Threading

- `ezsp_driver_poll()` is the single RX entry point; it must be called
  from a dedicated task. `ezsp_backend` runs that task.
- `ezsp_sreq` blocks the calling task on a mutex + binary semaphore;
  only one outstanding request at a time. Multiple threads serialize
  through the mutex.
- ASH ACK frames are sent inline from `ezsp_driver_poll` after a
  successful DATA decode (`uart_send` from the polling task).

## Failure modes

| Condition | Behaviour |
|-----------|-----------|
| `ezsp_ash_reset` no `RSTACK` in `timeout_ms`     | log E `RSTACK timeout`, return false |
| ASH CRC mismatch                                 | log W with expected/got, frame dropped |
| ASH NAK received                                 | log W; **retransmit not implemented** (ZB-F1) |
| ASH ERROR received                               | log E (link is hosed) |
| ASH CANCEL byte                                  | partial frame discarded, parser restarts |
| `ezsp_sreq` no reply in `timeout_ms`             | returns false |
| `ezsp_sreq` reply with mismatched seq, same cmd  | currently ignored (ZB-F2) |
| Single-buffer reply collision                    | second concurrent caller may see corrupted frame (ZB-F2) |

## EZSP vs ZNP

| Aspect            | ZNP (`znp_driver`)             | EZSP (`ezsp_driver`)              |
|-------------------|--------------------------------|------------------------------------|
| Framing           | SOF + LEN + XOR-FCS            | FLAG + escape + CRC16 + LFSR rand |
| Reliability       | per-frame SREQ/SRSP            | sliding-window ACK/NAK (incomplete) |
| Reset handshake   | GPIO pulse (`SYS_RESET_IND`)   | GPIO pulse + ASH `RST` / `RSTACK` |
| Default timeout   | 2000 ms                        | 3000 ms                            |
| Default GPIOs     | TX=16 RX=17 RST=28 BSL=29      | TX=26 RX=27 RST=28                 |
| Async dispatch    | dedicated `znp_areq_dispatch`  | `ezsp_register_callback` table     |
| Max payload       | 250 (`MT_MAX_PAYLOAD`)         | 200 (`EZSP_MAX_PAYLOAD`)           |
| Production-ready  | Yes                            | No (ZB-F1)                         |

## Integration

```cpp
#include "ezsp_driver.h"

ezsp_driver_init();
ezsp_hw_reset();
if (!ezsp_ash_reset()) { /* link dead */ }

ezsp_register_callback(EZSP_CB_STACK_STATUS,      on_stack);
ezsp_register_callback(EZSP_CB_TRUST_CENTER_JOIN, on_join);
ezsp_register_callback(EZSP_CB_INCOMING_MESSAGE,  on_zcl);

EzspFrame rsp{};
ezsp_sreq(ezsp_make_req(EZSP_VERSION), rsp);
ESP_LOGI(TAG, "EZSP version=%u", rsp.payload[0]);

// Pump in a dedicated task:
for (;;) { ezsp_driver_poll(); vTaskDelay(pdMS_TO_TICKS(5)); }
```

## Cross-references

- `components/ezsp_backend/README.md` — adapter that wires this driver into `device_backend`
- `components/znp_driver/README.md` — sibling TI transport (template for the API shape)
- `components/c6_driver/README.md` — planned third backend (Espressif ESP32-C6 ZNCP)
- `docs/FINDINGS.md` — **ZB-F1** (driver gaps), **ZB-F2** (sequence ignored, shared response frame)
