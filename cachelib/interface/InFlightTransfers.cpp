/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cachelib/interface/InFlightTransfers.h"

#include <folly/Utility.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/coro/Baton.h>

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace facebook::cachelib::interface {

struct InFlightTransfers::State {
  folly::ConcurrentHashMapSIMD<
      std::string,
      std::weak_ptr<Entry>,
      folly::transparent<std::hash<std::string_view>>,
      folly::transparent<std::equal_to<std::string_view>>>
      entries_;
};

struct InFlightTransfers::Entry {
  Entry(std::shared_ptr<State> state, std::string key)
      : state_(std::move(state)), key_(std::move(key)) {}
  ~Entry() = default;
  Entry(const Entry&) = delete;
  Entry& operator=(const Entry&) = delete;
  Entry(Entry&&) = delete;
  Entry& operator=(Entry&&) = delete;

  std::shared_ptr<State> state_;
  std::string key_;
  folly::CancellationSource cancellation_;
  folly::coro::Baton finished_;
};

InFlightTransfers::InFlightTransfers() : state_(std::make_shared<State>()) {}

InFlightTransfers::TransferToken::TransferToken(
    std::shared_ptr<Entry> entry) noexcept
    : entry_(std::move(entry)) {}

InFlightTransfers::TransferToken::~TransferToken() { finish(); }

InFlightTransfers::TransferToken& InFlightTransfers::TransferToken::operator=(
    TransferToken&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  TransferToken previous{std::move(*this)};
  entry_ = std::move(other.entry_);
  return *this;
}

void InFlightTransfers::TransferToken::finish() noexcept {
  auto entry = std::move(entry_);
  if (!entry) {
    return;
  }
  entry->state_->entries_.erase_key_if(
      entry->key_, [&entry](const auto& current) {
        return !current.owner_before(entry) && !entry.owner_before(current);
      });
  // Keep the baton alive across inline waiter resumption, outside map locks.
  entry->finished_.post();
}

folly::CancellationToken
InFlightTransfers::TransferToken::getCancellationToken() const noexcept {
  return entry_ ? entry_->cancellation_.getToken() : folly::CancellationToken{};
}

std::optional<InFlightTransfers::TransferToken>
InFlightTransfers::tryAcquireTransferToken(Key key) {
  auto entry =
      std::make_shared<Entry>(state_, std::string{key.data(), key.size()});
  auto [it, inserted] = state_->entries_.insert_or_assign_if(
      entry->key_, std::weak_ptr<Entry>{entry},
      [](const auto& current) { return current.expired(); });
  if (!inserted) {
    return std::nullopt;
  }
  return TransferToken{std::move(entry)};
}

std::shared_ptr<InFlightTransfers::Entry> InFlightTransfers::findEntry(
    Key key) const {
  auto it = state_->entries_.find(std::string_view{key.data(), key.size()});
  return it == state_->entries_.end() ? nullptr : it->second.lock();
}

void InFlightTransfers::cancel(Key key) {
  if (auto entry = findEntry(key)) {
    entry->cancellation_.requestCancellation();
  }
}

folly::coro::Task<void> InFlightTransfers::cancelAndWait(Key key) {
  auto entry = findEntry(key);
  if (entry) {
    entry->cancellation_.requestCancellation();
  }
  return waitForCompletion(std::move(entry));
}

folly::coro::Task<void> InFlightTransfers::waitForCompletion(
    std::shared_ptr<Entry> entry) {
  if (entry) {
    co_await entry->finished_;
  }
}

} // namespace facebook::cachelib::interface
