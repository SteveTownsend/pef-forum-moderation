#ifndef __event_cache_hpp__
#define __event_cache_hpp__
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

#include "common/activity/account_events.hpp"
#include <cache.hpp>
#include <lfu_cache_policy.hpp>
#include <mutex>

namespace activity {
constexpr size_t MaxAccounts = 500000;
constexpr size_t MaxBacklog = 10000;

template <typename Key, typename Value>
using lfu_cache_t =
    typename caches::fixed_sized_cache<Key, Value, caches::LFUCachePolicy>;

class event_cache {
public:
  event_cache();
  ~event_cache() = default;

  // Callback on LFU cache eviction
  void on_erase(std::string const &did,
                caches::WrappedValue<account> const &entry);

  void record(timed_event const &value);
  caches::WrappedValue<account> get_account(std::string const &did);

private:
  // visitor for event-specific logic
  struct augment_event {
    template <typename T> void operator()(T const &) {}
  };

  // LFU cache of recently-active accounts
  std::mutex _cache_lock;
  lfu_cache_t<std::string, account> _account_events;
};
} // namespace activity

#endif