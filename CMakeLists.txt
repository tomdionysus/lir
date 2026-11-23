#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

cmake_minimum_required(VERSION 3.24)

project(lir VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Executables at the top of the build tree: ./build/lir_tests.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LIR_BUILD_TESTING "Build lir_tests (GoogleTest)" OFF)
option(LIR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
# Off: only lir::mqtt and lir::client, for a project that fetches Lir to talk
# to it (docs/contract.md, section 3).
option(LIR_BUILD_SERVER "Build the broker and the tests; off builds only lir::mqtt and lir::client" ON)
option(LIR_BUILD_FUZZING "Build the libFuzzer targets (lir/fuzz/), with clang" OFF)
# On: Dannan is the installed package (find_package), never a copy, and Lir's
# install rules are defined, as the Debian build has it (docs/packaging.md).
option(LIR_SYSTEM_DANNAN "Build against the installed Dannan and define the install rules" OFF)
# On: the optional Lugh plugin (lir/auth/lugh.h), on Lugh's lugh_client;
# Lir builds and serves without it (the maintainer, 2026-11-14).
option(LIR_LUGH "Build the Lugh plugin and its tests" OFF)

find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS headers)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)
find_package(Threads REQUIRED)

# A fuzz build runs under AddressSanitizer and UndefinedBehaviorSanitizer,
# Dannan included: instrumented and plain code sharing containers give ASan
# false reports.
if(LIR_BUILD_FUZZING)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Dannan, at its tag, cloned from the sibling checkout into the build tree,
# so building Lir never touches ../dannan. A path or a URL: STRING, as a PATH
# entry would make a URL given with -D into a path under the source tree.
set(LIR_DANNAN_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../dannan" CACHE STRING "Dannan's repository, a path or a URL")
# Not a cache variable: a moved pin must take effect in a build tree that
# already exists.
set(LIR_DANNAN_TAG "0.11.1")
# Lugh, for the plugin, at its tag, as Dannan is. Fetched with neither its
# node nor its tests, so it takes Lir's own lir::mqtt and dannan::dannan.
set(LIR_LUGH_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../lugh" CACHE STRING "Lugh's repository, a path or a URL")
set(LIR_LUGH_TAG "0.6.0")
include(FetchContent)
include(GNUInstallDirs)
# A project that fetches Lir has its own Dannan already: one Dannan a process.
# The installed one is found by minor, at least the pinned patch (Dannan's
# SameMinorVersion).
if(LIR_SYSTEM_DANNAN)
  find_package(dannan ${LIR_DANNAN_TAG} CONFIG REQUIRED)
elseif(NOT TARGET dannan::dannan)
  FetchContent_Declare(dannan GIT_REPOSITORY "${LIR_DANNAN_REPOSITORY}" GIT_TAG "${LIR_DANNAN_TAG}")
  set(DANNAN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(dannan)
endif()

# Every TSan run passes the stack's one list of suppressions, which Dannan
# names whether found installed or fetched; tools/ci.sh reads its path here.
if(DEFINED DANNAN_TSAN_SUPPRESSIONS)
  file(WRITE "${PROJECT_BINARY_DIR}/tsan-suppressions.path" "${DANNAN_TSAN_SUPPRESSIONS}")
endif()

# Only Lir's own code is instrumented for libFuzzer's coverage, so the fuzzer
# explores Lir's parsers and not Dannan's internals.
if(LIR_BUILD_FUZZING)
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

add_library(lir_warnings INTERFACE)
target_compile_options(lir_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(lir_warnings INTERFACE -Wthread-safety)
endif()
if(LIR_WARNINGS_AS_ERRORS)
  target_compile_options(lir_warnings INTERFACE -Werror)
endif()

configure_file(lir/include/lir/version.h.in ${PROJECT_BINARY_DIR}/generated/lir/version.h @ONLY)

add_subdirectory(lir)

if(LIR_BUILD_TESTING AND LIR_BUILD_SERVER)
  find_package(GTest 1.14 CONFIG REQUIRED)
  add_subdirectory(lir/test)
endif()

if(LIR_BUILD_FUZZING)
  add_subdirectory(lir/fuzz)
endif()
