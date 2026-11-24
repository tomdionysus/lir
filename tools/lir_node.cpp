//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// lir-node: one node of Lir, a Dannan node serving MQTT. Every node is the
// same; there is no node of any other kind. Its main is Dannan's
// node-program helper: the settings (cluster.*, log.*, node.* and Lir's own,
// lir/edge/settings.h; `lir-node --help` lists them), the node, signals and
// the drain. On SIGTERM or SIGINT it drains: it withdraws its adverts and
// closes its clients within the close bound, then leaves the cluster.

#include <memory>
#include <optional>
#include <string>

#include "dannan/supervise/node_program.h"
#include "lir/auth/anonymous.h"
#include "lir/edge/server.h"
#include "lir/edge/settings.h"
#include "lir/version.h"

#ifdef LIR_WITH_LUGH
#include "lir/auth/lugh.h"
#include "lugh/client/relying_party.h"
#endif

int main(int argc, char** argv) {
  dannan::supervise::ProgramSpec spec;
  spec.name = "lir-node";
  spec.prefix = std::string(lir::edge::settings_prefix);
  spec.version = lir::kVersion;
  spec.default_config = "/etc/lir/config.yaml";
  spec.settings = [](dannan::config::Reader& reader) { lir::edge::add_settings(reader); };

  // Read and checked before any node is built, so a setting that cannot be
  // used stops the start (exit 2), naming it.
  lir::edge::LirSettings settings;
  spec.configure = [&](const dannan::config::Settings& read, dannan::NodeConfig& node) {
    lir::edge::require_node(read, node);
    settings = lir::edge::lir_settings(read);
  };

  std::unique_ptr<lir::contract::Authenticator> authenticator;
#ifdef LIR_WITH_LUGH
  std::unique_ptr<lugh::client::RelyingParty> relying_party;
#endif
  std::unique_ptr<lir::edge::Server> server;

  return dannan::supervise::run_node_program(argc, argv, spec, [&](dannan::supervise::Program& program) {
    auto& context = program.context();
    auto log = program.logger("lir.node");
#ifdef LIR_WITH_LUGH
    if (!settings.lugh_issuers.empty()) {
      lugh::client::RelyingParty::Settings lugh;
      lugh.issuers = settings.lugh_issuers;
      lugh.audience = settings.lugh_audience;
      lugh.ca_path = settings.lugh_ca;
      relying_party =
          std::make_unique<lugh::client::RelyingParty>(program.io(), std::move(lugh), context.time().source(), dannan::contract::system_wall_clock());
      relying_party->start();
      authenticator = std::make_unique<lir::auth::Lugh>([&party = *relying_party](std::string_view token) { return party.verify(token); }, settings.anonymous,
                                                        context.time().source(), dannan::contract::system_wall_clock());
      program.on_stop([&] { relying_party->stop(); });
    }
#endif
    if (!authenticator) authenticator = std::make_unique<lir::auth::Anonymous>(settings.anonymous);
    if (!settings.anonymous) log.info("anonymous clients are refused");

    server = std::make_unique<lir::edge::Server>(program.io(), context, *authenticator, settings.server, &context.time().source());
    server->start();
    context.readiness().set("lir.serving", dannan::status::Health::ready);
    log.info("serving MQTT on port " + std::to_string(server->port()) +
             (server->tls_port() ? ", TLS on " + std::to_string(server->tls_port()) : std::string()));

    // The drain: the adverts withdrawn first, then every client closed within
    // the close bound.
    program.on_drain([&](dannan::contract::Deadline, std::function<void()> done) {
      context.readiness().set("lir.serving", dannan::status::Health::draining, "stopping");
      server->stop(program.io().get_executor(), std::move(done));
    });
    // Gone before the node stops: its views unwatch the node's roster.
    program.on_stop([&] { server.reset(); });
  });
}
