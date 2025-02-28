/*************************************************************************
Public Education Forum Moderation Firehose Client
Copyright (c) Steve Townsend 2024

>>> SOURCE LICENSE >>>
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation (www.fsf.org); either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

A copy of the GNU General Public License is available at
http://www.fsf.org/licensing/licenses
>>> END OF LICENSE >>>
*************************************************************************/

#include "common/moderation/session_manager.hpp"
#include "common/bluesky/client.hpp"
#include "common/log_wrapper.hpp"
#include "restc-cpp/RequestBuilder.h"
#include <boost/fusion/adapted.hpp>
#include <jwt-cpp/traits/boost-json/traits.h>

// com.atproto.server.createSession
BOOST_FUSION_ADAPT_STRUCT(bsky::session_tokens,
                          (std::string, accessJwt)(std::string, refreshJwt))
BOOST_FUSION_ADAPT_STRUCT(bsky::login_info,
                          (std::string, identifier)(std::string, password))

namespace bsky {
pds_session::pds_session(bsky::client &client, std::string const &host)
    : _client(client), _host(host) {}

void pds_session::connect(bsky::login_info const &credentials) {
  _credentials = credentials;
  internal_connect();
}

void pds_session::internal_connect() {
  constexpr bool needs_refresh_check(false);
  constexpr bool no_post_log(true);
  _tokens = _client.do_post<bsky::login_info, bsky::session_tokens>(
      "com.atproto.server.createSession", _credentials, needs_refresh_check,
      no_post_log);

  auto access_token = jwt::decode<jwt::traits::boost_json>(_tokens.accessJwt);
  _access_expiry = access_token.get_expires_at();
  REL_INFO("bsky session access token expires at {}", _access_expiry);
  auto refresh_token = jwt::decode<jwt::traits::boost_json>(_tokens.refreshJwt);
  _refresh_expiry = refresh_token.get_expires_at();
  REL_INFO("bsky session refresh token expires at {}", _refresh_expiry);
}

// this is only called for POSTs, which write and are therefore always
// token-secured
void pds_session::check_refresh() {
  if (_tokens.refreshJwt.empty()) {
    REL_INFO("Skip refresh: no tokens");
  }
  auto now(std::chrono::system_clock::now());
  auto time_to_expiry(std::chrono::duration_cast<std::chrono::milliseconds>(
                          _access_expiry - now)
                          .count());
  if ((_access_expiry < now) ||
      (time_to_expiry <
       static_cast<decltype(time_to_expiry)>(AccessExpiryBuffer.count()))) {
    try {
      REL_INFO("Refresh access token, expiry in {} ms", time_to_expiry);
      bsky::empty empty_body;
      constexpr bool needs_refresh_check(false);
      constexpr bool no_post_log(true);
      _tokens = _client.do_post<bsky::empty, bsky::session_tokens>(
          "com.atproto.server.refreshSession", empty_body, needs_refresh_check,
          no_post_log);
      // assumes refresh and access JWTs have expiry, we are out of luck
      // otherwise
      auto access_token =
          jwt::decode<jwt::traits::boost_json>(_tokens.accessJwt);
      _access_expiry = access_token.get_expires_at();
      REL_INFO("bsky session access token now expires at {}", _access_expiry);
      auto refresh_token =
          jwt::decode<jwt::traits::boost_json>(_tokens.refreshJwt);
      _refresh_expiry = refresh_token.get_expires_at();
      REL_INFO("bsky session refresh token now expires at {}", _refresh_expiry);
    } catch (std::exception const &exc) {
      // Invalid token -> reconnect from scratch. Example result:
      // 2025-02-22 21:22:08.323097733    error     17
      //    POST for com.atproto.server.refreshSession exception
      //    Request failed with HTTP error:
      //    400 Bad Request {"error":"InvalidToken","message":"Token could not
      //    be verified"}
      if (std::string_view(exc.what()).contains("\"error\":\"InvalidToken\"")) {
        REL_WARNING("bsky session token refresh failed, reconnect");
        internal_connect();
      } else {
        throw;
      }
    }
  }
}

} // namespace bsky
