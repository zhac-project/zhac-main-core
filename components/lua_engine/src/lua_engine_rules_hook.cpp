// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// lua_engine_rules_hook.cpp — wire the simple_rules `DO lua.run "..."`
// action into the Lua scheduler. Lives in its own TU so lua_engine
// only takes the simple_rules dependency when this file is compiled.

#include "sdkconfig.h"

#if CONFIG_LUA_ENGINE_ENABLED

#include "esp_log.h"
#include "simple_rules.h"

extern "C" {
#include "lua_scheduler.h"
}

static const char* TAG = "lua_rules_hook";

static void on_script_run(const char* name, const SimpleRulesScriptEvent& ev) {
    LuaScriptInvokeArgs args{};
    args.key      = ev.key     ? ev.key     : "";
    args.value    = ev.value   ? ev.value   : "";
    args.str_val  = ev.str_val ? ev.str_val : "";
    args.ieee     = ev.ieee;
    args.cluster  = ev.cluster;
    args.attr_id  = ev.attr_id;
    args.val_type = ev.val_type;
    args.int_val  = ev.int_val;
    if (!lua_scheduler_push_run_named(name, &args)) {
        ESP_LOGW(TAG, "script.run '%s' dropped — scheduler queue full", name);
    }
}

extern "C" void lua_engine_rules_hook_install(void) {
    simple_rules_set_script_hook(on_script_run);
    ESP_LOGI(TAG, "script.run rule action hooked");
}

#else

extern "C" void lua_engine_rules_hook_install(void) {}

#endif
