# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
set(MAIN_REQUIRES
    hap_protocol
    zap_common
    hap_slave
    hap_session
    hap_json
    event_bus
    zigbee_mgr
    device_shadow
    zap_store
    lua_engine
    esp_timer
    freertos
    simple_rules
    mqtt_gw
    tg_gw
    metrics
    nvs_flash
    spiffs
    app_update
    device_backend
    zhc_adapter
    # Backend requires are unconditional ON PURPOSE. ESP-IDF expands
    # REQUIRES in an early script-mode pass BEFORE sdkconfig is generated
    # (build-system docs: "requirements are expanded before the
    # configuration is loaded"), so an `if(CONFIG_ZHAC_NCP_EZSP)` here
    # always took the else-branch on a fresh build dir and EZSP-enabled
    # configs failed with `ezsp_backend.h: No such file or directory`.
    # Safe to list all three always: main.cpp picks the backend via
    # preprocessor, and the ezsp TUs are whole-TU-gated on
    # CONFIG_ZHAC_EZSP_BACKEND_ENABLE (empty objects in ZNP builds).
    zigbee_backend
    ezsp_backend
    ezsp_driver
)

idf_component_register(
    SRCS "main.cpp" "hap_dispatch.cpp" "p4_ota.cpp" "test_hap_throughput.cpp"
    INCLUDE_DIRS "."
    REQUIRES ${MAIN_REQUIRES}
)

# F17 (FINDINGS.md): warnings-as-errors for first-party app code. The clean
# build was warning-free here at IDF's default level; -Werror keeps it so.
# (embedded-zhc already enforces this; extending -Werror to the remaining
# first-party components is a mechanical follow-up — backlog is ~0.)
# -Wno-error=cpp: the F14 `#warning` (EZSP backend is experimental) in
# main.cpp must stay a loud warning in EZSP configs, not a build breaker.
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror -Wno-error=cpp)
