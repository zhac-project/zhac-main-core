// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host stand-in for the Lua-engine Kconfig symbols (LU-01/LU-02 host tests).
#pragma once
#define CONFIG_LUA_ENGINE_ENABLED            1
#define CONFIG_LUA_ENGINE_MAX_COROUTINES     64
#define CONFIG_LUA_ENGINE_TIMER_POOL         8
#define CONFIG_LUA_ENGINE_RESUME_QUEUE_DEPTH 8
#define CONFIG_LUA_ENGINE_TASK_STACK_BYTES   32768
#define CONFIG_LUA_ENGINE_TASK_PRIORITY      5
#define CONFIG_LUA_MAXSTACK                  1000000
