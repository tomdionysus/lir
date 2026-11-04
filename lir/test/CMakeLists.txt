#
# Lir - The Distributed Event System
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

add_executable(lir_tests
  mqtt_packet_test.cpp
  mqtt_properties_test.cpp
  mqtt_topic_test.cpp
  mqtt_wire_test.cpp
  shard_test.cpp
  subs_trie_test.cpp
  version_test.cpp
)
target_link_libraries(lir_tests PRIVATE lir GTest::gtest_main lir_warnings)
