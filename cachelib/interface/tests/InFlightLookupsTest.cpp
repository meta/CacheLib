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

#include <folly/CancellationToken.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/WithCancellation.h>
#include <folly/executors/ManualExecutor.h>
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "cachelib/common/Time.h"
#include "cachelib/interface/InFlightTransfers.h"
#include "cachelib/interface/components/tests/CacheComponentFactory.h"

using namespace facebook::cachelib::interface;

TEST(InFlightLookupsTest, ConcurrentAdmissionHasOneOwner) {
  InFlightTransfers transfers;
  std::array<std::optional<InFlightTransfers::LookupRegistration>, 8> lookups;
  std::array<std::thread, 8> threads;
  std::promise<void> start;
  auto ready = start.get_future().share();
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i] = std::thread([&, i] {
      ready.wait();
      lookups[i] = transfers.joinOrCreateLookup("key");
    });
  }
  start.set_value();
  for (auto& thread : threads) {
    thread.join();
  }

  size_t owners = 0;
  size_t ownerIndex = lookups.size();
  for (size_t i = 0; i < lookups.size(); ++i) {
    ASSERT_TRUE(lookups[i]);
    if (lookups[i]->owner) {
      ++owners;
      ownerIndex = i;
    }
  }
  ASSERT_EQ(owners, 1);
  auto other = transfers.joinOrCreateLookup("other");
  EXPECT_TRUE(other.owner);
  EXPECT_TRUE(
      lookups[ownerIndex]->owner->complete(std::optional<ReadHandle>{}));

  InFlightTransfers::SharedLookupResult shared;
  for (const auto& lookup : lookups) {
    auto result = folly::coro::blockingWait(lookup->waiter.wait());
    ASSERT_NE(result, nullptr);
    ASSERT_TRUE(result->hasValue());
    EXPECT_FALSE(result->value());
    if (shared) {
      EXPECT_EQ(result, shared);
    }
    shared = std::move(result);
  }
}

TEST(InFlightLookupsTest, ErrorIsSharedAndCompletionHappensOnce) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto first = transfers.joinOrCreateLookup("key");
  auto second = transfers.joinOrCreateLookup("key");
  ASSERT_TRUE(first.owner);
  EXPECT_FALSE(second.owner);
  auto firstWait =
      folly::coro::co_withExecutor(&executor, first.waiter.wait()).start();
  auto secondWait =
      folly::coro::co_withExecutor(&executor, second.waiter.wait()).start();
  executor.drain();
  EXPECT_FALSE(firstWait.isReady());
  EXPECT_FALSE(secondWait.isReady());

  EXPECT_TRUE(first.owner->complete(
      makeError(Error::Code::FIND_FAILED, "lookup failed")));
  EXPECT_FALSE(first.owner->complete(std::optional<ReadHandle>{}));
  executor.drain();
  ASSERT_TRUE(firstWait.isReady());
  ASSERT_TRUE(secondWait.isReady());
  auto result = std::move(firstWait).get();
  EXPECT_EQ(result, std::move(secondWait).get());
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(result->hasError());
  EXPECT_EQ(result->error().code_, Error::Code::FIND_FAILED);
  EXPECT_EQ(result->error().error_, "lookup failed");
}

TEST(InFlightLookupsTest, MovingOwnershipAbandonsOnlyReplacedLookup) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto first = transfers.joinOrCreateLookup("first");
  auto second = transfers.joinOrCreateLookup("second");
  ASSERT_TRUE(first.owner);
  ASSERT_TRUE(second.owner);
  auto firstWait =
      folly::coro::co_withExecutor(&executor, first.waiter.wait()).start();
  auto secondWait =
      folly::coro::co_withExecutor(&executor, second.waiter.wait()).start();
  executor.drain();

  auto moved = std::move(*first.owner);
  EXPECT_FALSE(first.owner->complete(std::optional<ReadHandle>{}));
  first.owner.reset();
  executor.drain();
  EXPECT_FALSE(firstWait.isReady());
  EXPECT_FALSE(secondWait.isReady());

  *second.owner = std::move(moved);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(moved.complete(std::optional<ReadHandle>{}));
  executor.drain();
  EXPECT_FALSE(firstWait.isReady());
  ASSERT_TRUE(secondWait.isReady());
  auto abandonedSecond = std::move(secondWait).get();
  ASSERT_NE(abandonedSecond, nullptr);
  ASSERT_TRUE(abandonedSecond->hasValue());
  EXPECT_FALSE(abandonedSecond->value());
  EXPECT_TRUE(transfers.joinOrCreateLookup("second").owner);
  EXPECT_FALSE(transfers.joinOrCreateLookup("first").owner);

  second.owner.reset();
  executor.drain();
  ASSERT_TRUE(firstWait.isReady());
  auto abandonedFirst = std::move(firstWait).get();
  ASSERT_NE(abandonedFirst, nullptr);
  ASSERT_TRUE(abandonedFirst->hasValue());
  EXPECT_FALSE(abandonedFirst->value());
  InFlightTransfers::LookupOwner empty;
  EXPECT_FALSE(empty.complete(std::optional<ReadHandle>{}));
}

TEST(InFlightLookupsTest, WaitTaskOutlivesWaiterRegistryAndKey) {
  folly::ManualExecutor executor;
  std::optional<InFlightTransfers::LookupOwner> owner;
  auto task = [&] {
    InFlightTransfers transfers;
    auto lookup = transfers.joinOrCreateLookup(std::string(100, 'k'));
    owner = std::move(lookup.owner);
    auto copied = lookup.waiter;
    auto moved = std::move(copied);
    return moved.wait();
  }();
  ASSERT_TRUE(owner);
  auto wait = folly::coro::co_withExecutor(&executor, std::move(task)).start();
  executor.drain();
  EXPECT_FALSE(wait.isReady());
  owner.reset();
  executor.drain();
  ASSERT_TRUE(wait.isReady());
  auto result = std::move(wait).get();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(result->hasValue());
  EXPECT_FALSE(result->value());
}

TEST(InFlightLookupsTest, DestroyingOldLookupDoesNotClearReplacement) {
  InFlightTransfers transfers;
  auto old = std::make_optional(transfers.joinOrCreateLookup("key"));
  ASSERT_TRUE(old->owner);
  auto oldTask = std::make_optional(old->waiter.wait());
  EXPECT_TRUE(old->owner->complete(std::optional<ReadHandle>{}));
  auto oldResult = folly::coro::blockingWait(old->waiter.wait());

  auto current = transfers.joinOrCreateLookup("key");
  ASSERT_TRUE(current.owner);
  EXPECT_FALSE(old->owner->complete(
      makeError(Error::Code::FIND_FAILED, "already completed")));
  old.reset();
  oldTask.reset();
  oldResult.reset();
  auto joined = transfers.joinOrCreateLookup("key");
  EXPECT_FALSE(joined.owner);
  EXPECT_TRUE(current.owner->complete(
      makeError(Error::Code::FIND_FAILED, "replacement")));
  auto result = folly::coro::blockingWait(current.waiter.wait());
  EXPECT_EQ(result, folly::coro::blockingWait(joined.waiter.wait()));
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(result->hasError());
  EXPECT_EQ(result->error().error_, "replacement");
}

TEST(InFlightLookupsTest, LookupSurvivesEvictionAndPromotion) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto eviction = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(eviction);
  auto lookup = transfers.joinOrCreateLookup("key");
  ASSERT_TRUE(lookup.owner);
  auto wait =
      folly::coro::co_withExecutor(&executor, lookup.waiter.wait()).start();
  auto evictionWait =
      folly::coro::co_withExecutor(&executor, transfers.waitForTransfer("key"))
          .start();
  executor.drain();

  eviction.reset();
  executor.drain();
  ASSERT_TRUE(evictionWait.isReady());
  std::move(evictionWait).get();
  EXPECT_FALSE(wait.isReady());
  auto promotion = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(promotion);
  auto joined = transfers.joinOrCreateLookup("key");
  EXPECT_FALSE(joined.owner);
  promotion.reset();
  executor.drain();
  EXPECT_FALSE(wait.isReady());

  EXPECT_TRUE(lookup.owner->complete(std::optional<ReadHandle>{}));
  executor.drain();
  ASSERT_TRUE(wait.isReady());
  EXPECT_EQ(std::move(wait).get(),
            folly::coro::blockingWait(joined.waiter.wait()));
}

TEST(InFlightLookupsTest, PendingAndHeldMutationPreserveLookup) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  auto lookup = transfers.joinOrCreateLookup("key");
  ASSERT_TRUE(lookup.owner);
  auto wait =
      folly::coro::co_withExecutor(&executor, lookup.waiter.wait()).start();
  auto mutation =
      folly::coro::co_withExecutor(
          &executor,
          transfers.lockForOperation(
              "key", InFlightTransfers::TransferPolicy::CancelAndWait))
          .start();
  executor.drain();
  EXPECT_FALSE(mutation.isReady());
  auto pendingJoin = transfers.joinOrCreateLookup("key");
  EXPECT_FALSE(pendingJoin.owner);

  transfer.reset();
  executor.drain();
  ASSERT_TRUE(mutation.isReady());
  auto guard = std::move(mutation).get();
  EXPECT_FALSE(wait.isReady());
  auto heldJoin = transfers.joinOrCreateLookup("key");
  EXPECT_FALSE(heldJoin.owner);
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  EXPECT_TRUE(lookup.owner->complete(std::optional<ReadHandle>{}));
  executor.drain();
  ASSERT_TRUE(wait.isReady());
  auto result = std::move(wait).get();
  EXPECT_EQ(result, folly::coro::blockingWait(pendingJoin.waiter.wait()));
  EXPECT_EQ(result, folly::coro::blockingWait(heldJoin.waiter.wait()));
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  EXPECT_TRUE(transfers.joinOrCreateLookup("key").owner);
  guard = {};
  EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
}

TEST(InFlightLookupsTest, CallerCancellationDoesNotAbandonWait) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto lookup = transfers.joinOrCreateLookup("key");
  ASSERT_TRUE(lookup.owner);
  folly::CancellationSource cancellation;
  cancellation.requestCancellation();
  auto wait = folly::coro::co_withExecutor(
                  &executor,
                  folly::coro::co_withCancellation(cancellation.getToken(),
                                                   lookup.waiter.wait()))
                  .start();
  executor.drain();
  EXPECT_FALSE(wait.isReady());
  EXPECT_TRUE(lookup.owner->complete(std::optional<ReadHandle>{}));
  executor.drain();
  ASSERT_TRUE(wait.isReady());
  auto result = std::move(wait).get();
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(result->hasValue());
  EXPECT_FALSE(result->value());
}

TEST(InFlightLookupsTest, SharedHitPinsRemovedItemForLateWaiter) {
  test::RAMCacheFactory factory;
  auto component = factory.create();
  ASSERT_NE(component, nullptr);
  const std::string key = "key";
  const std::string value = "pinned value";
  {
    auto allocated = folly::coro::blockingWait(component->allocate(
        key, value.size(), facebook::cachelib::util::getCurrentTimeSec(), 0));
    ASSERT_TRUE(allocated.hasValue());
    auto descriptor = std::move(allocated).value();
    std::memcpy(descriptor.mutableData(), value.data(), value.size());
    EXPECT_TRUE(folly::coro::blockingWait(
                    component->insert(std::move(descriptor).release()))
                    .hasValue());
    auto found = folly::coro::blockingWait(component->find(key));
    ASSERT_TRUE(found.hasValue());
    ASSERT_TRUE(found.value());
    ASSERT_TRUE(found.value()->isHandleBacked());

    InFlightTransfers::SharedLookupResult retained;
    {
      InFlightTransfers transfers;
      auto first = transfers.joinOrCreateLookup(key);
      auto second = transfers.joinOrCreateLookup(key);
      ASSERT_TRUE(first.owner);
      EXPECT_FALSE(second.owner);
      EXPECT_TRUE(first.owner->complete(
          std::optional<ReadHandle>{std::move(*found.value()).release()}));
      auto firstResult = folly::coro::blockingWait(first.waiter.wait());
      auto secondResult = folly::coro::blockingWait(second.waiter.wait());
      EXPECT_EQ(firstResult, secondResult);
      firstResult.reset();

      auto removed = folly::coro::blockingWait(component->remove(key));
      ASSERT_TRUE(removed.hasValue());
      EXPECT_TRUE(removed.value());
      auto missing = folly::coro::blockingWait(component->find(key));
      ASSERT_TRUE(missing.hasValue());
      EXPECT_FALSE(missing.value());

      auto lateResult = folly::coro::blockingWait(second.waiter.wait());
      EXPECT_EQ(lateResult, secondResult);
      retained = std::move(lateResult);
    }
    ASSERT_NE(retained, nullptr);
    ASSERT_TRUE(retained->hasValue());
    ASSERT_TRUE(retained->value());
    const auto& handle = *retained->value();
    EXPECT_EQ(handle->getKey().str(), key);
    EXPECT_EQ(std::string(static_cast<const char*>(handle->getMemory()),
                          handle->getMemorySize()),
              value);
  }
  EXPECT_TRUE(component->shutdown().hasValue());
}
