#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# One libFuzzer target per parser of untrusted bytes. Clang only; the
# top-level file instruments the whole build when fuzzing is on.
foreach(target mqtt_packet sn_packet http_target cluster_codec cluster_stored)
  add_executable(${target}_fuzz ${target}_fuzz.cpp)
  target_link_options(${target}_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_libraries(${target}_fuzz PRIVATE lir lir_warnings)
endforeach()
