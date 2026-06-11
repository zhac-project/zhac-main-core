// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// zhac_lua_module.cpp — native Lua bindings for ZHAC.
//
// `zhac` module — the canonical scripting surface for ZHAC.
// Registrations (`on_attr_change`, `on_cron`, `on_mqtt`, `on_boot`)
// stash Lua function refs in the registry; the scheduler's event
// dispatcher (lua_scheduler.cpp) iterates them when EventBus frames
// arrive.

#include "sdkconfig.h"

#if CONFIG_LUA_ENGINE_ENABLED

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

#include "device_shadow.h"
#include "event_bus.h"
#include "mqtt_gw.h"
#include "tg_gw.h"
#include "zap_common.h"
#include "zap_store.h"
#include "zhc_adapter.h"
#include "zigbee_pool.h"

extern "C" {
#include "lua_internal.h"
#include "lua_scheduler.h"
}

static const char* TAG = "zhac_lua";

// Registry keys for the on_* handler tables. The scheduler reads the
// same keys by name — keep in sync with lua_scheduler.cpp::reg_key_for.
#define REG_ON_ATTR   "zhac_on_attr_refs"
#define REG_ON_CRON   "zhac_on_cron_refs"
#define REG_ON_MQTT   "zhac_on_mqtt_refs"
#define REG_ON_BOOT   "zhac_on_boot_refs"
#define REG_ON_RAW    "zhac_on_raw_refs"

namespace {

// Parse "0x…" / decimal / colon-separated 16-hex-char IEEE → uint64.
uint64_t parse_ieee_hex(const char* s) {
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint64_t v = 0;
    for (; *s; ++s) {
        char c = *s;
        if (c == ':' || c == '-') continue;
        if (c >= '0' && c <= '9')      v = (v << 4) | (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (uint64_t)(c - 'A' + 10);
        else return 0;
    }
    return v;
}

int register_handler(lua_State* L, const char* reg_key) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, reg_key);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, reg_key);
    }
    const lua_Integer len = luaL_len(L, -1);
    lua_pushvalue(L, 1);
    lua_seti(L, -2, len + 1);
    lua_pop(L, 1);
    return 0;
}

}  // namespace

// ── zhac.log(level_or_msg [, msg]) ────────────────────────────────────
static int l_zhac_log(lua_State* L) {
    const int top = lua_gettop(L);
    const char* msg;
    char level = 'I';
    if (top >= 2) {
        level = luaL_checkstring(L, 1)[0];
        msg   = luaL_checkstring(L, 2);
    } else {
        msg = luaL_checkstring(L, 1);
    }
    switch (level) {
        case 'e': case 'E': ESP_LOGE("lua_script", "%s", msg); break;
        case 'w': case 'W': ESP_LOGW("lua_script", "%s", msg); break;
        case 'd': case 'D': ESP_LOGD("lua_script", "%s", msg); break;
        default:            ESP_LOGI("lua_script", "%s", msg); break;
    }
    return 0;
}

// ── zhac.millis() → integer ───────────────────────────────────────────
static int l_zhac_millis(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000LL));
    return 1;
}

// ── zhac.sleep(ms) — yield until the timer fires. Coroutine-only. ────
static int l_zhac_sleep(lua_State* L) {
    const lua_Integer delay = luaL_checkinteger(L, 1);
    if (delay < 0) return luaL_error(L, "zhac.sleep: negative delay");

    if (lua_pushthread(L) == 1) {
        lua_pop(L, 1);
        return luaL_error(L, "zhac.sleep must be called from a coroutine");
    }
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (ref == LUA_NOREF) return luaL_error(L, "zhac.sleep: out of registry");

    if (!lua_scheduler_sleep(ref, (uint32_t)delay)) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "zhac.sleep: timer pool exhausted");
    }
    return lua_yield(L, 0);
}

// ── zhac.set_attr(ieee_hex, key, value) → bool ────────────────────────
// Value type dispatches: bool → send_bool, integer → send_uint,
// string → send_string.
static int l_zhac_set_attr(lua_State* L) {
    const char* ieee_s = luaL_checkstring(L, 1);
    const char* key    = luaL_checkstring(L, 2);
    const uint64_t ieee = parse_ieee_hex(ieee_s);
    if (!ieee) {
        ESP_LOGW(TAG, "set_attr: bad ieee '%s'", ieee_s);
        lua_pushboolean(L, 0);
        return 1;
    }
    // F6/F35 (FINDINGS.md): snapshot under the pool lock; the blocking
    // radio sends below run on the detached copy — never on a raw pool
    // pointer that a swap-with-last remove could retarget.
    ZapDevice snap;
    if (!zigbee_pool_snapshot(ieee, &snap)) {
        ESP_LOGW(TAG, "set_attr: device 0x%016" PRIx64 " not in pool", ieee);
        lua_pushboolean(L, 0);
        return 1;
    }
    const uint8_t ep = snap.endpoints[0] ? snap.endpoints[0] : 1;

    bool ok = false;
    if (lua_isboolean(L, 3)) {
        ok = zhac_adapter_send_bool(snap.ieee_addr, snap.model_id,
                                      snap.manufacturer_name,
                                      snap.nwk_addr, ep, key,
                                      lua_toboolean(L, 3) != 0);
    } else if (lua_isinteger(L, 3)) {
        ok = zhac_adapter_send_uint(snap.ieee_addr, snap.model_id,
                                      snap.manufacturer_name,
                                      snap.nwk_addr, ep, key,
                                      (uint64_t)lua_tointeger(L, 3));
    } else if (lua_isstring(L, 3)) {
        ok = zhac_adapter_send_string(snap.ieee_addr, snap.model_id,
                                        snap.manufacturer_name,
                                        snap.nwk_addr, ep, key,
                                        lua_tostring(L, 3));
    } else {
        return luaL_error(L,
            "zhac.set_attr: value must be bool/integer/string");
    }
    lua_pushboolean(L, ok);
    return 1;
}

// ── zhac.get_attr(ieee_hex, key) → int|string|bool|nil ───────────────
static int l_zhac_get_attr(lua_State* L) {
    const char* ieee_s = luaL_checkstring(L, 1);
    const char* key    = luaL_checkstring(L, 2);
    const uint64_t ieee = parse_ieee_hex(ieee_s);
    if (!ieee) { lua_pushnil(L); return 1; }

    ShadowAttr attrs[32];
    const uint8_t n = device_shadow_get_attrs(ieee, attrs, 32);
    for (uint8_t i = 0; i < n; ++i) {
        if (std::strcmp(attrs[i].key, key) == 0) {
            switch (attrs[i].val_type) {
                case VAL_BOOL:
                    lua_pushboolean(L, attrs[i].int_val != 0);
                    return 1;
                case VAL_STR:
                    lua_pushstring(L, attrs[i].str_val);
                    return 1;
                default:
                    lua_pushinteger(L, (lua_Integer)attrs[i].int_val);
                    return 1;
            }
        }
    }
    lua_pushnil(L);
    return 1;
}

// ── zhac.publish(topic, payload [, qos [, retain]]) ──────────────────
static int l_zhac_publish(lua_State* L) {
    const char* topic        = luaL_checkstring(L, 1);
    size_t      payload_len  = 0;
    const char* payload      = luaL_checklstring(L, 2, &payload_len);
    const int   qos          = (int)luaL_optinteger(L, 3, 0);
    const bool  retain       = lua_toboolean(L, 4) != 0;
    mqtt_gw_publish(topic, payload, payload_len, qos, retain);
    return 0;
}

// ── zhac.event(name) — fire a named RULE_EVENT ───────────────────────
static int l_zhac_event(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    Event ev{};
    ev.type = EventType::RULE_EVENT;
    auto* p = reinterpret_cast<RuleEventPayload*>(ev.data);
    std::strncpy(p->name, name, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';
    event_bus_publish(ev);
    return 0;
}

// ── zhac.telegram_settoken(token) ────────────────────────────────────
static int l_zhac_telegram_settoken(lua_State* L) {
    const char* token = luaL_checkstring(L, 1);
    bool ok = tg_gw_settoken(token);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ── zhac.telegram_setchat(chat_id) ───────────────────────────────────
static int l_zhac_telegram_setchat(lua_State* L) {
    const char* chat = luaL_checkstring(L, 1);
    bool ok = tg_gw_setchat(chat);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ── zhac.telegram_send(text [, chat [, parse_mode]]) ───────────────
static int l_zhac_telegram_send(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    const char* chat = lua_isnoneornil(L, 2) ? nullptr : luaL_checkstring(L, 2);
    const char* pm   = lua_isnoneornil(L, 3) ? nullptr : luaL_checkstring(L, 3);
    bool ok = tg_gw_send(text, chat, pm);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ── on_* registrations ───────────────────────────────────────────────
static int l_zhac_on_attr_change(lua_State* L) { return register_handler(L, REG_ON_ATTR); }
static int l_zhac_on_cron       (lua_State* L) { return register_handler(L, REG_ON_CRON); }
static int l_zhac_on_mqtt       (lua_State* L) { return register_handler(L, REG_ON_MQTT); }
static int l_zhac_on_boot       (lua_State* L) { return register_handler(L, REG_ON_BOOT); }
static int l_zhac_on_zcl_raw    (lua_State* L) { return register_handler(L, REG_ON_RAW);  }

// ── Module table ──────────────────────────────────────────────────────
static const luaL_Reg kZhacLib[] = {
    {"log",              l_zhac_log},
    {"millis",           l_zhac_millis},
    {"sleep",            l_zhac_sleep},
    {"set_attr",         l_zhac_set_attr},
    {"get_attr",         l_zhac_get_attr},
    {"publish",          l_zhac_publish},
    {"mqtt_publish",     l_zhac_publish},
    {"event",            l_zhac_event},
    {"telegram_settoken", l_zhac_telegram_settoken},
    {"telegram_setchat",  l_zhac_telegram_setchat},
    {"telegram_send",     l_zhac_telegram_send},
    {"on_attr_change",   l_zhac_on_attr_change},
    {"on_cron",          l_zhac_on_cron},
    {"on_mqtt",          l_zhac_on_mqtt},
    {"on_boot",          l_zhac_on_boot},
    {"on_zcl_raw",       l_zhac_on_zcl_raw},
    {NULL, NULL},
};

extern "C" int luaopen_zhac(lua_State* L) {
    luaL_newlib(L, kZhacLib);
    return 1;
}

#endif
