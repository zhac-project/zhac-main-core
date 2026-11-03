# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host tests for the Lua CPU-runaway / coroutine-leak fixes (review 2026-10-24):
#   LU-01  lua_coro_leak_tests  — bare coroutine.yield() must not leak a slot
#   LU-02  lua_runaway_tests    — a pcall-wrapped runaway can't swallow the kill
# Both build the vendored Lua 5.5 on the host (as test/host, test/attr_value do).
#   cmake -B build -S . [-DLUA_DIR=<georgik__lua>] && cmake --build build && ctest --test-dir build
# Run with PATH=/usr/bin first if ESP-IDF is active (its `as` breaks g++).
cmake_minimum_required(VERSION 3.16)
project(lua_runaway_host_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

include(${CMAKE_CURRENT_SOURCE_DIR}/../../lua_limits.cmake)   # LUAI_MAXCCALLS / MAXCCALLS
set(LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../managed_components/georgik__lua CACHE PATH "georgik__lua")
if(NOT EXISTS ${LUA_DIR}/lua/lapi.c)
    message(FATAL_ERROR "Lua 5.5 sources not found at ${LUA_DIR}: configure zhac-main-core with IDF once "
                        "(it downloads managed_components), or pass -DLUA_DIR=<georgik__lua>")
endif()
file(GLOB LUA_SRCS ${LUA_DIR}/lua/*.c)
list(FILTER LUA_SRCS EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
add_library(host_lua STATIC ${LUA_SRCS})
target_include_directories(host_lua PUBLIC ${LUA_DIR}/include ${LUA_DIR}/lua)
target_compile_definitions(host_lua PUBLIC CONFIG_LUA_MAXSTACK=1000000 PRIVATE LUA_USE_POSIX ${ZHAC_LUA_LIMIT_DEFS})
target_compile_options(host_lua PRIVATE -w -O2)   # vendored code, as upstream ships it

set(ZC ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../zhac-components/components)

# LU-02: link the REAL sandbox so the actual sandboxed_pcall/xpcall run.
add_executable(lua_runaway_tests test_runaway.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../../src/lua_sandbox.c)
target_include_directories(lua_runaway_tests PRIVATE stubs ../../src ../../include)
target_compile_options(lua_runaway_tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(lua_runaway_tests PRIVATE host_lua m pthread)
add_test(NAME lua_runaway COMMAND lua_runaway_tests)

# LU-01, and LU-02's scheduler half: #includes the REAL scheduler TU
# (src/lua_scheduler.cpp) and links the REAL sandbox, so the real runaway hook
# drives the real kill-aware pcall. Compiled -Wall -Wextra but NOT -Werror — it
# is a firmware TU built for a cross target, and host g++ flags a few benign
# things the IDF toolchain does not.
add_executable(lua_coro_leak_tests test_coro_leak.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../../src/lua_sandbox.c)
target_include_directories(lua_coro_leak_tests PRIVATE stubs ../../src ../../include
    ${ZC}/zap_common/include ${ZC}/event_bus/include ${ZC}/cron_parser/include)
target_compile_options(lua_coro_leak_tests PRIVATE -Wall -Wextra)
target_link_libraries(lua_coro_leak_tests PRIVATE host_lua m pthread)
add_test(NAME lua_coro_leak COMMAND lua_coro_leak_tests)
