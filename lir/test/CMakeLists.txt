#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

add_executable(lir_tests
  broker_test.cpp
  cluster_test.cpp
  cluster_codec_test.cpp
  edge_test.cpp
  http_test.cpp
  mqtt_packet_test.cpp
  mqtt_properties_test.cpp
  mqtt_topic_test.cpp
  mqtt_wire_test.cpp
  process_test.cpp
  shard_test.cpp
  subs_trie_test.cpp
  version_test.cpp
)
target_link_libraries(lir_tests PRIVATE lir GTest::gtest_main lir_warnings)
# Dannan's fixtures (temp_dir.h) write only inside Lir's build tree.
target_compile_definitions(lir_tests PRIVATE DANNAN_TEST_TEMP_ROOT="${PROJECT_BINARY_DIR}/test-tmp")

# The process test runs lir-node as a child, against the mosquitto clients
# where they are installed.
find_program(LIR_MOSQUITTO_PUB mosquitto_pub)
find_program(LIR_MOSQUITTO_SUB mosquitto_sub)
target_compile_definitions(lir_tests PRIVATE LIR_NODE_BINARY="$<TARGET_FILE:lir-node>" LIR_MOSQUITTO_PUB="${LIR_MOSQUITTO_PUB}"
                                             LIR_MOSQUITTO_SUB="${LIR_MOSQUITTO_SUB}")
add_dependencies(lir_tests lir-node)
