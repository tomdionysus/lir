#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

add_executable(lir_tests
  interop_mosquitto_test.cpp
  mqtt_packet_test.cpp
  mqtt_properties_test.cpp
  mqtt_topic_test.cpp
  mqtt_wire_test.cpp
  version_test.cpp
)
target_link_libraries(lir_tests PRIVATE lir GTest::gtest_main lir_warnings)
# Dannan's fixtures (temp_dir.h) write only inside Lir's build tree.
target_compile_definitions(lir_tests PRIVATE DANNAN_TEST_TEMP_ROOT="${PROJECT_BINARY_DIR}/test-tmp")

# A real MQTT client (Tom, 2026-11-20: "You will test with a real MQTT
# client"): the mosquitto command-line clients, required, never skipped.
find_program(LIR_MOSQUITTO_PUB mosquitto_pub REQUIRED)
find_program(LIR_MOSQUITTO_SUB mosquitto_sub REQUIRED)
target_compile_definitions(lir_tests PRIVATE LIR_MOSQUITTO_PUB="${LIR_MOSQUITTO_PUB}" LIR_MOSQUITTO_SUB="${LIR_MOSQUITTO_SUB}")
# Dannan's child-process fixture uses a pseudo-terminal: libutil on Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(lir_tests PRIVATE util)
endif()
