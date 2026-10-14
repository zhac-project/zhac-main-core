# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host test of cjson.encode's output limit (review F20), real cjson + Lua.
#
#   cmake -B build -S . [-DLUA_DIR=<georgik__lua>] && cmake --build build && ctest --test-dir build
#
# Lua from managed_components/georgik__lua (configure zhac-main-core with IDF
# once, or point LUA_DIR at any checkout that has it). Run with PATH=/usr/bin
# first if ESP-IDF is active (its `as` breaks g++).
cmake_minimum_required(VERSION 3.16)
project(lua_cjson_cap_test C CXX)
set(CMAKE_CXX_STANDARD 20)
set(LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../managed_components/georgik__lua CACHE PATH "georgik__lua")
if(NOT EXISTS ${LUA_DIR}/lua/lapi.c)
    message(FATAL_ERROR "Lua sources not found at ${LUA_DIR}; pass -DLUA_DIR=<georgik__lua>")
endif()
set(CJSON ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
file(GLOB LUA_SRCS ${LUA_DIR}/lua/*.c)
list(FILTER LUA_SRCS EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
add_library(host_lua STATIC ${LUA_SRCS} ${CJSON}/lua_cjson.c ${CJSON}/strbuf.c ${CJSON}/fpconv.c)
target_include_directories(host_lua PUBLIC ${LUA_DIR}/include ${LUA_DIR}/lua ${CJSON})
target_compile_definitions(host_lua PUBLIC CONFIG_LUA_MAXSTACK=1000000 PRIVATE LUA_USE_POSIX)
target_compile_options(host_lua PRIVATE -w -O2)
enable_testing()
add_executable(cjson_cap_tests test_cap.cpp)
target_compile_options(cjson_cap_tests PRIVATE -fsanitize=address,undefined)
target_link_options(cjson_cap_tests PRIVATE -fsanitize=address,undefined)
target_link_libraries(cjson_cap_tests PRIVATE host_lua m)
add_test(NAME cjson_cap COMMAND cjson_cap_tests)
