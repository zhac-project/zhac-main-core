# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host test of `require` over the script store (src/lua_require.c with
# src/lua_script_cache.cpp) against the real Lua library: review F28.
#
#   cmake -B build -S . [-DLUA_DIR=<georgik__lua>] && cmake --build build && ctest --test-dir build
#
# Lua as in ../attr_value; the store's "partition" is a short scratch
# directory, as in ../script_store. Run with PATH=/usr/bin first if ESP-IDF is
# active (its `as` breaks g++).
cmake_minimum_required(VERSION 3.16)
project(lua_require_test C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../managed_components/georgik__lua CACHE PATH "georgik__lua")
if(NOT EXISTS ${LUA_DIR}/lua/lapi.c)
    message(FATAL_ERROR "Lua sources not found at ${LUA_DIR}; pass -DLUA_DIR=<georgik__lua>")
endif()
file(GLOB LUA_SRCS ${LUA_DIR}/lua/*.c)
list(FILTER LUA_SRCS EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
add_library(host_lua STATIC ${LUA_SRCS})
target_include_directories(host_lua PUBLIC ${LUA_DIR}/include ${LUA_DIR}/lua)
target_compile_definitions(host_lua PUBLIC CONFIG_LUA_MAXSTACK=1000000 PRIVATE LUA_USE_POSIX)
target_compile_options(host_lua PRIVATE -w -O2)

set(STORE_DIR /tmp/zhac-require-test CACHE PATH "scratch directory standing in for the partition")
enable_testing()
add_executable(require_tests test_require.cpp ../../src/lua_require.c ../../src/lua_script_cache.cpp)
target_include_directories(require_tests PRIVATE stubs ../script_store/stubs ../../include ../../src)
target_compile_definitions(require_tests PRIVATE LUA_SCRIPT_BASE_PATH="${STORE_DIR}")
target_compile_options(require_tests PRIVATE -fsanitize=address,undefined)
target_link_options(require_tests PRIVATE -fsanitize=address,undefined)
target_link_libraries(require_tests PRIVATE host_lua m)
add_test(NAME require COMMAND require_tests)
