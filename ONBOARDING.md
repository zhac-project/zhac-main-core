# ONBOARDING — zhac-main-core (ESP32-P4 firmware)

You are an AI agent arriving on **zhac-main-core**, the P4-side
firmware for ZHAC. This repo owns the Zigbee coordinator and the P4
half of the HAP protocol. Read top-to-bottom before coding.

---

## 1. Platform context

**ZHAC** = dual-chip ESP32 Zigbee Home Automation Controller.

- **ESP32-P4** runs this firmware — Zigbee coordinator, Lua engine.
- **ESP32-S3** runs `zhac-net-core` — WiFi / REST / WS / MQTT.
- They talk over SPI using the custom **HAP** binary protocol. S3 is
  the host; P4 is the slave.
- A Preact SPA (`www-spa`) is bundled into S3 SPIFFS.

Data flow (P4 perspective):

```
Zigbee device ──► EZSP/ZNP driver ──► zigbee_backend
                                         │
                                         ▼
                                  zhc_adapter_try_decode
                                         │
                                         ▼
                                    device_shadow
                                         │
                                         ▼
                              hap_slave (P4 side of HAP)
                                         │
                                         ▼ SPI
                                 S3 hap_master ──► WS / MQTT
```

Downstream (user action):

```
WS / REST ──► S3 api_handlers ──► hap_master ──► SPI
                                                  │
                                                  ▼
                                          P4 hap_slave
                                                  │
                                                  ▼
                                          hap_dispatch  ← main/hap_dispatch.cpp
                                                  │
                                                  ▼
                             zhc_adapter_send_{bool,uint,string}
                                                  │
                                                  ▼
                                          zigbee_backend
                                                  │
                                                  ▼
                                          EZSP/ZNP ──► Zigbee device
```

### Repo split

Tag `v2026042301` (2026-04-23) baseline. 7 repos:
`zhac-platform`, `embedded-zhc`, `zhac-components`,
**`zhac-main-core`** *(this)*, `zhac-net-core`, `www-spa`, `zhac-docs`.

---

## 2. What this repo owns

- `main/` — entry point, HAP dispatch, P4 OTA.
- `components/` — P4-only components (Zigbee stack, Lua, HAP slave).
- `partitions.csv` · `sdkconfig.defaults` · `CMakeLists.txt`.

### Layout

```
zhac-main-core/
├── main/
│   ├── main.cpp                 (app_main, boot order)
│   ├── hap_dispatch.cpp         (HAP command handlers — big file)
│   ├── p4_ota.{h,cpp}           (OTA over SPI from S3)
│   ├── idf_component.yml        (9 dep components + esp-idf lua)
│   ├── CMakeLists.txt
│   └── Kconfig.projbuild
├── components/
│   ├── ezsp_driver/             (EmberZNet Serial Protocol driver)
│   ├── ezsp_backend/            (zigbee_backend impl over EZSP)
│   ├── znp_driver/              (TI Z-Stack Monitor driver)
│   ├── zigbee_backend/          (backend-agnostic dispatch)
│   ├── zigbee_mgr/              (pool, identity, configure-queue, zcl_seq)
│   ├── hap_slave/               (P4 side of HAP SPI transport)
│   ├── lua_engine/              (Lua rules VM)
│   └── lua_cjson/               (vendored Lua cJSON)
├── partitions.csv · sdkconfig.defaults
├── CMakeLists.txt
├── LICENSE · NOTICE · CLA.md · CONTRIBUTORS.md · CONTRIBUTING.md
└── LICENSES/                    (AGPL-3.0-or-later.txt · Apache-2.0.txt)
```

### Shared deps (from zhac-components)

Pulled via `main/idf_component.yml`:
`zap_common`, `zap_store`, `event_bus`, `device_shadow`,
`device_backend`, `zhc_adapter`, `hap_protocol`, `hap_session`,
`hap_json`, `metrics`. Plus `georgik/lua` from the ESP registry.

### Library dep

`embedded-zhc` is pulled via CMake `FetchContent` at a pinned tag
(**not** Component Manager — it has its own CMake tree). Override
with `EMBEDDED_ZHC_PATH` for local dev.

---

## 3. Building

```bash
cd zhac-main-core
idf.py set-target esp32p4
idf.py build
```

With local sibling checkouts:

```bash
export ZHAC_PATH=${ZHAC_PATH:-$HOME/zhac}   # dir holding the cloned ZHAC repos
export IDF_COMPONENT_OVERRIDE_PATH=$ZHAC_PATH/zhac-components/components
export EMBEDDED_ZHC_PATH=$ZHAC_PATH/embedded-zhc
idf.py build
```

**Do not run `idf.py build` for the user.** They build firmware
themselves. Your job stops at code changes.

Flash + monitor is theirs too (`idf.py -p /dev/ttyUSB0 flash monitor`
on ESP32-P4 at 460800 or 921600 baud).

---

## 4. Key files / symbols

### `main/hap_dispatch.cpp` — the P4-side command surface

Every HAP command S3 can issue lands here. Large file — use Read with
an offset or grep rather than reading it whole. Handlers of note:

- `handle_device_add` / `handle_device_delete` — pair / remove.
  `handle_device_delete` now includes a **hard-remove path** that
  calls `device_shadow_clear_attrs` + `zhac_adapter_invalidate_def_cache`.
- `handle_set_attribute` — write path. **Performs an optimistic
  shadow update** after a successful ZCL send, so the UI reflects the
  toggle before the device's next attribute report arrives (critical
  for Tuya LED drivers that stay silent on command-driven changes).

Optimistic shadow snippet (keep this pattern):

```cpp
if (ok && attr.key[0] != '\0') {
    const char* k = attr.key;
    uint8_t vt = (strcmp(k, "state") == 0) ? VAL_BOOL : VAL_INT;
    device_shadow_update_optimistic(attr.ieee, k, vt,
                                    static_cast<int32_t>(attr.val));
}
```

### `components/zigbee_mgr/`

Owns the device pool, identity resolution, the configure-queue (runs
`zhac_adapter_configure` off the pairing IRQ), and ZCL sequence
numbering.

### `components/lua_engine/`

Lua 5.5 VM for rules. API documented in `zhac-docs/LUA_API.md`.
zhac-specific bindings in `src/zhac_lua_module.cpp`. Don't block the
Lua task — rules run on the event loop.

### `components/ezsp_driver/` vs `znp_driver/`

Two radio back-ends. Pick via Kconfig. EZSP covers most Silicon Labs
radios; ZNP covers TI CC2xxx. Both implement the same
`zigbee_backend` interface.

---

## 5. Cross-repo contracts

### HAP (S3↔P4 wire format)

- Binary envelope defined in
  `zhac-components/components/hap_protocol/`.
- P4 side: `components/hap_slave/`. S3 side: `hap_master` in
  `zhac-net-core`.
- Every P4-originated event (attr report, device join, alert) goes
  through the same SPI channel.

### `ZclAttribute` (52 B)

Canonical in `zap_common`. **Don't define a parallel struct.**
Everything — adapter, shadow, hap_json, Lua bindings — uses this
type.

### Sizes to respect

`ZclAttribute` = 52 B, `ZclAttrEvent` = 96 B, `ShadowAttr` = 52 B,
`ZapDevice` = 522 B. Changes require `NVS_SHADOW_VERSION` bump
(currently v4).

---

## 6. Conventions

- **C++17** (`-fno-exceptions -fno-rtti`).
- **String keys ≤ 20 chars, string values ≤ 24 chars** — attribute
  names come from the `zhc` library's exposes; no hand-maintained
  namespace.
- **Use `zhc_adapter` exclusively** when touching device attributes.
  Direct `zhc::` includes in firmware are forbidden.
- **Don't block hot paths.** `hap_dispatch` runs on the HAP task; any
  long-running work (NVS writes, ZCL bursts) goes on a worker.
- **No logging in ISR context.** The S3 freeze bug was root-caused to
  unbounded work + logging inside the log pipeline itself.
- **NVS schema versioning.** If you change `ShadowAttr` or `ZapDevice`
  layouts, bump `NVS_SHADOW_VERSION` and ship a wipe-on-mismatch
  migration.

---

## 7. User preferences (persistent)

- **User builds firmware themselves.** Don't run `idf.py build`.
- **Early-dev stance.** Accept breaking changes — no migration
  shims, no dual-code-paths.
- **Prefer hook/callback registration** when two components would
  otherwise need each other (canonical example:
  `zhac_adapter_register_shadow_hook` instead of `zhc_adapter`
  including `device_shadow.h` directly).

---

## 8. Gotchas

- **Definition cache is sticky.** If a device is removed and
  re-paired, call `zhac_adapter_invalidate_def_cache(ieee)` — the
  cache is keyed by IEEE, not model+manuf, and a stale entry will
  route traffic to the old definition.
- **Tuya LED silence.** Mains-powered Tuya LED drivers often don't
  send reports for command-driven changes. Every light definition
  must include `ReportingSpec` + `ConfigStep` initial-read sequence,
  **and** the P4 should emit an optimistic shadow update on
  successful write.
- **TZ type union.** UI sends `VAL_UINT` through
  `zhac_adapter_send_uint`; TZ converters that only accept
  `VAL_BOOL` / `VAL_STR` will reject valid input. When adding a TZ
  converter, accept the full union.
- **Configure-queue ordering.** `zhac_adapter_configure` must run
  **after** the device appears in `device_backend` and the bindings
  have been flushed — otherwise `config_steps` reads fire before the
  bind replies and get dropped. `zigbee_configure_queue.cpp`
  sequences this; don't bypass it.
- **Wi-Fi credentials reboot.** S3-side `api_wifi_connect` now uses
  a deferred `esp_timer_start_once(_, 1000000)` because blocking
  `esp_restart()` inside the handler raced the WS reply. Don't
  reinstate the blocking version.

---

## 9. Licensing

- **AGPL-3.0-or-later** for this repo.
- Every file starts with:
  ```
  // SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
  // SPDX-License-Identifier: AGPL-3.0-or-later
  ```
- Vendored `lua_cjson` carries its own MIT header — don't relicense.
- `LICENSE` at repo root points to `LICENSES/AGPL-3.0-or-later.txt`.
- CLA: Apache ICLA v2.2 + §4 relicensing grant. Sign by adding
  yourself to `CONTRIBUTORS.md` in your first PR (covers all 7 ZHAC
  repos).

Note: the CLA allows the maintainer to dual-license your
contribution under Apache-2.0 later — that's intentional.

---

## 10. Where to go next

- **HAP message reference**: `zhac-docs/` (check for `HAP_*.md`)
  and `zhac-components/components/hap_protocol/README.md`.
- **Lua API**: `zhac-docs/LUA_API.md`.
- **Rules DSL**: `zhac-docs/RULES_DSL.md` (rules run on S3, but Lua
  rules run on P4 — know the difference).
- **Device porting**: `zhac-docs/PORTING_DEVICES.md` +
  `embedded-zhc/` definitions.
- **P4 GPIO allocation**: `zhac-docs/esp32_p4_gpio_allocation.md`.

---

*Tag on first split: `v2026042301` · 2026-04-23.*
*License: AGPL-3.0-or-later · Maintainer: Evgenij Cjura.*
