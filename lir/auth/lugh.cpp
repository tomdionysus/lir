//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/auth/lugh.h"

#include <algorithm>
#include <array>

#include "lugh/core/encoding.h"

namespace lir::auth {

std::variant<TokenClaims, std::string> LughVerifier::verify(std::string_view token) const {
  // The audience is Lir's to check (TokenAuthenticator), so none is asked
  // of Lugh here.
  auto verified = _verifier.verify_access(token);
  if (auto* why = std::get_if<std::string>(&verified)) return std::move(*why);
  auto& access = std::get<lugh::client::AccessToken>(verified);
  TokenClaims claims;
  claims.subject = std::move(access.subject);
  claims.audiences = std::move(access.audiences);
  claims.scopes = std::move(access.scopes);
  claims.expires_unix = access.expires;
  claims.bound_to_key = access.jkt.has_value();
  if (access.x5t) {
    // cnf x5t#S256 is a SHA-256 thumbprint, base64url (RFC 8705, 3.1); one
    // that is not could never match a certificate.
    const auto bytes = lugh::encoding::base64url_decode(*access.x5t);
    if (!bytes || bytes->size() != 32) return std::string("cnf.x5t#S256 is not a SHA-256 thumbprint");
    std::array<uint8_t, 32> thumbprint{};
    std::copy(bytes->begin(), bytes->end(), thumbprint.begin());
    claims.bound_certificate = thumbprint;
  }
  return claims;
}

LughPlugin::LughPlugin(boost::asio::io_context& io, LughConfig config, const dannan::contract::TimeSource& time, const dannan::contract::WallClock& wall)
    : _https(io, config.ca_path),
      _verifier(std::move(config.issuers), wall),
      _keys(lugh::client::HttpsKeySource::make(_verifier, _https, io.get_executor(), time, config.keys)),
      _adapter(_verifier) {}

LughPlugin::~LughPlugin() { stop(); }

void LughPlugin::start() { _keys->start(); }

void LughPlugin::stop() { _keys->stop(); }

}  // namespace lir::auth
