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
)

if(CONFIG_ZHAC_NCP_EZSP)
    list(APPEND MAIN_REQUIRES ezsp_backend ezsp_driver)
else()
    list(APPEND MAIN_REQUIRES zigbee_backend)
endif()

idf_component_register(
    SRCS "main.cpp" "hap_dispatch.cpp" "p4_ota.cpp" "test_hap_throughput.cpp"
    INCLUDE_DIRS "."
    REQUIRES ${MAIN_REQUIRES}
)

# F17 (FINDINGS.md): warnings-as-errors for first-party app code. The clean
# build was warning-free here at IDF's default level; -Werror keeps it so.
# (embedded-zhc already enforces this; extending -Werror to the remaining
# first-party components is a mechanical follow-up — backlog is ~0.)
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
