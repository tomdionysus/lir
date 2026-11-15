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

find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS headers)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)
find_package(Threads REQUIRED)

# Dannan, the network layer, at its tag, cloned from the sibling checkout
# into the build tree, so building Lir never touches ../dannan.
set(LIR_DANNAN_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../dannan" CACHE PATH "Dannan's repository")
# Not a cache variable: a moved pin must take effect in a build tree that
# already exists.
set(LIR_DANNAN_TAG "0.8.0")
include(FetchContent)
FetchContent_Declare(dannan GIT_REPOSITORY "${LIR_DANNAN_REPOSITORY}" GIT_TAG "${LIR_DANNAN_TAG}")
set(DANNAN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(dannan)

# Lugh's lugh_client, for the optional Lugh plugin (Tom, 2026-11-14): a
# commit on Lugh's develop until Lugh releases, tested there on the Dannan
# pinned above, which it takes rather than fetching its own. Only the
# library is built. It carries Boost.JSON's definitions, so Lir's own
# (lir/rules/boost_json.cpp) is left out when it is linked.
if(LIR_BUILD_LUGH)
  set(LIR_LUGH_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../lugh" CACHE PATH "Lugh's repository")
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
add_subdirectory(tools)

if(LIR_BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  add_subdirectory(lir/test)
endif()

if(LIR_BUILD_FUZZING)
  add_subdirectory(lir/fuzz)
endif()
