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
      std::weak_ptr<Registration>,
      folly::transparent<std::hash<std::string_view>>,
      folly::transparent<std::equal_to<std::string_view>>>
      tails_;
};

struct InFlightTransfers::Registration {
  Registration(std::shared_ptr<State> state,
               std::string key,
               folly::CancellationSource cancellation =
                   folly::CancellationSource::invalid())
      : state_(std::move(state)),
        key_(std::move(key)),
        cancellation_(std::move(cancellation)) {}
  ~Registration() = default;
  Registration(const Registration&) = delete;
  Registration& operator=(const Registration&) = delete;
  Registration(Registration&&) = delete;
  Registration& operator=(Registration&&) = delete;

  std::shared_ptr<State> state_;
  std::string key_;
  // Live only for transfers; key operation guards never expose a cancellation
  // token.
  folly::CancellationSource cancellation_;
  folly::coro::Baton finished_;
  std::shared_ptr<Registration> next_;

  bool isTransfer() const noexcept { return cancellation_.canBeCancelled(); }
};

InFlightTransfers::InFlightTransfers() : state_(std::make_shared<State>()) {}

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
  node->state_->tails_.erase_key_if(node->key_, [&node](const auto& current) {
    return !current.owner_before(node) && !node.owner_before(current);
  });
  // Keep the baton alive across inline waiter resumption, outside map locks.
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
  auto node = std::make_shared<Registration>(
      state_, std::string{key.data(), key.size()}, folly::CancellationSource{});
  auto [it, inserted] = state_->tails_.insert_or_assign_if(
      node->key_, std::weak_ptr<Registration>{node},
      [](const auto& current) { return current.expired(); });
  if (!inserted) {
    return std::nullopt;
  }
  return TransferToken{std::move(node)};
}

std::shared_ptr<InFlightTransfers::Registration>
InFlightTransfers::findRegistration(Key key) const {
  auto it = state_->tails_.find(std::string_view{key.data(), key.size()});
  if (it != state_->tails_.end()) {
    return it->second.lock();
  }
  return nullptr;
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
  auto node = std::make_shared<Registration>(std::move(state), std::move(key));
  KeyOperationGuard guard{node};
  node->state_->tails_.insert_or_assign_if(
      node->key_, std::weak_ptr<Registration>{node}, [&](const auto& current) {
        node->next_ = current.lock();
        return true;
      });
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
