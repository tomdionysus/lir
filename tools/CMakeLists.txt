#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# lir-node: a Dannan node that hosts Lir and serves MQTT.
add_executable(lir-node lir_node.cpp)
target_link_libraries(lir-node PRIVATE lir lir_warnings)
