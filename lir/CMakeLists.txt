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
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(lir_mqtt
  PUBLIC dannan::dannan
  PRIVATE $<BUILD_INTERFACE:lir_warnings>
)
set_target_properties(lir_mqtt PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} EXPORT_NAME mqtt)

# The client library of the contract (docs/contract.md, section 1), built
# with or without the broker: what Louhi, Lugh and Mimir link.
add_library(lir_client SHARED
  client/client.cpp
)
add_library(lir::client ALIAS lir_client)
target_include_directories(lir_client
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(lir_client
  PUBLIC lir_mqtt dannan::dannan
  PRIVATE $<BUILD_INTERFACE:lir_warnings>
)
set_target_properties(lir_client PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} EXPORT_NAME client)

# Installed only against an installed Dannan: a project that fetches Lir
# brings its own Dannan target, which no export of Lir's can name.
if(LIR_SYSTEM_DANNAN)
  include(CMakePackageConfigHelpers)
  install(TARGETS lir_mqtt lir_client EXPORT lirTargets LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY mqtt/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lir/mqtt FILES_MATCHING PATTERN "*.h")
  install(FILES client/client.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lir/client)
  install(EXPORT lirTargets NAMESPACE lir:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lir)
  configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/lirConfig.cmake.in ${PROJECT_BINARY_DIR}/lirConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lir)
  # Patches of one minor keep the ABI, as the SONAME says.
  write_basic_package_version_file(${PROJECT_BINARY_DIR}/lirConfigVersion.cmake COMPATIBILITY SameMinorVersion)
  install(FILES ${PROJECT_BINARY_DIR}/lirConfig.cmake ${PROJECT_BINARY_DIR}/lirConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lir)
endif()

if(NOT LIR_BUILD_SERVER)
  return()
endif()

# The broker, linked into lir-node and the tests; static, as no other
# program links it (the public libraries are lir::mqtt and lir::client).
add_library(lir STATIC
  auth/anonymous.cpp
  broker/delivery.cpp
  broker/link.cpp
  edge/edge.cpp
  edge/server.cpp
  edge/settings.cpp
  edge/websocket.cpp
  system/views.cpp
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

# The optional Lugh plugin, after lir::mqtt exists for Lugh's topic checks to
# take. Static, as Lugh's client libraries are.
if(LIR_LUGH)
  set(LUGH_BUILD_NODE OFF CACHE BOOL "" FORCE)
  set(LUGH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(lugh GIT_REPOSITORY "${LIR_LUGH_REPOSITORY}" GIT_TAG "${LIR_LUGH_TAG}")
  FetchContent_MakeAvailable(lugh)
  add_library(lir_lugh STATIC
    auth/lugh.cpp
  )
  target_link_libraries(lir_lugh
    PUBLIC lir lugh::client_topics
    PRIVATE lir_warnings
  )
  # The settings take lugh.* only from a build that can use them.
  target_compile_definitions(lir PUBLIC LIR_WITH_LUGH)
endif()

# One node of Lir (tools/lir_node.cpp): a Dannan node serving MQTT.
add_executable(lir-node ${PROJECT_SOURCE_DIR}/tools/lir_node.cpp)
target_link_libraries(lir-node PRIVATE lir lir_warnings)
if(LIR_LUGH)
  target_link_libraries(lir-node PRIVATE lir_lugh)
endif()

# The node's service and its example configuration, installed with the
# installed Dannan, as the Debian build has it (debian/rules splits them).
if(LIR_SYSTEM_DANNAN)
  set(LIR_NODE_BINARY "${CMAKE_INSTALL_FULL_BINDIR}/lir-node")
  set(LIR_CONFDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/lir")
  configure_file(${PROJECT_SOURCE_DIR}/deploy/lir.service.in ${PROJECT_BINARY_DIR}/lir.service @ONLY)
  install(TARGETS lir-node RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES ${PROJECT_BINARY_DIR}/lir.service DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
  install(FILES ${PROJECT_SOURCE_DIR}/config/config.yaml DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/lir)
  install(FILES ${PROJECT_SOURCE_DIR}/docs/man/lir-node.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endif()
