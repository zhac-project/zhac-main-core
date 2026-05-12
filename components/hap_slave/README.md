# hap_slave — P4-side SPI Slave for HAP

The ESP32-P4 half of the SPI link to the ESP32-S3. Runs a single dedicated
FreeRTOS task that keeps an SPI slave transaction permanently armed (the
"always-armed" design) so the S3 master can clock data in or out at any
instant. Outbound frames are produced from any P4 task by `hap_slave_send()`;
the slave task picks them up on its next loop iteration and asserts DRDY to
wake the S3.

This component is **P4-only**. The CMakeLists.txt early-returns an empty
component on `esp32s3` to avoid pulling P4 GPIOs (notably GPIO22) into the
S3 build. The S3 counterpart is `hap_master`.

## Where it sits

```
[any P4 task: hap_dispatch, mqtt_gw, log_ring, simple_rules, ...]
        │
        ▼
hap_slave_send(frame)         ◀── this component (producer side)
        │
        │ heap-encode + xQueueSend → s_tx_q
        ▼
[hap_slave task — pinned, prio 7]
        │ on_loop:
        │   1. dequeue PendingTx from s_tx_q (if any)
        │   2. memcpy into s_tx_buf, set DRDY=1
        │   3. spi_slave_queue_trans (HAP_DMA_BUF_SIZE bytes)
        │   4. spi_slave_get_trans_result (BLOCKING, portMAX_DELAY)
        │   5. DRDY=0; decode RX; dispatch via callback
        ▼
[hap_session_on_receive]  →  cfg.on_frame  →  hap_dispatch
```

Always-armed means the slave is in `get_trans_result(portMAX_DELAY)` with a
queued transaction ~100 % of the time. There is **no window** where the
master can clock and find the slave unprepared. The previous (pre-rewrite)
design only armed the slave for outbound TX, which lost S3→P4 commands.
See the inline `hap_slave.cpp:5-29` design comment.

## Dependencies (CMakeLists.txt)

`REQUIRES hap_protocol esp_driver_spi esp_driver_gpio freertos`. Built only
when `IDF_TARGET != esp32s3` (i.e. esp32p4).

## Hardware

### Pin assignment (P4 slave)

| Signal | GPIO   | Direction | Notes                                                       |
|--------|--------|-----------|-------------------------------------------------------------|
| MOSI   | GPIO19 | S3 → P4   | `hap_slave.cpp:49` (`PIN_MOSI`)                              |
| MISO   | GPIO20 | P4 → S3   | `hap_slave.cpp:50` (`PIN_MISO`)                              |
| SCLK   | GPIO18 | S3 → P4   | `hap_slave.cpp:48` (`PIN_SCLK`), input on slave              |
| CS     | GPIO21 | S3 → P4   | `hap_slave.cpp:51` (`PIN_CS`), input on slave                |
| DRDY   | GPIO22 | P4 → S3   | `hap_slave.cpp:52` (`PIN_DRDY`), output, wired to S3 GPIO8    |

The S3 master uses a different GPIO bank (GPIO11/13/12/10/8 — see
`components/hap_master/README.md`); the cross-board jumper map is what makes
the link work.

### SPI configuration

| Setting     | Value                            |
|-------------|----------------------------------|
| Bus         | `SPI2_HOST` (slave mode)         |
| Mode        | 0 (CPOL=0, CPHA=0; clock from S3)|
| DMA channel | auto                             |
| TX buffer   | DMA-capable, internal SRAM       |
| RX buffer   | DMA-capable, internal SRAM       |
| Buffer size | `HAP_DMA_BUF_SIZE` (frame max)   |

`HAP_DMA_BUF_SIZE` matches `HAP_MAX_FRAME_SIZE` (4110 B) so a single SPI
transaction is exactly one max frame's worth of bytes in each direction.

## Important constants

| Constant            | Value             | Source                     |
|---------------------|-------------------|----------------------------|
| `HAP_DMA_BUF_SIZE`  | `HAP_MAX_FRAME_SIZE` (4110)  | `hap_slave.cpp:55`         |
| `TX_QUEUE_LEN`      | 32 outbound slots | `hap_slave.cpp:69`         |
| Slave task stack    | 4096 B            | `xTaskCreatePinnedToCore`  |
| Slave task priority | 7                 | same                       |

`s_tx_q` is `xQueueCreate(TX_QUEUE_LEN, sizeof(PendingTx))`. Each
`PendingTx` holds a heap-allocated encoded blob and its length; the
producer is fire-and-forget. If the queue is full, the new send is dropped
with a warning (see error table below).

## Public API (`include/hap_slave.h`)

```cpp
using HapFrameCallback = std::function<void(const HapFrame&)>;

// One-time init: claim SPI2 in slave mode, allocate DMA buffers, configure
// DRDY output (held low at boot), create s_tx_q, spawn the hap_slave task
// pinned. Logs:
//   I hap_slave init OK — always-armed, DRDY GPIO22
void hap_slave_init();

// Encode `frame` into a freshly malloc'd blob, push onto s_tx_q, return.
// Producer-side fire-and-forget; never blocks (queue is bounded). The
// slave task copies the blob into the DMA buffer and asserts DRDY on its
// next loop iteration. Safe to call from any task (and from hap_session_send
// as the HapSendFn).
void hap_slave_send(const HapFrame& frame);

// Install the RX dispatcher. Typically:
//   hap_slave_set_callback([](const HapFrame& f){ hap_session_on_receive(f); });
// Called from the slave task's context, after it decodes a valid frame.
void hap_slave_set_callback(HapFrameCallback cb);
```

## Threading and concurrency

| Resource                | Owner                                                |
|-------------------------|------------------------------------------------------|
| `s_tx_buf` / `s_rx_buf` | **hap_slave task only** — never touched by producers |
| `spi_slave_queue_trans` / `get_trans_result` | hap_slave task only |
| `s_tx_q`                | producer-side: fire-and-forget; consumer: hap_slave task |
| `gpio_set_level(PIN_DRDY)` | hap_slave task only                                |
| Callback dispatch        | hap_slave task context (caller usually re-enters via hap_session_on_receive) |

Producers (`hap_slave_send`) are non-blocking. The DMA buffers and SPI
driver are touched by exactly one task — no mutex needed. This is the
discipline that lets us call `get_trans_result(..., portMAX_DELAY)` and
trust the always-armed property.

## Error and failure modes

| Log line                                          | Meaning                                                  |
|---------------------------------------------------|----------------------------------------------------------|
| `W TX drop: malloc failed type=0xXX seq=N`        | `hap_slave_send` couldn't allocate the blob. Frame dropped. |
| `E hap_encode overflow type=0xXX`                 | Caller frame > `HAP_DMA_BUF_SIZE`. Frame dropped.        |
| `E queue_trans: ESP_ERR_…`                        | SPI driver wouldn't accept a transaction. Loop sleeps 10 ms and retries. |
| `E get_trans_result: ESP_ERR_…`                   | Driver-level fault. Loop continues; DRDY pulled low.     |
| `W RX copy malloc fail`                           | Decoded frame, but couldn't allocate the callback's private payload buffer. Frame dropped. |
| `W RX CRC mismatch (signal integrity?)`           | One frame lost. `hap_decode_stream` will resync next iteration. |
| `W RX bad magic 0xXXYY`                           | RX half was zeros / garbage — usually means the master clocked a transaction without sending (DRDY-only read). Not an error. |
| `E RX HAP_VERSION mismatch …`                     | Mixed-firmware S3 / P4.                                  |

## Integration example

Wired alongside `hap_session`. Producers call `hap_session_send`, which
delegates to `hap_slave_send` via the configured `HapSendFn`.

```cpp
#include "hap_slave.h"
#include "hap_session.h"

static void on_frame(const HapFrame& f) {
    // Dispatch by f.type — see firmware/zhac-main-core/main/hap_dispatch.cpp.
}

void hap_init() {
    hap_slave_init();
    hap_slave_set_callback([](const HapFrame& f) {
        hap_session_on_receive(f);
    });

    HapSessionCfg cfg{};
    cfg.send         = [](const HapFrame& f) { hap_slave_send(f); };
    cfg.on_frame     = on_frame;
    cfg.on_sync      = [](const HapFrame&) { /* S3 just rebooted */ };
    cfg.on_link_dead = []() { ESP_LOGE("hap", "S3 link dead"); };
    hap_session_init(cfg);
}

// task_hap loop body — only the tick is needed; RX is driven by the
// hap_slave task.
for (;;) {
    hap_session_tick();
    vTaskDelay(pdMS_TO_TICKS(10));
}
```

## Cross-references

- `components/hap_master/README.md` — S3-side counterpart and pin map
- `components/hap_session/README.md` — sliding-window layer above this driver
- `components/hap_protocol/README.md` — wire format
- `firmware/zhac-main-core/main/hap_dispatch.cpp` — primary consumer (`task_hap`,
  per-type handlers, `on_zcl_attr_for_hap` emits `BULK_STATE_UPDATE`
  per-attr live; the batched `flush_bulk` path was retired 2026-04-25
  — CC-F6 in `docs/FINDINGS.md`)
- `components/mqtt_gw/README.md` — uses `hap_slave_send` indirectly via
  `hap_session_send` to forward MQTT publishes from P4 to S3

## Recent changes

- "Always-armed" task model replaced the previous TX-triggered slave —
  fixed silent loss of S3→P4 commands when no outbound TX was queued.
- `hap_slave_send` is now strictly fire-and-forget (returns void). The
  earlier signature returned `bool` and a 500 ms timeout; both have been
  retired now that the producer no longer touches the SPI driver.
- DRDY default level is 0 at init; the slave task pulses it to 1 only when
  a real outbound frame is in `s_tx_buf`.
