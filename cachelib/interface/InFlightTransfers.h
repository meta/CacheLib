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

#pragma once

#include <folly/CancellationToken.h>
#include <folly/coro/Task.h>

#include <memory>
#include <optional>

#include "cachelib/interface/CacheItem.h"

namespace facebook::cachelib::interface {

/**
 * Registers one transfer per key and provides cooperative cancellation.
 * Callers own execution and retain the transfer token until their work ends.
 * Cancellation does not authorize cache publication or serialize mutations.
 */
class InFlightTransfers {
 private:
  struct Entry;
  struct State;

 public:
  class TransferToken {
   public:
    TransferToken() = default;
    ~TransferToken();
    TransferToken(const TransferToken&) = delete;
    TransferToken& operator=(const TransferToken&) = delete;
    TransferToken(TransferToken&&) noexcept = default;
    TransferToken& operator=(TransferToken&& other) noexcept;

    // Retaining a cancellation token does not retain the registration.
    folly::CancellationToken getCancellationToken() const noexcept;

   private:
    friend InFlightTransfers;
    explicit TransferToken(std::shared_ptr<Entry> entry) noexcept;
    void finish() noexcept;

    std::shared_ptr<Entry> entry_;
  };

  InFlightTransfers();
  ~InFlightTransfers() = default;
  InFlightTransfers(const InFlightTransfers&) = delete;
  InFlightTransfers& operator=(const InFlightTransfers&) = delete;
  InFlightTransfers(InFlightTransfers&&) = delete;
  InFlightTransfers& operator=(InFlightTransfers&&) = delete;

  // Returns nullopt while another transfer owns this key, even if cancelled.
  // Destroying the owning token releases the registration without cancelling.
  // Tokens may outlive this registry.
  std::optional<TransferToken> tryAcquireTransferToken(Key key);

  // Requests cancellation of the observed transfer, if any. Callbacks may run
  // inline and re-enter this registry. This does not block new registrations
  // after the owning token is released.
  void cancel(Key key);

  // Snapshots the current transfer and requests cancellation immediately.
  // Await the returned task for owning-token release after execution/cleanup.
  // The task can outlive this registry and ignores caller cancellation.
  // It only waits for the observed transfer; once that transfer releases its
  // registration, tryAcquireTransferToken() can register a new one.
  folly::coro::Task<void> cancelAndWait(Key key);

 private:
  std::shared_ptr<Entry> findEntry(Key key) const;
  static folly::coro::Task<void> waitForCompletion(
      std::shared_ptr<Entry> entry);

  std::shared_ptr<State> state_;
};

} // namespace facebook::cachelib::interface
