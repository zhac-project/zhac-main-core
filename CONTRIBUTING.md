# Contributing to zhac-main-core

ESP32-P4 firmware — Zigbee coordinator, Lua scheduler, rule engine.

## License and CLA

Licensed under **AGPL-3.0-or-later**. All contributions require signing
`CLA.md`. See `CONTRIBUTORS.md` for signup.

## Prerequisites

- ESP-IDF v6.0 (`riscv32-esp-elf` for ESP32-P4)
- Python ≥3.10 (for pytest integration tests, optional)

## Build

```bash
idf.py set-target esp32p4
idf.py build
```

First build may take a few minutes as Component Manager fetches
`zhac-components/*` from GitHub. Subsequent builds use the local cache
in `managed_components/`.

## Flash + monitor

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

## SPDX headers for new files

```c
// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
```

Kconfig / CMakeLists / Python / YAML use `#` comment syntax.
Lua uses `--`.

## Style

- C++17 (no exceptions, no RTTI).
- 4-space indent, `snake_case` vars/funcs, `UPPER_CASE` macros.
- No `std::string`, `std::vector` in hot paths — heap pressure.
- Prefer `snprintf` over concat.

## Adding a new HAP message type

1. Add the opcode to `hap_protocol` (in `zhac-components`).
2. Add encode/decode to `hap_json` (same repo).
3. Add the dispatcher entry in `main/hap_dispatch.cpp` here.
4. Document the wire format in the platform `docs/WS_API.md` if it
   has a public surface.

## Running tests

Unit tests live in `tests/` (host-side, run on your workstation):

```bash
pytest tests/ -v
```

Integration tests require hardware and live in the platform repo.

## Reporting bugs

Open an issue with:
- Firmware version (`zhac status` → `p4.fw`)
- Reset reason from boot log
- Minimal reproduction
- Zigbee backend in use (EZSP vs ZNP)
