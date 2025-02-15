#pragma once
/*************************************************************************
Public Education Forum Moderation Firehose Client
Copyright (c) Steve Townsend 2025

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
#include "common/bluesky/platform.hpp"
#include "common/log_wrapper.hpp"
#include "common/metrics_factory.hpp"
#include "common/moderation/session_manager.hpp"
#include "common/rest_utils.hpp"

// clang-format off
#include "restc-cpp/restc-cpp.h"
// clang-format on
#include "restc-cpp/RequestBody.h"
#include "restc-cpp/RequestBuilder.h"
#include "restc-cpp/SerializeJson.h"
#include "yaml-cpp/yaml.h"
#include <thread>
#include <unordered_set>

namespace bsky {
struct empty {};

struct profile_view_detailed {
  // all we need right now
  std::string did;
  std::string handle;
};
struct get_profiles_response {
  std::vector<profile_view_detailed> profiles;
};

namespace moderation {
struct report_subject {
  std::string _type = std::string(atproto::AdminDefsRepoRef);
  // TODO should be optional, add std::optional<strong_ref> for content, if
  // needed
  std::string did;
};
// always report the account. Report content indicates context.
struct report_request {
  std::string reasonType = std::string(bsky::moderation::ReasonOther);
  std::string reason;
  report_subject subject;
};
struct report_response {
  // ignore other fields
  std::string createdAt;
  int64_t id;
  std::string reportedBy;
};

struct label_event {
  std::string _type = std::string(bsky::moderation::EventLabel);
  std::vector<std::string> createLabelVals;
  std::vector<std::string> negateLabelVals;
};
// Label auto-reported account. Associated eport indicates context
struct emit_event_label_request {
  label_event event;
  report_subject subject;
  std::string createdBy;
};
struct acknowledge_event_comment {
  acknowledge_event_comment() = delete;
  inline acknowledge_event_comment(std::string const &project_name)
      : descriptor(project_name) {}
  std::string descriptor;
  std::string context;
  std::string did;
  std::string path;
};
struct acknowledge_event {
  std::string _type = std::string(bsky::moderation::EventAcknowledge);
  std::string comment;
  bool acknowledgeAccountSubjects = false;
};
struct emit_event_acknowledge_request {
  acknowledge_event event;
  report_subject subject;
  std::string createdBy;
};
struct tag_event_comment {
  tag_event_comment() = delete;
  inline tag_event_comment(std::string const &project_name)
      : descriptor(project_name) {}
  std::string descriptor;
};
struct tag_event {
  std::string _type = std::string(bsky::moderation::EventTag);
  std::string comment;
  // both lists are mandatory
  std::vector<std::string> add;
  std::vector<std::string> remove;
};
struct emit_event_tag_request {
  tag_event event;
  report_subject subject;
  std::string createdBy;
};
struct comment_event_comment {
  comment_event_comment() = delete;
  inline comment_event_comment(std::string const &project_name)
      : descriptor(project_name) {}
  std::string descriptor;
  std::string context;
  std::string reason;
};
struct comment_event {
  std::string _type = std::string(bsky::moderation::EventComment);
  std::string comment;
};

struct emit_event_comment_request {
  comment_event event;
  report_subject subject;
  std::string createdBy;
};
struct emit_event_response {
  // ignore other fields
  std::string createdAt;
  int64_t id;
  std::string createdBy;
};
} // namespace moderation

template <typename OBJ>
std::string as_string(OBJ const &obj,
                      restc_cpp::serialize_properties_t properties = {}) {
  std::ostringstream oss;
  restc_cpp::SerializeToJson(obj, oss, properties);
  return oss.str();
}

class client {
public:
  client() = default;
  ~client() = default;

  void set_config(YAML::Node const &settings);
  std::string service_did() const { return _service_did; }
  inline bool is_ready() const { return _is_ready; }

  void label_account(std::string const &did,
                     std::vector<std::string> const &labels);
  void add_comment_for_subject(
      std::string const &did,
      bsky::moderation::comment_event_comment const &comment,
      std::string const &path);
  void acknowledge_subject(
      std::string const &did,
      bsky::moderation::acknowledge_event_comment const &comment,
      std::string const &path = {});
  void tag_report_subject(std::string const &did, std::string const &path,
                          bsky::moderation::tag_event_comment const &comment,
                          std::vector<std::string> const &add_tags,
                          std::vector<std::string> const &remove_tags);

  template <typename RECORD>
  atproto::create_record_response create_record(RECORD const &record) {
    size_t retries(0);
    atproto::create_record_response response;
    std::string record_str(as_string<RECORD>(record));
    while (retries < 5) {
      try {
        _session->check_refresh();

        restc_cpp::SerializeProperties properties;
        properties.name_mapping = &json::TypeFieldMapping;
        response =
            _rest_client
                ->ProcessWithPromiseT<atproto::create_record_response>(
                    [&](restc_cpp::Context &ctx) {
                      // This is a co-routine, running in a worker-thread
                      // Serialize response asynchronously. The asynchronous
                      // part does not really matter here, but it may if you
                      // receive huge data structures.
                      restc_cpp::SerializeFromJson(
                          response,

                          // Construct a request to the server
                          restc_cpp::RequestBuilder(ctx)
                              .Post(_host + "com.atproto.repo.createRecord")
                              .Header("Content-Type", "application/json")
                              .Header("Authorization",
                                      std::string("Bearer " +
                                                  _session->access_token()))
                              .Data(record_str)
                              // Send the request
                              .Execute());

                      // Return the session instance through C++ future<>
                      return response;
                    })

                // Get the Post instance from the future<>, or any C++
                // exception thrown within the lambda.
                .get();
        REL_INFO("createRecord for {} yielded uri {}", record_str,
                 response.uri);
        break;
      } catch (boost::system::system_error const &exc) {
        if (exc.code().value() == boost::asio::error::eof &&
            exc.code().category() == boost::asio::error::get_misc_category()) {
          REL_WARNING(
              "IoReaderImpl::ReadSome(createListItem): asio eof, retry");
          ++retries;
        } else {
          // unrecoverable error
          throw;
        }
      } catch (std::exception const &exc) {
        REL_ERROR("createRecord {} exception {}", record_str, exc.what());
        throw;
      }
    }
    return response;
  }

  template <typename RESPONSE>
  RESPONSE get_record(std::string const &did, std::string const &collection,
                      std::string const &rkey) {
    size_t retries(0);
    // Instantiate a get_record structure, we will edit the record with
    // updates and put it back
    RESPONSE response;

    while (retries < 5) {
      try {
        restc_cpp::SerializeProperties properties;
        properties.name_mapping = &json::TypeFieldMapping;
        response =
            _rest_client
                ->ProcessWithPromiseT<RESPONSE>([&](restc_cpp::Context &ctx) {
                  // This is a co-routine, running in a worker-thread
                  // Construct a request to the server

                  restc_cpp::SerializeFromJson(
                      response,
                      // Send the request
                      restc_cpp::RequestBuilder(ctx)
                          .Get(_host + "com.atproto.repo.getRecord")
                          .Header(
                              "Authorization",
                              std::string("Bearer " + _session->access_token()))
                          .Argument("repo", did)
                          .Argument("collection", collection)
                          .Argument("rkey", rkey)
                          .Execute(),
                      &json::TypeFieldMapping);

                  // Return the list record instance through C++
                  // future<>
                  return response;
                })

                // Get the Post instance from the future<>, or any C++
                // exception thrown within the lambda.
                .get();
        REL_INFO("getRecord OK for {} {} {}", did, collection, rkey);
        break;
      } catch (boost::system::system_error const &exc) {
        if (exc.code().value() == boost::asio::error::eof &&
            exc.code().category() == boost::asio::error::get_misc_category()) {
          REL_WARNING("IoReaderImpl::ReadSome(getRecord): asio eof, retry");
          if (++retries >= 5)
            throw;
        } else {
          // unrecoverable error
          REL_ERROR("getRecord for {} {} {} Boost exception {}", did,
                    collection, rkey, exc.what())
          throw;
        }
      } catch (std::exception const &exc) {
        REL_ERROR("getRecord for {} {} {} exception {}", did, collection, rkey,
                  exc.what())
        throw;
      }
    }
    return response;
  }

  template <typename RECORD>
  atproto::put_record_response put_record(RECORD const &record) {
    size_t retries(0);
    atproto::put_record_response response;
    std::string record_str(as_string<RECORD>(record));
    while (retries < 5) {
      try {
        _session->check_refresh();

        restc_cpp::SerializeProperties properties;
        properties.name_mapping = &json::TypeFieldMapping;
        response =
            _rest_client
                ->ProcessWithPromiseT<atproto::put_record_response>(
                    [&](restc_cpp::Context &ctx) {
                      // This is a co-routine, running in a worker-thread
                      // Serialize it asynchronously. The asynchronous
                      // part does not really matter here, but it may if you
                      // receive huge data structures.
                      restc_cpp::SerializeFromJson(
                          response,

                          // Construct a request to the server
                          restc_cpp::RequestBuilder(ctx)
                              .Post(_host + "com.atproto.repo.putRecord")
                              .Header("Content-Type", "application/json")
                              .Header("Authorization",
                                      std::string("Bearer " +
                                                  _session->access_token()))
                              .Data(record_str)
                              // Send the request
                              .Execute());

                      // Return the record instance through C++
                      // future<>
                      return response;
                    })

                // Get the Post instance from the future<>, or any C++
                // exception thrown within the lambda.
                .get();
        REL_INFO("putRecord OK for {}", record_str);
        break;
      } catch (boost::system::system_error const &exc) {
        if (exc.code().value() == boost::asio::error::eof &&
            exc.code().category() == boost::asio::error::get_misc_category()) {
          REL_WARNING("IoReaderImpl::ReadSome(putRecord): asio eof, retry");
          ++retries;
        } else {
          // unrecoverable error
          throw;
        }
      } catch (std::exception const &exc) {
        REL_ERROR("putRecord for {} exception {}", record_str, exc.what())
        throw;
      }
    }
    return response;
  }

  template <typename REASON>
  void send_report(std::string const &did, REASON const &reason) {
    // serialize the report-reason and request-body only once
    restc_cpp::serialize_properties_t properties;
    properties.name_mapping = &json::TypeFieldMapping;

    bsky::moderation::report_request request;
    request.subject.did = did;

    std::ostringstream oss;
    restc_cpp::SerializeToJson(reason, oss, properties);
    request.reason = oss.str();

    // Instantiate a report_response structure.
    bsky::moderation::report_response response;
    std::ostringstream body;
    restc_cpp::SerializeToJson(request, body, properties);

    if (!_is_ready) {
      REL_ERROR("Bluesky client not ready, skip report of {}", body.str());
    }
    if (_dry_run) {
      REL_INFO("Dry-run Report of {}", body.str());
      return;
    }

    bool done(false);
    size_t retries(0);

    while (retries < 5) {
      try {
        _session->check_refresh();
        response =
            _rest_client
                ->ProcessWithPromiseT<bsky::moderation::report_response>(
                    [&](restc_cpp::Context &ctx) {
                      // This is a co-routine, running in a worker-thread

                      // Serialize it asynchronously. The asynchronously
                      // part does not really matter here, but it may if you
                      // receive huge data structures.
                      restc_cpp::SerializeFromJson(
                          response,

                          // Construct a request to the server
                          restc_cpp::RequestBuilder(ctx)
                              .Post(_host +
                                    "com.atproto.moderation.createReport")
                              .Header("Content-Type", "application/json")
                              .Header("Atproto-Accept-Labelers", _service_did)
                              .Header(
                                  "Atproto-Proxy",
                                  _service_did +
                                      std::string(atproto::ProxyLabelerSuffix))
                              .Header("Authorization",
                                      std::string("Bearer " +
                                                  _session->access_token()))
                              .Data(body.str())
                              // Send the request
                              .Execute());

                      // Return the session instance through C++ future<>
                      return response;
                    })

                // Get the Post instance from the future<>, or any C++
                // exception thrown within the lambda.
                .get();
        REL_INFO("Report of {} {} recorded at {}, reporter "
                 "{} id={}",
                 did, request.reason, response.createdAt, response.reportedBy,
                 response.id);
        metrics_factory::instance()
            .get_counter("automation")
            .Get({{"report", reason.get_name()}})
            .Increment();
        done = true;
        break;
      } catch (boost::system::system_error const &exc) {
        if (exc.code().value() == boost::asio::error::eof &&
            exc.code().category() == boost::asio::error::get_misc_category()) {
          REL_WARNING("IoReaderImpl::ReadSome(createReport): asio eof, retry");
          ++retries;
        } else {
          // unrecoverable error
          REL_ERROR("Create report of {} {} Boost exception {}", did,
                    request.reason, exc.what());
          break;
        }
      } catch (std::exception const &exc) {
        REL_ERROR("Create report of {} {} exception {}", did, request.reason,
                  exc.what());
        break;
      }
    }
    if (!done) {
      metrics_factory::instance()
          .get_counter("automation")
          .Get({{"report_error", reason.get_name()}})
          .Increment();
    }
  }

  typedef std::function<void(restc_cpp::RequestBuilder &builder)>
      get_callback_t;

  template <typename RESPONSE>
  RESPONSE do_get(std::string const &relative_path,
                  std::optional<get_callback_t> callback =
                      std::optional<get_callback_t>()) {
    size_t retries(0);
    RESPONSE response;

    while (retries < 5) {
      try {
        restc_cpp::SerializeProperties properties;
        properties.name_mapping = &json::TypeFieldMapping;
        response =
            _rest_client
                ->ProcessWithPromiseT<RESPONSE>([&](restc_cpp::Context &ctx) {
                  // This is a co-routine, running in a worker-thread
                  // Construct a request to the server
                  restc_cpp::RequestBuilder builder(ctx);
                  builder.Get(_host + relative_path);
                  if (_use_token) {
                    builder.Header(
                        "Authorization",
                        std::string("Bearer " + _session->access_token()));
                  }
                  // Collect overrides if present
                  if (callback.has_value()) {
                    callback.value()(builder);
                  }
                  restc_cpp::SerializeFromJson(response,
                                               // Send the request
                                               builder.Execute(),
                                               &json::TypeFieldMapping);

                  // Return the list record instance through C++
                  // future<>
                  return response;
                })

                // Get the Post instance from the future<>, or any C++
                // exception thrown within the lambda.
                .get();
        REL_TRACE("GET OK for {}", relative_path);
        break;
      } catch (boost::system::system_error const &exc) {
        if (exc.code().value() == boost::asio::error::eof &&
            exc.code().category() == boost::asio::error::get_misc_category()) {
          REL_WARNING("IoReaderImpl::ReadSome(GET): asio eof, retry");
          ++retries;
        } else {
          // unrecoverable error
          REL_ERROR("GET for {} Boost exception {}", relative_path, exc.what())
          throw;
        }
      } catch (std::exception const &exc) {
        REL_ERROR("GET for {} exception {}", relative_path, exc.what())
        throw;
      }
    }
    return response;
  }

  std::string raw_post(std::string const &relative_path,
                       const std::string &&body = std::string());

  template <typename BODY, typename RESPONSE, bool use_refresh = false,
            bool no_log = false>
  RESPONSE do_post(std::string const &relative_path, BODY const &body,
                   restc_cpp::serialize_properties_t properties =
                       restc_cpp::serialize_properties_t()) {
    RESPONSE response;
    // invariant, others can  be overridden by caller
    properties.name_mapping = &json::TypeFieldMapping;
    size_t retries(0);
    while (retries < 5) {
      try {
        _session->check_refresh();
        response =
            _rest_client
                ->ProcessWithPromiseT<RESPONSE>([&](restc_cpp::Context &ctx) {
                  // This is a co-routine, running in a worker-thread
                  // Construct a request to the server
                  // Send the request
                  restc_cpp::RequestBuilder builder(ctx);
                  if (_use_token) {
                    builder.Header(
                        "Authorization",
                        std::string("Bearer " +
                                    (use_refresh ? _session->refresh_token()
                                                 : _session->access_token())));
                  }
                  std::string body_str(as_string<BODY>(body, properties));
                  if (!body_str.empty()) {
                    builder.Data(body_str);
                  }
                  if (no_log) {
                    REL_INFO("Do POST for {}, body hidden", relative_path);
                  } else {
                    REL_INFO("Do POST for {}, body '{}'", relative_path,
                             body_str);
                  }

                  // Serialize it asynchronously. The asynchronously
                  // part does not really matter here, but it may if you
                  // receive huge data structures.
                  restc_cpp::SerializeFromJson(
                      response,
                      // Send the request
                      builder.Post(_host + relative_path)
                          .Header("Content-Type", "application/json")
                          .Execute(),
                      &json::TypeFieldMapping);
                  return response;
                })

                // Get the Post instance from the future<>, or any C++
                // exception thrown within the lambda.
                .get();
        if (no_log) {
          REL_INFO("POST for {} returned OK, result hidden", relative_path);
        } else {
          REL_INFO("POST for {} returned '{}'", relative_path,
                   bsky::as_string<RESPONSE>(response));
        }
        break;
      } catch (boost::system::system_error const &exc) {
        if (exc.code().value() == boost::asio::error::eof &&
            exc.code().category() == boost::asio::error::get_misc_category()) {
          if (++retries >= 5) {
            throw;
          } else {
            REL_WARNING("IoReaderImpl::ReadSome(POST): asio eof, retry");
          }
        } else {
          // unrecoverable error
          REL_ERROR("POST for {} Boost exception {}", relative_path, exc.what())
          throw;
        }
      } catch (std::exception const &exc) {
        REL_ERROR("POST for {} exception {}", relative_path, exc.what())
        throw;
      }
    }
    return response;
  }

  std::unordered_set<bsky::profile_view_detailed>
  get_profiles(std::unordered_set<std::string> const &dids);
  bsky::profile_view_detailed get_profile(std::string const &did);

private:
  template <typename EVENT_REQUEST>
  bsky::moderation::emit_event_response
  emit_event(EVENT_REQUEST const &request) {
    restc_cpp::serialize_properties_t properties;
    properties.name_mapping = &json::TypeFieldMapping;
    properties.ignore_empty_fileds =
        0; // negateLabelsVals is mandatory but unused
    size_t retries(0);
    bsky::moderation::emit_event_response response;
    // Serialize the body only once
    std::ostringstream body;
    restc_cpp::SerializeToJson(request, body, properties);
    while (retries < 5) {
      try {
        _session->check_refresh();
        response =
            _rest_client
                ->ProcessWithPromiseT<bsky::moderation::emit_event_response>(
                    [&](restc_cpp::Context &ctx) {
                      // This is a co-routine, running in a worker-thread

                      // Serialize it asynchronously. The asynchronously
                      // part does not really matter here, but it may if you
                      // receive huge data structures.
                      restc_cpp::SerializeFromJson(
                          response,

                          // Construct a request to the server
                          restc_cpp::RequestBuilder(ctx)
                              .Post(_host + "tools.ozone.moderation.emitEvent")
                              .Header("Content-Type", "application/json")
                              .Header("Atproto-Accept-Labelers", _service_did)
                              .Header(
                                  "Atproto-Proxy",
                                  _service_did +
                                      std::string(atproto::ProxyLabelerSuffix))
                              .Header("Authorization",
                                      std::string("Bearer " +
                                                  _session->access_token()))
                              .Data(body.str())
                              // Send the request
                              .Execute());

                      // Return the session instance through C++ future<>
                      return response;
                    })

                // Get the Post instance from the future<>, or any C++
                // exception thrown within the lambda.
                .get();
        REL_INFO("emit-event {} recorded at {}, reporter "
                 "{} id={}",
                 body.str(), response.createdAt, response.createdBy,
                 response.id);
        break;
      } catch (boost::system::system_error const &exc) {
        if (exc.code().value() == boost::asio::error::eof &&
            exc.code().category() == boost::asio::error::get_misc_category()) {
          REL_WARNING("IoReaderImpl::ReadSome(emitEvent): asio eof, retry");
          ++retries;
        } else {
          // unrecoverable error
          REL_ERROR("emitEvent {} Boost exception {}", body.str(), exc.what());
          throw;
        }
      } catch (std::exception const &exc) {
        REL_ERROR("emitEvent {} exception {}", body.str(), exc.what());
        throw;
      }
    }
    return response;
  }

  std::unique_ptr<restc_cpp::RestClient> _rest_client;
  std::unique_ptr<pds_session> _session;

  std::string _handle;
  std::string _password;
  std::string _did;
  std::string _host;
  std::string _port;
  std::string _service_did;
  bool _dry_run = true;
  bool _use_token = false;
  bool _is_ready = false;
};

template <>
inline std::string as_string<bsky::empty>(bsky::empty const &,
                                          restc_cpp::serialize_properties_t) {
  return {};
}

} // namespace bsky
namespace std {
template <> struct equal_to<bsky::profile_view_detailed> {
  inline bool operator()(bsky::profile_view_detailed const &lhs,
                         bsky::profile_view_detailed const &rhs) const {
    return lhs.did == rhs.did;
  }
};
template <> struct hash<bsky::profile_view_detailed> {
  inline size_t operator()(bsky::profile_view_detailed const &profile) const {
    return std::hash<std::string>()(profile.did);
  }
};
} // namespace std
