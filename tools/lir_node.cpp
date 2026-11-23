//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// lir-node: one node of Lir, a Dannan node serving MQTT. Every node is the
// same; there is no node of any other kind. Its settings are
// lir/edge/settings.h's (`lir-node --help` lists them). It runs until SIGTERM
// or SIGINT, then withdraws its adverts, closes its clients within the
// close bound, and leaves the cluster.

#include <signal.h>

#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dannan/node.h"
#include "lir/auth/anonymous.h"
#include "lir/edge/server.h"
#include "lir/edge/settings.h"
#include "lir/version.h"

#ifdef LIR_WITH_LUGH
#include "lir/auth/lugh.h"
#include "lugh/client/relying_party.h"
#endif

namespace {

std::atomic<bool> stopping{false};

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> arguments(argv + 1, argv + argc);
  if (arguments.size() == 1 && arguments[0] == "--version") {
    std::cout << "lir-node " << lir::kVersion << "\n";
    return 0;
  }
  if (arguments.size() == 1 && arguments[0] == "--help") {
    std::cout << "lir-node [--config PATH] [settings]\n\n" << lir::edge::node_reader().describe();
    return 0;
  }
  lir::edge::NodeSettings settings;
  try {
    const auto read = lir::edge::node_reader().read(arguments);
    if (!read.positional.empty()) throw dannan::config::ConfigError("unexpected argument " + read.positional.front());
    for (const auto& warning : read.warnings) std::cerr << "lir-node: " << warning << "\n";
    settings = lir::edge::node_settings(read);
  } catch (const std::exception& error) {
    std::cerr << "lir-node: " << error.what() << "\n";
    return 2;
  }

  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, [](int) { stopping = true; });
  signal(SIGINT, [](int) { stopping = true; });

  const auto sink = dannan::status::console_sink(settings.log_format, settings.log_level);
  settings.node.log = sink;
  dannan::status::ScopedLogger log(sink, "lir.node");

  boost::asio::io_context io;
  auto work = boost::asio::make_work_guard(io);
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < std::max(2u, std::thread::hardware_concurrency()); ++i) threads.emplace_back([&] { io.run(); });

  int status = 0;
  try {
    dannan::Node node(io, settings.node);
    node.start();
    auto& context = node.context();
    for (const auto& revocation : settings.revocations) context.identity().revoke(revocation);

    std::unique_ptr<lir::contract::Authenticator> authenticator;
#ifdef LIR_WITH_LUGH
    std::unique_ptr<lugh::client::RelyingParty> relying_party;
    if (!settings.lugh_issuers.empty()) {
      lugh::client::RelyingParty::Settings lugh;
      lugh.issuers = settings.lugh_issuers;
      lugh.audience = settings.lugh_audience;
      lugh.ca_path = settings.lugh_ca;
      relying_party =
          std::make_unique<lugh::client::RelyingParty>(io, std::move(lugh), dannan::contract::steady_time_source(), dannan::contract::system_wall_clock());
      relying_party->start();
      authenticator = std::make_unique<lir::auth::Lugh>([&party = *relying_party](std::string_view token) { return party.verify(token); }, settings.anonymous,
                                                        context.time().source(), dannan::contract::system_wall_clock());
    }
#endif
    if (!authenticator) authenticator = std::make_unique<lir::auth::Anonymous>(settings.anonymous);
    if (!settings.anonymous) log.info("anonymous clients are refused");

    lir::edge::Server server(io, context, *authenticator, settings.server, &context.time().source());
    server.start();
    log.info("serving MQTT on port " + std::to_string(server.port()) + (server.tls_port() ? ", TLS on " + std::to_string(server.tls_port()) : std::string()));

    while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    log.info("stopping");
    std::promise<void> closed;
    server.stop(io.get_executor(), [&] { closed.set_value(); });
    closed.get_future().wait_for(settings.server.close_bound * 2);
#ifdef LIR_WITH_LUGH
    if (relying_party) relying_party->stop();
#endif
    node.stop();
  } catch (const std::exception& error) {
    log.error(error.what());
    status = 1;
  }
  work.reset();
  io.stop();
  for (auto& thread : threads) thread.join();
  return status;
}
