//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <memory>
#include <string>
#include <vector>

#include "dannan/contract/time_source.h"
#include "lir/auth/tokens.h"
#include "lugh/client/https.h"
#include "lugh/client/key_source.h"
#include "lugh/client/verifier.h"

// The Lugh plugin (design, section 1; the maintainer 2026-11-14, P10): clients sign in
// with an access token from a Lugh, checked offline by lugh_client against
// the keys it fetches from each trusted issuer. Built only with
// LIR_BUILD_LUGH; Lir builds and runs without it. Lugh is one way to
// authenticate, not the only one: everything here is behind TokenVerifier,
// and Lir's own checks (audience, scopes, binding, expiry) are
// TokenAuthenticator's.
namespace lir::auth {

// TokenVerifier over lugh_client's Verifier. Thread-safe and waits on
// nothing, as the Verifier is.
class LughVerifier final : public TokenVerifier {
 public:
  // The verifier outlives this.
  explicit LughVerifier(const lugh::client::Verifier& verifier) : _verifier(verifier) {}
  std::variant<TokenClaims, std::string> verify(std::string_view token) const override;

 private:
  const lugh::client::Verifier& _verifier;
};

struct LughConfig {
  // Each trusted Lugh, as its tokens name it (iss): one per site.
  std::vector<std::string> issuers;
  // The roots the issuers' HTTPS certificates chain to; empty: the system's.
  std::string ca_path;
  lugh::client::HttpsKeySource::Settings keys;
};

// The plugin whole: the HTTPS client, the Verifier and the key source that
// keeps its keys fresh, with the TokenVerifier over them.
class LughPlugin {
 public:
  // The io context and the clocks outlive this. Throws std::invalid_argument
  // for no issuers, std::runtime_error for roots that cannot be read.
  LughPlugin(boost::asio::io_context& io, LughConfig config, const dannan::contract::TimeSource& time, const dannan::contract::WallClock& wall);
  ~LughPlugin();
  LughPlugin(const LughPlugin&) = delete;
  LughPlugin& operator=(const LughPlugin&) = delete;

  // Fetches every issuer's keys now and keeps them fresh until stop(). Until
  // an issuer's keys come, its tokens are refused. Waits on nothing.
  void start();
  void stop();
  const TokenVerifier& verifier() const noexcept { return _adapter; }
  // Each issuer's keys: waiting, fresh or stale, and the last error.
  std::vector<lugh::client::HttpsKeySource::IssuerStatus> status() const { return _keys->status(); }

 private:
  lugh::client::HttpsClient _https;
  lugh::client::Verifier _verifier;
  std::shared_ptr<lugh::client::HttpsKeySource> _keys;
  LughVerifier _adapter;
};

}  // namespace lir::auth
