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
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
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
  ~Entry() {
    state_->entries_.erase_key_if(key_, [this](const auto& registered) {
      return !registered.owner_before(self) && !self.owner_before(registered);
    });
  }
  Entry(const Entry&) = delete;
  Entry& operator=(const Entry&) = delete;
  Entry(Entry&&) = delete;
  Entry& operator=(Entry&&) = delete;

  std::shared_ptr<State> state_;
  std::string key_;
  std::weak_ptr<Entry> self;
  std::mutex mutex;
  std::weak_ptr<Registration> current;
  std::weak_ptr<GetCtx> lookup;
};

struct InFlightTransfers::Registration {
  explicit Registration(std::shared_ptr<Entry> entryValue,
                        folly::CancellationSource cancellation =
                            folly::CancellationSource::invalid())
      : entry(std::move(entryValue)), cancellation_(std::move(cancellation)) {}
  ~Registration() = default;
  Registration(const Registration&) = delete;
  Registration& operator=(const Registration&) = delete;
  Registration(Registration&&) = delete;
  Registration& operator=(Registration&&) = delete;

  std::shared_ptr<Entry> entry;
  // Live only for transfers; key operation guards never expose a cancellation
  // token.
  folly::CancellationSource cancellation_;
  folly::coro::Baton finished_;
  std::shared_ptr<Registration> next_;

  bool isTransfer() const noexcept { return cancellation_.canBeCancelled(); }
};

struct InFlightTransfers::GetCtx {
  explicit GetCtx(std::shared_ptr<Entry> entryValue)
      : entry(std::move(entryValue)) {}
  GetCtx(const GetCtx&) = delete;
  GetCtx& operator=(const GetCtx&) = delete;
  GetCtx(GetCtx&&) = delete;
  GetCtx& operator=(GetCtx&&) = delete;

  std::shared_ptr<Entry> entry;
  std::optional<LookupResult> result{std::in_place, std::nullopt};
  folly::coro::Baton finished_;
};

InFlightTransfers::InFlightTransfers() : state_(std::make_shared<State>()) {}

std::shared_ptr<InFlightTransfers::Entry> InFlightTransfers::getOrCreateEntry(
    std::shared_ptr<State> state, std::string key) {
  auto it = state->entries_.find(std::string_view{key});
  if (it != state->entries_.end()) {
    if (auto entry = it->second.lock()) {
      return entry;
    }
  }
  auto entry = std::make_shared<Entry>(state, std::move(key));
  entry->self = entry;
  // Keep the existing entry alive until after the map lock is released.
  std::shared_ptr<Entry> existing;
  state->entries_.insert_or_assign_if(entry->key_, std::weak_ptr<Entry>{entry},
                                      [&](const auto& current) {
                                        existing = current.lock();
                                        return !existing;
                                      });
  return existing ? std::move(existing) : std::move(entry);
}

InFlightTransfers::LookupWaiter::LookupWaiter(
    std::shared_ptr<GetCtx> context) noexcept
    : context_(std::move(context)) {}

folly::coro::Task<InFlightTransfers::SharedLookupResult>
InFlightTransfers::LookupWaiter::wait() const {
  return waitForLookup(context_);
}

folly::coro::Task<InFlightTransfers::SharedLookupResult>
InFlightTransfers::waitForLookup(std::shared_ptr<GetCtx> context) {
  co_await context->finished_;
  co_return SharedLookupResult{context, &*context->result};
}

InFlightTransfers::LookupOwner::LookupOwner(
    std::shared_ptr<GetCtx> context) noexcept
    : context_(std::move(context)) {}

InFlightTransfers::LookupOwner::~LookupOwner() { finish(); }

InFlightTransfers::LookupOwner& InFlightTransfers::LookupOwner::operator=(
    LookupOwner&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  LookupOwner previous{std::move(*this)};
  context_ = std::move(other.context_);
  return *this;
}

bool InFlightTransfers::LookupOwner::complete(LookupResult result) noexcept {
  static_assert(std::is_nothrow_move_constructible_v<LookupResult>);
  if (!context_) {
    return false;
  }
  context_->result.emplace(std::move(result));
  finish();
  return true;
}

void InFlightTransfers::LookupOwner::finish() noexcept {
  auto context = std::move(context_);
  if (!context) {
    return;
  }
  auto& entry = *context->entry;
  {
    std::lock_guard lock{entry.mutex};
    if (!entry.lookup.owner_before(context) &&
        !context.owner_before(entry.lookup)) {
      entry.lookup.reset();
    }
  }
  context->finished_.post();
}

InFlightTransfers::LookupRegistration InFlightTransfers::joinOrCreateLookup(
    Key key) {
  auto entry = getOrCreateEntry(state_, std::string{key.data(), key.size()});
  std::shared_ptr<GetCtx> context;
  std::optional<LookupOwner> owner;
  {
    std::lock_guard lock{entry->mutex};
    context = entry->lookup.lock();
    if (!context) {
      context = std::make_shared<GetCtx>(entry);
      owner.emplace(LookupOwner{context});
      entry->lookup = context;
    }
  }
  return {LookupWaiter{std::move(context)}, std::move(owner)};
}

InFlightTransfers::RegistrationGuard::RegistrationGuard(
    std::shared_ptr<Registration> node) noexcept
    : node_(std::move(node)) {}

InFlightTransfers::RegistrationGuard&
InFlightTransfers::RegistrationGuard::operator=(
    RegistrationGuard&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  RegistrationGuard previous{std::move(*this)};
  node_ = std::move(other.node_);
  return *this;
}

InFlightTransfers::RegistrationGuard::~RegistrationGuard() {
  auto node = std::move(node_);
  if (!node) {
    return;
  }
  auto& entry = *node->entry;
  {
    std::lock_guard lock{entry.mutex};
    if (!entry.current.owner_before(node) &&
        !node.owner_before(entry.current)) {
      entry.current.reset();
    }
  }
  // Keep the baton alive across inline waiter resumption, outside locks.
  node->finished_.post();
}

InFlightTransfers::TransferToken::TransferToken(
    std::shared_ptr<Registration> node) noexcept
    : registration_(std::move(node)) {}

InFlightTransfers::KeyOperationGuard::KeyOperationGuard(
    std::shared_ptr<Registration> node) noexcept
    : registration_(std::move(node)) {}

folly::CancellationToken
InFlightTransfers::TransferToken::getCancellationToken() const noexcept {
  const auto& node = registration_.node_;
  return node ? node->cancellation_.getToken() : folly::CancellationToken{};
}

std::optional<InFlightTransfers::TransferToken>
InFlightTransfers::tryAcquireTransferToken(Key key) {
  auto entry = getOrCreateEntry(state_, std::string{key.data(), key.size()});
  auto node =
      std::make_shared<Registration>(entry, folly::CancellationSource{});
  {
    std::lock_guard lock{entry->mutex};
    if (!entry->current.expired()) {
      return std::nullopt;
    }
    entry->current = node;
  }
  return TransferToken{std::move(node)};
}

std::shared_ptr<InFlightTransfers::Registration>
InFlightTransfers::findRegistration(Key key) const {
  auto it = state_->entries_.find(std::string_view{key.data(), key.size()});
  if (it == state_->entries_.end()) {
    return nullptr;
  }
  auto entry = it->second.lock();
  if (!entry) {
    return nullptr;
  }
  std::shared_ptr<Registration> current;
  {
    std::lock_guard lock{entry->mutex};
    current = entry->current.lock();
  }
  return current;
}

void InFlightTransfers::cancel(Key key) {
  if (auto node = findRegistration(key); node && node->isTransfer()) {
    node->cancellation_.requestCancellation();
  }
}

folly::coro::Task<void> InFlightTransfers::cancelAndWait(Key key) {
  auto node = findRegistration(key);
  if (node && node->isTransfer()) {
    node->cancellation_.requestCancellation();
  }
  return waitForCompletion(std::move(node));
}

folly::coro::Task<void> InFlightTransfers::waitForTransfer(Key key) {
  auto node = findRegistration(key);
  if (node && !node->cancellation_.canBeCancelled()) {
    node.reset();
  }
  return waitForCompletion(std::move(node));
}

folly::coro::Task<void> InFlightTransfers::waitForCompletion(
    std::shared_ptr<Registration> node) {
  if (node) {
    co_await node->finished_;
  }
}

folly::coro::Task<InFlightTransfers::KeyOperationGuard>
InFlightTransfers::lockForOperation(Key key, TransferPolicy policy) {
  return lockForOperationImpl(state_, std::string{key.data(), key.size()},
                              policy);
}

folly::coro::Task<InFlightTransfers::KeyOperationGuard>
InFlightTransfers::lockForOperationImpl(std::shared_ptr<State> state,
                                        std::string key,
                                        TransferPolicy policy) {
  auto entry = getOrCreateEntry(std::move(state), std::move(key));
  auto node = std::make_shared<Registration>(entry);
  KeyOperationGuard guard{node};
  {
    std::lock_guard lock{entry->mutex};
    node->next_ = entry->current.lock();
    entry->current = node;
  }
  if (const auto& previous = node->next_) {
    if (policy == TransferPolicy::CancelAndWait && previous->isTransfer()) {
      previous->cancellation_.requestCancellation();
    }
    co_await previous->finished_;
    node->next_.reset();
  }
  co_return std::move(guard);
}

} // namespace facebook::cachelib::interface
