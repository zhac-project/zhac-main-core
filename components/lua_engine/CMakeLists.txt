# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
if(IDF_TARGET STREQUAL "esp32p4")
    idf_component_register(
        SRCS "src/lua_alloc.c"
             "src/lua_engine.c"
             "src/lua_scheduler.cpp"
             "src/lua_sandbox.c"
             "src/lua_require.c"
             "src/zhac_lua_module.cpp"
             "src/lua_engine_event_bridge.cpp"
             "src/lua_engine_rules_hook.cpp"
             "src/lua_script_cache.cpp"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "src"
        REQUIRES freertos esp_timer lua
        PRIV_REQUIRES spiffs event_bus metrics
                      zap_common zap_store zhc_adapter
                      device_shadow mqtt_gw tg_gw zigbee_mgr
                      simple_rules lua_cjson
    )
else()
    # S3 + any other future target: register an empty component so
    # auto-discovery doesn't try to resolve the `lua` dep (only P4's
    # idf_component.yml carries it). Keeps the shared `components/`
    # layout simple — no per-target SRC lists anywhere upstream.
    idf_component_register()
endif()
