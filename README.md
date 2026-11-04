# zhac-main-core

> **Deprecated (2026-11-04).** The two-chip build (this ESP32-P4 firmware
> plus the ESP32-S3 `zhac-net-core`, joined by HAP over SPI) is no longer
> developed, built or tested, and never shipped. The ZHAC hub is
> [zhac-wired-core]; it gets Wi-Fi from an ESP32-C5 co-processor through
> ESP-Hosted. Do not build `main/` or `components/hap_slave`.
>
> The repository stays because other firmware builds from it:
> zhac-wired-core and zhac-mono-core compile the Lua engine
> (`components/lua_engine`, `components/lua_cjson`), and zhac-mono-core
> also uses `components/zigbee_backend`. Changes to those components are
> still made here and must keep those builds green.

[zhac-wired-core]: https://github.com/zhac-project/zhac-wired-core

ESP32-P4 firmware for [ZHAC]. Runs the Zigbee coordinator, Lua script
engine, and rule scheduler. Communicates with the S3 network core
over SPI via the custom HAP binary protocol.

[ZHAC]: https://github.com/zhac-project/zhac-docs

## Responsibilities

- Zigbee stack (Z-Stack / EZSP — selectable at build time)
- Device interview + identity + diagnostics
- Lua scheduler (callbacks on attribute change, cron triggers, rules)
- HAP slave endpoint (responds to commands from S3)

## Tree

```
zhac-main-core/
├── main/                (firmware entry, Kconfig, idf_component.yml)
├── components/
│   ├── hap_slave/
│   ├── lua_engine/
│   ├── lua_cjson/       (vendored MIT)
│   ├── zigbee_mgr/
│   ├── zigbee_backend/
│   ├── ezsp_driver/
│   ├── ezsp_backend/
│   └── znp_driver/
├── CMakeLists.txt
└── sdkconfig.defaults
```

Shared components (HAP protocol, device shadow, ZHC adapter, ...)
are pulled at build time from [zhac-components]; the ZHC library
from [embedded-zhc].

[zhac-components]: https://github.com/zhac-project/zhac-components
[embedded-zhc]:    https://github.com/zhac-project/embedded-zhc

## Building standalone

```bash
git clone https://github.com/zhac-project/zhac-main-core.git
cd zhac-main-core
source /path/to/esp-idf-v6.0/export.sh
idf.py set-target esp32p4
idf.py build
```

Component Manager fetches `zhac-components/*` from GitHub on first
build. For local development, override with:

```bash
git clone https://github.com/zhac-project/zhac-components.git ../zhac-components
git clone https://github.com/zhac-project/embedded-zhc.git   ../embedded-zhc

export IDF_COMPONENT_OVERRIDE_PATH=$PWD/../zhac-components/components
export EMBEDDED_ZHC_PATH=$PWD/../embedded-zhc
idf.py build
```

With the sibling layout above, CMake also resolves
`../zhac-components/components` without `IDF_COMPONENT_OVERRIDE_PATH` — the
export just makes the override explicit.

## Flash

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

## License

GNU AGPL v3 or later. See `LICENSE`.

## Contributing

See `CONTRIBUTING.md`. All contributions require signing `CLA.md`.

## Versioning

Releases tagged `vYYYYMMDDVV` (UTC date + 2-digit revision). Each repo
tags its own version independently.
