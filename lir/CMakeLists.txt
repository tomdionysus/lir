#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# The event system. Public headers are lir/include/lir/*.h, included as
# "lir/x.h"; everything else is included from the repository root as
# "lir/<subsystem>/x.h".

# The MQTT codec, on its own so a client links it without the broker: the
# stack's one MQTT codec.
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

# The MQTT client library (the maintainer, 2026-11-15), for the stack's projects that
# publish or subscribe on Lir: Louhi, Lugh's events, Mimir.
add_library(lir_client SHARED
  client/client.cpp
)
add_library(lir::client ALIAS lir_client)
target_link_libraries(lir_client
  PUBLIC lir_mqtt dannan::dannan Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads
  PRIVATE lir_warnings
)
set_target_properties(lir_client PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})

if(NOT LIR_BUILD_SERVER)
  return()
endif()

add_library(lir SHARED
  auth/scopes.cpp
  auth/tokens.cpp
  broker/broker.cpp
  cluster/balance.cpp
  cluster/cluster.cpp
  cluster/codec.cpp
  cluster/sequence.cpp
  edge/edge.cpp
  http/http.cpp
  rules/rules.cpp
  shard/shard.cpp
  sn/gateway.cpp
  sn/packet.cpp
  system/ledger.cpp
  system/system.cpp
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
# The Lugh plugin, linked into the library whole so Boost.JSON's definitions
# (lugh_client's then) are in one place; without it, Lir compiles its own.
if(LIR_BUILD_LUGH)
  target_sources(lir PRIVATE auth/lugh.cpp)
  target_link_libraries(lir PUBLIC lugh::client)
  target_compile_definitions(lir PUBLIC LIR_WITH_LUGH=1)
else()
  target_sources(lir PRIVATE rules/boost_json.cpp)
endif()
set_target_properties(lir PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
