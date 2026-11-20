#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# Rewritten from scratch (the maintainer, 2026-11-20). Public headers are
# lir/include/lir/*.h, included as "lir/x.h"; everything else is included from
# the repository root as "lir/<subsystem>/x.h". lir::mqtt and lir::client
# return here as docs/contract.md states them.

# The MQTT codec, on its own so a client links it without the broker: the
# stack's one MQTT codec (docs/contract.md, section 2).
add_library(lir_mqtt SHARED
  mqtt/packet.cpp
  mqtt/properties.cpp
  mqtt/topic.cpp
  mqtt/wire.cpp
)
add_library(lir::mqtt ALIAS lir_mqtt)
target_include_directories(lir_mqtt
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
)
target_link_libraries(lir_mqtt
  PUBLIC dannan::dannan
  PRIVATE lir_warnings
)
set_target_properties(lir_mqtt PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})

if(NOT LIR_BUILD_SERVER)
  return()
endif()

add_library(lir SHARED
  version.cpp
)
target_include_directories(lir
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
)
target_link_libraries(lir
  PUBLIC lir_mqtt dannan::dannan Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads
  PRIVATE lir_warnings
)
set_target_properties(lir PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
