# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host test of the script store (src/lua_script_cache.cpp): concurrent saves of
# one script, and recovery from a power cut in the middle of a save.
#
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
#
# The "partition" is a directory in the build tree, and rename() refuses to
# replace an existing file, as SPIFFS's does. Run with PATH=/usr/bin first if
# ESP-IDF is active (its `as` breaks g++).
cmake_minimum_required(VERSION 3.16)
project(lua_script_store_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

enable_testing()
# Short: the store builds paths into 96-byte buffers.
set(STORE_DIR /tmp/zhac-script-store-test CACHE PATH "scratch directory standing in for the partition")
add_executable(script_store_tests test_script_store.cpp ../../src/lua_script_cache.cpp)
target_include_directories(script_store_tests PRIVATE stubs ../../include)
target_compile_definitions(script_store_tests PRIVATE
    LUA_SCRIPT_BASE_PATH="${STORE_DIR}")
target_compile_options(script_store_tests PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(script_store_tests PRIVATE -fsanitize=address,undefined -Wl,--wrap=rename)
target_link_libraries(script_store_tests PRIVATE Threads::Threads)
add_test(NAME script_store COMMAND script_store_tests)
