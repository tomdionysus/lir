#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

cmake_minimum_required(VERSION 3.24)

project(lir VERSION 0.1.0 LANGUAGES CXX)

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
option(LIR_BUILD_FUZZING "Build the libFuzzer targets in lir/fuzz; needs a clang that ships libFuzzer" OFF)
option(LIR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(LIR_BUILD_LUGH "Build the Lugh plugin: clients sign in with a Lugh access token (fetches Lugh's lugh_client)" OFF)
# Off: only the MQTT codec (lir::mqtt) and the client library (lir::client),
# for another project that fetches Lir to talk to it (Louhi, Lugh, Mimir).
option(LIR_BUILD_SERVER "Build the broker, lir-node and the tests; off builds only lir::mqtt and lir::client" ON)

find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS headers)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)
find_package(Threads REQUIRED)

# Fuzzing instruments everything the targets reach, Dannan's codec
# included: libFuzzer needs coverage from the parsers, not the harness.
if(LIR_BUILD_FUZZING)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "LIR_BUILD_FUZZING needs clang")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Dannan, the network layer, at its tag, cloned from the sibling checkout
# into the build tree, so building Lir never touches ../dannan.
# A path or a URL: STRING, as a PATH entry would make a URL given with -D
# into a path under the source tree.
set(LIR_DANNAN_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../dannan" CACHE STRING "Dannan's repository, a path or a URL")
# Not a cache variable: a moved pin must take effect in a build tree that
# already exists.
set(LIR_DANNAN_TAG "0.9.0")
include(FetchContent)
# A project that fetches Lir has its own Dannan already: one Dannan a process.
if(NOT TARGET dannan::dannan)
  FetchContent_Declare(dannan GIT_REPOSITORY "${LIR_DANNAN_REPOSITORY}" GIT_TAG "${LIR_DANNAN_TAG}")
  set(DANNAN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(dannan)
endif()

# Lugh's lugh_client, for the optional Lugh plugin (the maintainer, 2026-11-14): a
# commit on Lugh's develop until Lugh releases, tested there on the Dannan
# pinned above, which it takes rather than fetching its own. Only the
# library is built. It carries Boost.JSON's definitions, so Lir's own
# (lir/rules/boost_json.cpp) is left out when it is linked.
if(LIR_BUILD_LUGH AND LIR_BUILD_SERVER)
  set(LIR_LUGH_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../lugh" CACHE STRING "Lugh's repository, a path or a URL")
  set(LIR_LUGH_REF "dbb0573420952336ede86d8db3e86640e48dddb3")
  FetchContent_Declare(lugh GIT_REPOSITORY "${LIR_LUGH_REPOSITORY}" GIT_TAG "${LIR_LUGH_REF}")
  set(LUGH_BUILD_NODE OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(lugh)
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
if(LIR_BUILD_SERVER)
  add_subdirectory(tools)
endif()

if(LIR_BUILD_TESTING AND LIR_BUILD_SERVER)
  find_package(GTest 1.14 CONFIG REQUIRED)
  add_subdirectory(lir/test)
endif()

if(LIR_BUILD_FUZZING AND LIR_BUILD_SERVER)
  add_subdirectory(lir/fuzz)
endif()
