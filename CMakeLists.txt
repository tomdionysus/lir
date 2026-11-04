#
# Lir - The Distributed Event System
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

find_package(Boost 1.87 CONFIG REQUIRED COMPONENTS headers)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)
find_package(Threads REQUIRED)

# Dannan, the network layer, from the sibling checkout, pinned to one commit
# and cloned into the build tree, so building Lir never touches ../dannan.
# Moves to find_package(dannan 0.2 CONFIG) once the 0.2.0 tag is cut.
set(LIR_DANNAN_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../dannan" CACHE PATH "Dannan's repository")
set(LIR_DANNAN_COMMIT "295ca4bc88a8b18a32d797a62509d55aebfb501a" CACHE STRING "The Dannan commit Lir builds against")
include(FetchContent)
FetchContent_Declare(dannan GIT_REPOSITORY "${LIR_DANNAN_REPOSITORY}" GIT_TAG "${LIR_DANNAN_COMMIT}")
set(DANNAN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(dannan)

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
