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
#include <string>

#include "cachelib/interface/CacheItem.h"

namespace facebook::cachelib::interface {

/**
 * Registers one transfer per key and provides cooperative cancellation.
 * Callers own execution and retain the transfer token until their work ends.
 * A key operation guard waits for the key's transfer and earlier guards,
 * optionally requests transfer cancellation, and keeps new transfers out
 * until it is released.
 */
class InFlightTransfers {
 private:
  struct Registration;
  struct State;

  struct RegistrationGuard {
    RegistrationGuard() = default;
    explicit RegistrationGuard(std::shared_ptr<Registration> node) noexcept;
    ~RegistrationGuard();
    RegistrationGuard(const RegistrationGuard&) = delete;
    RegistrationGuard& operator=(const RegistrationGuard&) = delete;
    RegistrationGuard(RegistrationGuard&&) noexcept = default;
    RegistrationGuard& operator=(RegistrationGuard&& other) noexcept;

    std::shared_ptr<Registration> node_;
  };

 public:
  enum class TransferPolicy { Wait, CancelAndWait };

  class TransferToken {
   public:
    TransferToken() = default;
    ~TransferToken() = default;
    TransferToken(const TransferToken&) = delete;
    TransferToken& operator=(const TransferToken&) = delete;
    TransferToken(TransferToken&&) noexcept = default;
    TransferToken& operator=(TransferToken&&) noexcept = default;

    // Retaining a cancellation token does not retain the registration.
    folly::CancellationToken getCancellationToken() const noexcept;

   private:
    friend InFlightTransfers;
    explicit TransferToken(std::shared_ptr<Registration> node) noexcept;

    RegistrationGuard registration_;
  };

  class KeyOperationGuard {
   public:
    KeyOperationGuard() = default;
    ~KeyOperationGuard() = default;
    KeyOperationGuard(const KeyOperationGuard&) = delete;
    KeyOperationGuard& operator=(const KeyOperationGuard&) = delete;
    KeyOperationGuard(KeyOperationGuard&&) noexcept = default;
    KeyOperationGuard& operator=(KeyOperationGuard&&) noexcept = default;

   private:
    friend InFlightTransfers;
    explicit KeyOperationGuard(std::shared_ptr<Registration> node) noexcept;

    RegistrationGuard registration_;
  };

  InFlightTransfers();
  ~InFlightTransfers() = default;
  InFlightTransfers(const InFlightTransfers&) = delete;
  InFlightTransfers& operator=(const InFlightTransfers&) = delete;
  InFlightTransfers(InFlightTransfers&&) = delete;
  InFlightTransfers& operator=(InFlightTransfers&&) = delete;

  // Returns nullopt while a transfer is registered or a key operation guard is
  // pending or held for this key. Destroying the owning token releases the
  // registration without cancelling. Tokens may outlive this registry.
  std::optional<TransferToken> tryAcquireTransferToken(Key key);

  // Requests cancellation only if the current tail is a transfer. Does not
  // cancel through queued guards or block new registrations. Callbacks may run
  // inline and re-enter this registry.
  void cancel(Key key);

  // Snapshots the current tail and cancels it only if it is a transfer.
  // Await for that token/guard's release, including all earlier work. Does not
  // reserve the key or wait for later registrations. Do not await while holding
  // work that this tail depends on. Caller cancellation is ignored; the task
  // may outlive this registry.
  folly::coro::Task<void> cancelAndWait(Key key);

  // Snapshots the current tail only if it is a transfer, without cancelling.
  // A queued guard hides earlier transfers and is not waited for. The task
  // does not reserve the key, ignores caller cancellation, and may outlive
  // this registry. Recheck cache state after waiting.
  folly::coro::Task<void> waitForTransfer(Key key);

  // When awaited, reserves the key and waits for its transfer and earlier
  // guards in registration order. New transfers stay blocked until release.
  // CancelAndWait requests cancellation only when the immediate predecessor
  // is a transfer; it does not cancel through queued guards. Wait never
  // cancels.
  //
  // Hold Wait through insert's presence check and insertion. Waiting behind a
  // transfer is conservative: the item already exists, so insert would fail
  // once the transfer publishes, and callers may fail early instead. Waiting
  // is required behind a removal or a transfer it cancels. For findToWrite,
  // find/promote and pin first, then hold CancelAndWait while acquiring the
  // write handle. Hold CancelAndWait through insertOrReplace/remove. Recheck
  // component state under the guard.
  //
  // Do not await while holding this key's guard or transfer token, or locks
  // needed by earlier work. Caller cancellation is ignored; the task and guard
  // may outlive the registry.
  folly::coro::Task<KeyOperationGuard> lockForOperation(Key key,
                                                        TransferPolicy policy);

 private:
  std::shared_ptr<Registration> findRegistration(Key key) const;
  static folly::coro::Task<void> waitForCompletion(
      std::shared_ptr<Registration> node);
  static folly::coro::Task<KeyOperationGuard> lockForOperationImpl(
      std::shared_ptr<State> state, std::string key, TransferPolicy policy);

  std::shared_ptr<State> state_;
};

} // namespace facebook::cachelib::interface
