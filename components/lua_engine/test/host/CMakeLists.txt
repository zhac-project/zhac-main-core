# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host tests for lua_engine's pure parts, and for the Lua C-stack limits on the vendored Lua. Not an IDF project:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(lua_engine_host_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

enable_testing()

# script.error reports (Lua's prefix parsed, the message cut, the rate gate) and the boot-loop guard's pure parts
add_executable(lua_err_tests lua_err_test.cpp)
target_include_directories(lua_err_tests PRIVATE ../../include ../../src stubs)   # stubs: esp_system.h (reset reasons)
add_test(NAME lua_err COMMAND lua_err_tests)

# The Lua C-stack limits: the vendored Lua 5.5 (managed_components/georgik__lua, which an IDF configure of this
# repo downloads; -DLUA_DIR=<dir> otherwise) and lua-cjson, built with the definitions lua_limits.cmake gives the
# firmware's Lua library.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../lua_limits.cmake)
set(LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../managed_components/georgik__lua CACHE PATH "georgik__lua")
if(NOT EXISTS ${LUA_DIR}/lua/lapi.c)
    message(FATAL_ERROR "Lua 5.5 sources not found at ${LUA_DIR}: configure zhac-main-core with IDF once "
                        "(it downloads managed_components), or pass -DLUA_DIR=<georgik__lua>")
endif()
set(CJSON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../lua_cjson/src)
file(GLOB LUA_SRCS ${LUA_DIR}/lua/*.c)
list(FILTER LUA_SRCS EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
add_library(host_lua STATIC ${LUA_SRCS} ${CJSON_DIR}/lua_cjson.c ${CJSON_DIR}/strbuf.c ${CJSON_DIR}/fpconv.c)
target_include_directories(host_lua PUBLIC ${LUA_DIR}/include ${LUA_DIR}/lua ${CJSON_DIR})
# CONFIG_LUA_MAXSTACK: luaconf.h reads it (the firmware's Kconfig default); every user of lua.h needs it.
target_compile_definitions(host_lua PUBLIC CONFIG_LUA_MAXSTACK=1000000 PRIVATE LUA_USE_POSIX ${ZHAC_LUA_LIMIT_DEFS})
target_compile_options(host_lua PRIVATE -w -O2)   # vendored code, as upstream ships it; -O2 as the firmware
find_package(Threads REQUIRED)
add_executable(lua_limits_tests lua_limits_test.cpp)
target_include_directories(lua_limits_tests PRIVATE ../../src)
target_link_libraries(lua_limits_tests PRIVATE host_lua m Threads::Threads)
add_test(NAME lua_limits COMMAND lua_limits_tests)
