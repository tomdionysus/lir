#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# The event system. Public headers are lir/include/lir/*.h, included as
# "lir/x.h"; everything else is included from the repository root as
# "lir/<subsystem>/x.h".
add_library(lir SHARED
  auth/scopes.cpp
  broker/broker.cpp
  cluster/balance.cpp
  cluster/cluster.cpp
  cluster/codec.cpp
  edge/edge.cpp
  http/http.cpp
  mqtt/packet.cpp
  mqtt/properties.cpp
  mqtt/topic.cpp
  mqtt/wire.cpp
  rules/boost_json.cpp
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
  PUBLIC dannan::dannan Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads
  PRIVATE lir_warnings
)
set_target_properties(lir PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
