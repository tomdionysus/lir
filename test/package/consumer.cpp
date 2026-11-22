//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Built against the installed package: a PUBLISH through the codec and back,
// the topic grammar, and a client made and stopped, from the installed headers
// and libraries alone.
#include <boost/asio/io_context.hpp>
#include <cstdio>
#include <variant>

#include "lir/client/client.h"
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

  boost::asio::io_context io;
  lir::client::ClientConfig config;
  config.host = "127.0.0.1";
  config.client_id = "consumer";
  bool stopped = false;
  {
    lir::client::Client client(io.get_executor(), config, dannan::contract::steady_time_source(),
                               dannan::status::ScopedLogger(dannan::status::null_sink(), "consumer"));
    client.stop([&] { stopped = true; });
    io.run_for(std::chrono::seconds(5));
  }
  if (!stopped) return 1;
  std::printf("consumer linked lir::client\n");
  return 0;
}
