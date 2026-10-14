# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host test of the attribute value -> Lua type mapping (src/lua_attr_value.h)
# against the real Lua library.
#
#   cmake -B build -S . [-DLUA_DIR=<georgik__lua>] && cmake --build build && ctest --test-dir build
#
# Lua comes from managed_components/georgik__lua (configure zhac-main-core
# with IDF once, or point LUA_DIR at any checkout that has it, e.g.
# zhac-wired-core's). zcl_attribute.h comes from the sibling zhac-components.
# Run with PATH=/usr/bin first if ESP-IDF is active (its `as` breaks g++).
cmake_minimum_required(VERSION 3.16)
project(lua_attr_value_test C CXX)

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
target_compile_options(host_lua PRIVATE -w -O2)   # vendored code, as upstream ships it

enable_testing()
add_executable(attr_value_tests test_attr_value.cpp)
target_compile_options(attr_value_tests PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(attr_value_tests PRIVATE -fsanitize=address,undefined)
target_include_directories(attr_value_tests PRIVATE ../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../zhac-components/components/zap_common/include)
target_link_libraries(attr_value_tests PRIVATE host_lua m)
add_test(NAME attr_value COMMAND attr_value_tests)
