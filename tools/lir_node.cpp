//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// lir-node: one node of a Lir cluster. A Dannan node (the cluster's
// network) that advertises "lir" and serves MQTT on its own port. It
// reports what it does as one JSON object per line on stdout:
//
//   {"event":"listening","id":"...","port":N,"mqtt_port":M}
//   {"event":"status","connections":N,"sessions":N,"retained":N,...}   every --status-ms
//   {"event":"stopped"}
//
// It runs for --seconds (0: until SIGTERM or SIGINT).
//
//   lir-node --key FILE --state DIR [--port N] [--listen ADDR]
//            [--mqtt-port N] [--mqtt-listen ADDR] [--advertise HOST]
//            [--bootstrap HOST:PORT]... [--name NAME] [--domain NAME]
//            [--heartbeat-ms N] [--status-ms N] [--seconds N]
//            [--log-level LEVEL] [--log-format text|json]
//            [--session-standbys N]
//   lir-node --version

#include <signal.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dannan/node.h"
#include "lir/cluster/cluster.h"
#include "lir/edge/edge.h"
#include "lir/version.h"

namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;

std::atomic<bool> stopping{false};
std::mutex output;

void say(const std::string& line) {
  std::lock_guard lock(output);
  std::cout << line << std::endl;
}

struct Options {
  std::string key;
  std::string state;
  std::string listen = "0.0.0.0";
  std::string advertise;
  uint16_t port = 7437;
  std::string mqtt_listen = "0.0.0.0";
  uint16_t mqtt_port = 1883;
  std::vector<dannan::net::Endpoint> bootstrap;
  std::string name;
  std::string domain;
  std::chrono::milliseconds heartbeat{5000};
  std::chrono::milliseconds status{1000};
  int seconds = 0;
  dannan::status::LogLevel log_level = dannan::status::LogLevel::info;
  dannan::status::LogFormat log_format = dannan::status::LogFormat::text;
  bool version = false;
  size_t session_standbys = 1;
};

dannan::net::Endpoint endpoint(const std::string& text) {
  const auto colon = text.rfind(':');
  if (colon == std::string::npos) throw std::runtime_error("expected HOST:PORT, got " + text);
  return {text.substr(0, colon), static_cast<uint16_t>(std::stoi(text.substr(colon + 1)))};
}

Options parse(int argc, char** argv) {
  Options out;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    auto value = [&]() -> std::string {
      if (++i >= argc) throw std::runtime_error(option + " needs a value");
      return argv[i];
    };
    if (option == "--version")
      out.version = true;
    else if (option == "--key")
      out.key = value();
    else if (option == "--state")
      out.state = value();
    else if (option == "--listen")
      out.listen = value();
    else if (option == "--advertise")
      out.advertise = value();
    else if (option == "--port")
      out.port = static_cast<uint16_t>(std::stoi(value()));
    else if (option == "--mqtt-listen")
      out.mqtt_listen = value();
    else if (option == "--mqtt-port")
      out.mqtt_port = static_cast<uint16_t>(std::stoi(value()));
    else if (option == "--bootstrap")
      out.bootstrap.push_back(endpoint(value()));
    else if (option == "--name")
      out.name = value();
    else if (option == "--domain")
      out.domain = value();
    else if (option == "--heartbeat-ms")
      out.heartbeat = std::chrono::milliseconds(std::stoi(value()));
    else if (option == "--status-ms")
      out.status = std::chrono::milliseconds(std::stoi(value()));
    else if (option == "--seconds")
      out.seconds = std::stoi(value());
    else if (option == "--log-level")
      out.log_level = dannan::status::parse_log_level(value());
    else if (option == "--session-standbys")
      out.session_standbys = static_cast<size_t>(std::stoul(value()));
    else if (option == "--log-format")
      out.log_format = dannan::status::parse_log_format(value());
    else
      throw std::runtime_error("unknown option " + option);
  }
  if (!out.version && (out.key.empty() || out.state.empty())) throw std::runtime_error("--key and --state are required");
  return out;
}

std::string status_json(lir::edge::Edge& edge) {
  struct Figures {
    size_t connections = 0;
    size_t sessions = 0;
    size_t retained = 0;
    lir::broker::Stats stats;
  };
  const auto f =
      edge.call<Figures>([](lir::broker::Broker& broker) { return Figures{broker.connections(), broker.sessions(), broker.retained(), broker.stats()}; });
  std::ostringstream out;
  out << "{\"event\":\"status\",\"connections\":" << f.connections << ",\"sessions\":" << f.sessions << ",\"retained\":" << f.retained
      << ",\"accepted\":" << f.stats.connections_accepted << ",\"refused\":" << f.stats.connections_refused
      << ",\"protocol_errors\":" << f.stats.protocol_errors << ",\"received\":" << f.stats.messages_received << ",\"delivered\":" << f.stats.messages_delivered
      << "}";
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  try {
    options = parse(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "lir-node: " << error.what() << "\n";
    return 2;
  }
  if (options.version) {
    std::cout << lir::kVersion << "\n";
    return 0;
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, [](int) { stopping = true; });
  signal(SIGINT, [](int) { stopping = true; });

  asio::io_context io;
  auto work = asio::make_work_guard(io);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&] { io.run(); });

  int status = 0;
  try {
    dannan::NodeConfig config;
    config.state_path = options.state;
    config.key = dannan::identity::load_cluster_key(options.key);
    config.listen_host = options.listen;
    config.advertise_host = options.advertise;
    config.port = options.port;
    config.bootstrap = options.bootstrap;
    config.node_name = options.name;
    config.failure_domain = options.domain;
    config.heartbeat = options.heartbeat;
    config.dead_after = options.heartbeat * 6;
    config.telemetry_interval = options.heartbeat;
    config.version = std::string("lir-node ") + lir::kVersion;
    config.log = dannan::status::console_sink(options.log_format, options.log_level);
    dannan::Node node(io, config);
    lir::edge::EdgeConfig edge_config;
    edge_config.listen_host = options.mqtt_listen;
    edge_config.port = options.mqtt_port;
    lir::contract::AllowAll allow;
    lir::edge::Edge edge(io, edge_config, dannan::contract::steady_time_source(), allow, node.context().logger("lir"));
    // The cluster's routes are registered before the node starts.
    lir::cluster::ClusterConfig cluster_config;
    cluster_config.session_standbys = options.session_standbys;
    lir::cluster::Cluster cluster(node.context(), edge, cluster_config);
    node.start();
    cluster.start();
    edge.start();
    say("{\"event\":\"listening\",\"id\":\"" + dannan::identity::to_string(node.context().identity().id()) + "\",\"port\":" + std::to_string(node.port()) +
        ",\"mqtt_port\":" + std::to_string(edge.port()) + "}");

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds);
    auto next_status = std::chrono::steady_clock::now() + options.status;
    while (!stopping && (options.seconds == 0 || std::chrono::steady_clock::now() < until)) {
      if (std::chrono::steady_clock::now() >= next_status) {
        say(status_json(edge));
        next_status += options.status;
      }
      std::this_thread::sleep_for(50ms);
    }
    edge.stop();
    cluster.stop();
    node.stop();
  } catch (const std::exception& error) {
    std::cerr << "lir-node: " << error.what() << "\n";
    status = 1;
  }
  work.reset();
  io.stop();
  for (auto& thread : threads) thread.join();
  say("{\"event\":\"stopped\"}");
  return status;
}
