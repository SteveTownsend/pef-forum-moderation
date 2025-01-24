/*************************************************************************
NAFO Forum Moderation Firehose Client
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
#include "moderation/embed_checker.hpp"
#include "jwt-cpp/traits/boost-json/traits.h"
#include "log_wrapper.hpp"
#include "matcher.hpp"
#include "metrics.hpp"
#include "moderation/action_router.hpp"
#include "moderation/report_agent.hpp"
#include "post_processor.hpp"
#include "restc-cpp/RequestBuilder.h"

namespace bsky {
namespace moderation {

embed_checker &embed_checker::instance() {
  static embed_checker my_instance;
  return my_instance;
}

embed_checker::embed_checker() : _queue(QueueLimit) {}

void embed_checker::start() {
  restc_cpp::Request::Properties properties;
  properties.maxRedirects = UrlRedirectLimit;
  // favour cache eviction since we are promiscuous about connections
  properties.cacheCleanupIntervalSeconds = 5;
  properties.cacheTtlSeconds = 4;
  properties.cacheMaxConnectionsPerEndpoint = 1;
  properties.cacheMaxConnections = 256;
  for (size_t count = 0; count < NumberOfThreads; ++count) {
    _rest_client = restc_cpp::RestClient::Create(properties);
    _threads[count] = std::thread([&] {
      while (true) {
        embed::embed_info_list embed_list;
        _queue.wait_dequeue(embed_list);
        // process the item
        metrics::instance()
            .operational_stats()
            .Get({{"embed_checker", "backlog"}})
            .Decrement();

        // TODO the work
        // add LFU cache of URL/did/rate-limit pairs
        // add LFU cache of content-cid/did/rate-limit
        // add metrics
        for (auto const &next_embed : embed_list._embeds) {
          embed_handler handler(*this, *_rest_client, embed_list._did,
                                embed_list._path);
          _rest_client->GetConnectionProperties()->redirectFn = std::bind(
              &embed_handler::on_url_redirect, &handler, std::placeholders::_1,
              std::placeholders::_2, std::placeholders::_3);
          ;

          std::visit(handler, next_embed);
        }
        // TODO terminate gracefully
      }
    });
  }
}

void embed_checker::wait_enqueue(embed::embed_info_list &&value) {
  _queue.enqueue(value);
  metrics::instance()
      .operational_stats()
      .Get({{"embed_checker", "backlog"}})
      .Increment();
}

void embed_checker::image_seen(std::string const &repo, std::string const &path,
                               std::string const &cid) {
  // return true if insert fails, we already know this one
  metrics::instance()
      .embed_stats()
      .Get({{"embed_checker", "image_checks"}})
      .Increment();
  std::lock_guard<std::mutex> guard(_lock);
  auto inserted(_checked_images.insert({cid, 1}));
  if (!inserted.second) {
    if (bsky::alert_needed(++(inserted.first->second), ImageFactor)) {
      REL_INFO("Image repetition count {:6} {} at {}/{}",
               inserted.first->second, print_cid(cid), repo, path);
      metrics::instance()
          .embed_stats()
          .Get({{"images", "repetition"}})
          .Increment();
    }
  }
}

void embed_handler::operator()(embed::image const &value) {
  _checker.image_seen(_repo, _path, value._cid);
}

void embed_checker::record_seen(std::string const &repo,
                                std::string const &path,
                                std::string const &uri) {
  metrics::instance()
      .embed_stats()
      .Get({{"embed_checker", "record_checks"}})
      .Increment();
  std::lock_guard<std::mutex> guard(_lock);
  auto inserted(_checked_records.insert({uri, 1}));
  if (!inserted.second) {
    if (bsky::alert_needed(++(inserted.first->second), RecordFactor)) {
      REL_INFO("Record repetition count {:6} {} at {}/{}",
               inserted.first->second, uri, repo, path);
      metrics::instance()
          .embed_stats()
          .Get({{"records", "repetition"}})
          .Increment();
    }
  }
}

void embed_handler::operator()(embed::record const &value) {
  _checker.record_seen(_repo, _path, value._uri);
}

bool embed_checker::uri_seen(std::string const &repo, std::string const &path,
                             std::string const &uri) {
  // return true if insert fails, we already know this one
  metrics::instance()
      .embed_stats()
      .Get({{"embed_checker", "link_checks"}})
      .Increment();
  std::lock_guard<std::mutex> guard(_lock);
  auto inserted(_checked_uris.insert({uri, 1}));
  if (!inserted.second) {
    if (bsky::alert_needed(++(inserted.first->second), LinkFactor)) {
      REL_INFO("Link repetition count {:6} {} at {}/{}", inserted.first->second,
               uri, repo, path);
      metrics::instance()
          .embed_stats()
          .Get({{"links", "repetition"}})
          .Increment();
    }
    return true;
  }
  return false;
}

bool embed_checker::should_process_uri(std::string const &uri) {
  // ensure well-formed, then check whitelist vs substring after prefix to
  // first '/' or end of string
  // check for platform suffix on URLs
  // leftover text may be handled by websites, this shows in posts as trailing
  // "..."
  constexpr std::string_view UrlSuffix = "\xe2\x80\xa6";
  std::string target;
  if (ends_with(uri, UrlSuffix)) {
    target = uri.substr(0, uri.length() - UrlSuffix.length());
  } else {
    target = uri;
  }
  boost::core::string_view url_view = target;
  boost::system::result<boost::urls::url_view> parsed_uri =
      boost::urls::parse_uri(url_view);
  if (parsed_uri.has_error()) {
    // TOTO this fails for multilanguage e.g.
    // https://bsky.app/profile/did:plc:j5k6e6hf2rp4bkqk5sao56ad/post/3lg6hohjsg422
    REL_WARNING("Skip malformed URI {}, error {}", uri,
                parsed_uri.error().message());
    metrics::instance().embed_stats().Get({{"links", "malformed"}}).Increment();
    return false;
  }
  auto host(parsed_uri->host());
  if (starts_with(host, _uri_host_prefix)) {
    host = host.substr(_uri_host_prefix.length());
  }
  if (_whitelist_uris.contains(host)) {
    metrics::instance()
        .embed_stats()
        .Get({{"links", "whitelist_skipped"}})
        .Increment();
    return false;
  }
  return true;
}

void embed_handler::operator()(embed::external const &value) {
  // check redirects for string match, and for abuse
  if (_checker.uri_seen(_repo, _path, value._uri) ||
      !_checker.should_process_uri(value._uri)) {
    return;
  }
  _root_url = value._uri;
  _uri_chain.emplace_back(std::move(value._uri));
  bool done(false);
  bool completed(false);
  bool overflow(false);
  REL_INFO("Redirect check starting for {}", _root_url);
  while (!done) {
    size_t retries(0);
    while (retries < 5) {
      try {
        auto check_done =
            _rest_client.ProcessWithPromise([=](restc_cpp::Context &ctx) {
              restc_cpp::RequestBuilder builder(ctx);
              // pretend to be a browser, like the web-app
              builder.Get(_root_url)
                  .Header("User-Agent",
                          "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                          "AppleWebKit/537.36 (KHTML, like Gecko) "
                          "Chrome/132.0.0.0 Safari/537.36")
                  .Header("Referrer-Policy", "strict-origin-when-cross-origin")
                  .Header(
                      "Accept",
                      "text/html,application/xhtml+xml,application/"
                      "xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8")
                  .Header("Accept-Language", "en-US,en;q=0.9")
                  .Header("Accept-Encoding", "gzip, deflate")
                  .Execute();
            });
        try {
          check_done.get();
          completed = true;
        } catch (boost::system::system_error const &exc) {
          if (exc.code().value() == boost::asio::error::eof &&
              exc.code().category() ==
                  boost::asio::error::get_misc_category()) {
            REL_WARNING("Redirect check: asio eof, retry");
            ++retries;
          } else {
            // unrecoverable error
            REL_ERROR("Redirect check {} Boost exception {}", _root_url,
                      exc.what())
          }
        } catch (std::exception const &ex) {
          REL_ERROR("Redirect check {} exception {}", value._uri, ex.what());
        }
        done = true;
        break;
      } catch (restc_cpp::ConstraintException const &) {
        REL_ERROR("Redirect limit exceeded for {}", _root_url);
        overflow = true;
        done = true;
        // TODO report this
        report_agent::instance().wait_enqueue(
            account_report(_repo, link_redirection(_path, _uri_chain)));
        break;
      } catch (std::exception const &exc) {
        REL_ERROR("Redirect check for {} error {}", _root_url, exc.what());
        done = true;
        break;
      }
    }
  }
  if (completed) {
    metrics::instance()
        .embed_stats()
        .Get({{"link", "redirect_ok"}})
        .Increment();
  } else if (overflow) {
    metrics::instance()
        .embed_stats()
        .Get({{"link", "redirect_limit_exceeded"}})
        .Increment();
  } else {
    metrics::instance()
        .embed_stats()
        .Get({{"link", "redirect_error"}})
        .Increment();
  }
  metrics::instance()
      .link_stats()
      .GetAt({{"redirection", "hops"}})
      .Observe(static_cast<double>(_uri_chain.size()));
  REL_INFO("Redirect check complete {} hops for {}", _uri_chain.size(),
           format_vector(_uri_chain));
}

bool embed_handler::on_url_redirect(int code, std::string &url,
                                    const restc_cpp::Reply &reply) {
  REL_INFO("Redirect code {} for {}", code, url);
  _uri_chain.emplace_back(url);
  // already processed, or whitelisted
  if (_checker.uri_seen(_repo, _path, url) ||
      _checker.should_process_uri(url)) {
    return false; // stop following the chain
  };

  metrics::instance().embed_stats().Get({{"link", "redirections"}}).Increment();
  candidate_list candidate = {{_root_url, "redirected_url", url}};
  match_results results(
      _checker.get_matcher()->all_matches_for_candidates(candidate));
  if (!results.empty()) {
    metrics::instance()
        .embed_stats()
        .Get({{"link", "redirect_matched_rule"}})
        .Increment();

    REL_INFO("Redirect matched rules for {}", url);
    action_router::instance().wait_enqueue({_repo, {{_path, results}}});
  }
  return true;
}

void embed_checker::video_seen(std::string const &repo, std::string const &path,
                               std::string const &cid) {
  // return true if insert fails, we already know this one
  metrics::instance()
      .embed_stats()
      .Get({{"embed_checker", "video_checks"}})
      .Increment();
  std::lock_guard<std::mutex> guard(_lock);
  auto inserted(_checked_videos.insert({cid, 1}));
  if (!inserted.second) {
    if (bsky::alert_needed(++(inserted.first->second), VideoFactor)) {
      REL_INFO("Video repetition count {:6} {} at {}/{}",
               inserted.first->second, print_cid(cid), repo, path);
      metrics::instance()
          .embed_stats()
          .Get({{"videos", "repetition"}})
          .Increment();
    }
  }
}
void embed_handler::operator()(embed::video const &value) {
  _checker.video_seen(_repo, _path, value._cid);
}

} // namespace moderation
} // namespace bsky
