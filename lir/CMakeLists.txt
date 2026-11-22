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
