# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host tests for lua_engine's pure parts. Not an IDF project:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(lua_engine_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

enable_testing()

# script.error reports: Lua's prefix parsed, the message cut, one event per script per 10 s
add_executable(lua_err_tests lua_err_test.cpp)
target_include_directories(lua_err_tests PRIVATE ../../include)
add_test(NAME lua_err COMMAND lua_err_tests)
