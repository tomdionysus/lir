#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# A consumer of the installed package, as Lugh and Louhi are: it must find
# lir, and through it Dannan and its dependencies, from the prefix alone.
cmake_minimum_required(VERSION 3.24)
project(lir_package_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(lir ${LIR_EXPECTED_VERSION} CONFIG REQUIRED)
add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE lir::mqtt)
