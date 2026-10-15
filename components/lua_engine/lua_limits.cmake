# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The Lua C-stack limits (remote rules and scripts, final fix wave I-2(a), Option A). Every build that runs Lua
# includes this after its idf_component_register(): zhac-main-core's lua_engine and the overrides that build its
# sources (zhac-wired-core, zhac-mono-core). test/host includes it for the same values.
#
# Lua bounds C recursion with one counter, so the limit has to fit the deepest frames a script can stack up.
# Measured with -fstack-usage on the S31 / P4 builds (esp-15.2.0, -O2, stack protector): a counted level costs at
# most 960 B at run time (string.gsub with an __index function) and 304 B in the parser (nested for). 16 levels
# fit a 32 KB TaskLua (CONFIG_LUA_ENGINE_TASK_STACK_BYTES in sdkconfig.defaults) with a 25 % margin:
# 3.3 KB base + 1.25 x (17 x 960 B + 2,752 B for zhac.get_attr + 544 B to raise the error) = 27.8 KB. The parser
# part fits the 12 KB relay and httpd tasks that run script.check: 1.25 x 16 x 304 B + 0.45 KB = 6.5 KB of about
# 9 KB free. The script templates need 5-7 levels, the deepest documentation example 10.
#   LUAI_MAXCCALLS  nested C calls and parser levels (Lua's default 200)
#   MAXCCALLS       lstrlib's pattern-matching recursion (default 200; 48 B a level)
# lua-cjson's nesting depth (16) is in src/lua_engine_limits.h. Change any of these only with the measurement.
set(ZHAC_LUA_LIMIT_DEFS LUAI_MAXCCALLS=16 MAXCCALLS=32)

# In an IDF component (not in early expansion: idf_component_register returns before this line there). IDF
# processes `lua` before the components that require it, so its library target exists here. REQUIRES takes the
# short name `lua`, this lookup only the managed component's own name (idf_component.yml: georgik/lua).
if(COMMAND idf_component_get_property)
    idf_component_get_property(zhac_lua_lib georgik__lua COMPONENT_LIB)
    target_compile_definitions(${zhac_lua_lib} PRIVATE ${ZHAC_LUA_LIMIT_DEFS})
endif()
