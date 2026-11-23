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
#include "cachelib/interface/Handle.h"
#include "cachelib/interface/Result.h"

namespace facebook::cachelib::interface {

/**
 * Registers one transfer per key and provides cooperative cancellation.
 * Callers own execution and retain the transfer token until their work ends.
 * A key operation guard waits for the key's transfer and earlier guards,
 * optionally requests transfer cancellation, and keeps new transfers out
 * until it is released.
 * Lookups coalesce independently of transfer and mutation reservations.
 */
class InFlightTransfers {
 private:
  struct Entry;
  struct GetCtx;
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

  using LookupResult = Result<std::optional<ReadHandle>>;
  using SharedLookupResult = std::shared_ptr<const LookupResult>;

  class LookupWaiter {
   public:
    ~LookupWaiter() = default;
    LookupWaiter(const LookupWaiter&) = default;
    LookupWaiter& operator=(const LookupWaiter&) = default;
    LookupWaiter(LookupWaiter&&) noexcept = default;
    LookupWaiter& operator=(LookupWaiter&&) noexcept = default;

    // Waits for this lookup to finish with a shared hit, miss, or error.
    // Caller cancellation does not stop the wait. The returned task can outlive
    // the waiter and registry, and retaining a hit result keeps the item
    // pinned. Do not call on a moved-from waiter.
    folly::coro::Task<SharedLookupResult> wait() const;

   private:
    friend InFlightTransfers;
    explicit LookupWaiter(std::shared_ptr<GetCtx> context) noexcept;

    std::shared_ptr<GetCtx> context_;
  };

  class LookupOwner {
   public:
    LookupOwner() = default;
    ~LookupOwner();
    LookupOwner(const LookupOwner&) = delete;
    LookupOwner& operator=(const LookupOwner&) = delete;
    LookupOwner(LookupOwner&&) noexcept = default;
    LookupOwner& operator=(LookupOwner&& other) noexcept;

    // Publishes a hit, miss, or error to all waiters and lets a new lookup
    // start for this key. Returns false if this owner is empty, moved from,
    // or already completed.
    // A hit must contain an ingress handle for this key. Its component must
    // outlive every waiter, task, and shared result retaining that handle.
    // Destruction or replacement of an unfinished owner completes with a miss.
    bool complete(LookupResult result) noexcept;

   private:
    friend InFlightTransfers;
    explicit LookupOwner(std::shared_ptr<GetCtx> context) noexcept;
    void finish() noexcept;

    std::shared_ptr<GetCtx> context_;
  };

  struct LookupRegistration {
    LookupWaiter waiter;
    std::optional<LookupOwner> owner;
  };

  InFlightTransfers();
  ~InFlightTransfers() = default;
  InFlightTransfers(const InFlightTransfers&) = delete;
  InFlightTransfers& operator=(const InFlightTransfers&) = delete;
  InFlightTransfers(InFlightTransfers&&) = delete;
  InFlightTransfers& operator=(InFlightTransfers&&) = delete;

  // Joins or creates the lookup synchronously. Only its first caller receives
  // an owner; all waiters observe the same shared result. Completing the owner
  // unregisters the lookup before waking waiters, allowing a new lookup to
  // start. Registration does not reserve the key or validate cache state:
  // callers must coordinate publication and mutations. Transfers complete
  // independently.
  LookupRegistration joinOrCreateLookup(Key key);

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
  static std::shared_ptr<Entry> getOrCreateEntry(std::shared_ptr<State> state,
                                                 std::string key);
  std::shared_ptr<Registration> findRegistration(Key key) const;
  static folly::coro::Task<void> waitForCompletion(
      std::shared_ptr<Registration> node);
  static folly::coro::Task<KeyOperationGuard> lockForOperationImpl(
      std::shared_ptr<State> state, std::string key, TransferPolicy policy);

  static folly::coro::Task<SharedLookupResult> waitForLookup(
      std::shared_ptr<GetCtx> context);

  std::shared_ptr<State> state_;
};

} // namespace facebook::cachelib::interface
