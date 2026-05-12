// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// lua_engine_event_bridge.cpp — EventBus subscription + Lua dispatch.
//
// Lives as a separate .cpp TU because event_bus.h pulls in C++ headers
// (<cstdint>, <functional>). Exposes `lua_engine_event_bridge_start()`
// (C-linkage) so the engine init path can opt in regardless of whether
// the rest of the engine stays pure C.

#include "sdkconfig.h"

#if CONFIG_LUA_ENGINE_ENABLED

#include <cstring>

#include "esp_log.h"
#include "event_bus.h"

extern "C" {
#include "lua_scheduler.h"
}

static const char* TAG = "lua_evt_bridge";

namespace {

void on_attr_change(const Event& e) {
    // ZclAttrEvent already matches the 96-byte payload exactly.
    lua_scheduler_push_event(/*LUA_EVT_ATTR=*/1, e.data);
}

void on_mqtt_msg(const Event& e) {
    lua_scheduler_push_event(/*LUA_EVT_MQTT=*/2, e.data);
}

void on_ctrl_boot(const Event& e) {
    (void)e;
    lua_scheduler_push_event(/*LUA_EVT_BOOT=*/0, nullptr);
}

void on_zcl_raw(const Event& e) {
    // ZclRawEvent already matches the 96-byte payload exactly.
    lua_scheduler_push_event(/*LUA_EVT_RAW=*/3, e.data);
}

}  // namespace

extern "C" void lua_engine_event_bridge_start(void) {
    event_bus_subscribe(EventType::ATTR_CHANGE, on_attr_change);
    event_bus_subscribe(EventType::MQTT_MSG,    on_mqtt_msg);
    event_bus_subscribe(EventType::CTRL_BOOT,   on_ctrl_boot);
    event_bus_subscribe(EventType::ZCL_RAW,     on_zcl_raw);
    ESP_LOGI(TAG, "event bridge installed (attr / mqtt / boot / raw)");
}

#else

extern "C" void lua_engine_event_bridge_start(void) {}

#endif
