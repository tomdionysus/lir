//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Built against the installed package: a PUBLISH through the codec and back,
// and the topic grammar, from the installed headers and library alone.
#include <cstdio>
#include <variant>

#include "lir/mqtt/packet.h"
#include "lir/mqtt/topic.h"

int main() {
  using namespace lir::mqtt;
  Publish publish;
  publish.topic = "sensors/kitchen";
  publish.payload = {'1', '9'};
  const auto decoded = decode(encode(publish, Version::v5), Version::v5);
  const auto* back = std::get_if<Publish>(&decoded);
  if (!back || !(*back == publish) || !matches("sensors/+", back->topic)) return 1;
  std::printf("consumer linked lir::mqtt: %s\n", back->topic.c_str());
  return 0;
}
