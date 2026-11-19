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
#include <folly/coro/Collect.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/coro/WithCancellation.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/ManualExecutor.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cachelib/interface/InFlightTransfers.h"

using namespace facebook::cachelib::interface;

TEST(InFlightTransfersTest, OneTransferPerKey) {
  InFlightTransfers transfers;
  auto first = transfers.tryAcquireTransferToken("first");
  auto second = transfers.tryAcquireTransferToken("second");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_FALSE(transfers.tryAcquireTransferToken("first"));
  EXPECT_FALSE(transfers.tryAcquireTransferToken("second"));
  EXPECT_TRUE(first->getCancellationToken().canBeCancelled());
  EXPECT_FALSE(first->getCancellationToken().isCancellationRequested());
}

TEST(InFlightTransfersTest, CancellationKeepsRegistrationUntilRelease) {
  InFlightTransfers transfers;
  auto first = transfers.tryAcquireTransferToken("first");
  auto second = transfers.tryAcquireTransferToken("second");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  int callbacks = 0;
  folly::CancellationCallback callback{first->getCancellationToken(),
                                       [&] { ++callbacks; }};
  transfers.cancel("missing");
  transfers.cancel("first");
  transfers.cancel("first");
  EXPECT_EQ(callbacks, 1);
  EXPECT_TRUE(first->getCancellationToken().isCancellationRequested());
  EXPECT_FALSE(second->getCancellationToken().isCancellationRequested());
  EXPECT_FALSE(transfers.tryAcquireTransferToken("first"));
  first.reset();
  auto replacement = transfers.tryAcquireTransferToken("first");
  ASSERT_TRUE(replacement);
  EXPECT_FALSE(replacement->getCancellationToken().isCancellationRequested());
}

TEST(InFlightTransfersTest, ReleaseDoesNotCancelOrRetainRegistration) {
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  auto cancellation = transfer->getCancellationToken();
  transfer.reset();
  EXPECT_FALSE(cancellation.isCancellationRequested());
  EXPECT_FALSE(cancellation.canBeCancelled());
  EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
  InFlightTransfers::TransferToken empty;
  EXPECT_FALSE(empty.getCancellationToken().canBeCancelled());
}

TEST(InFlightTransfersTest, MovesTransferOwnershipAndReleasesPreviousKey) {
  InFlightTransfers transfers;
  auto first = transfers.tryAcquireTransferToken("first");
  auto second = transfers.tryAcquireTransferToken("second");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  auto moved = std::move(*first);
  EXPECT_FALSE(first->getCancellationToken().canBeCancelled());
  first.reset();
  EXPECT_FALSE(transfers.tryAcquireTransferToken("first"));
  auto previous = second->getCancellationToken();
  *second = std::move(moved);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(moved.getCancellationToken().canBeCancelled());
  EXPECT_FALSE(previous.canBeCancelled());
  EXPECT_TRUE(transfers.tryAcquireTransferToken("second"));
  EXPECT_FALSE(transfers.tryAcquireTransferToken("first"));
  transfers.cancel("first");
  EXPECT_TRUE(second->getCancellationToken().isCancellationRequested());
  second.reset();
  EXPECT_TRUE(transfers.tryAcquireTransferToken("first"));
}

TEST(InFlightTransfersTest, TransferOutlivesRegistry) {
  auto transfers = std::make_unique<InFlightTransfers>();
  auto transfer = transfers->tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  auto cancellation = transfer->getCancellationToken();
  transfers.reset();
  EXPECT_TRUE(cancellation.canBeCancelled());
  EXPECT_FALSE(cancellation.isCancellationRequested());
  transfer.reset();
  EXPECT_FALSE(cancellation.canBeCancelled());
}

TEST(InFlightTransfersTest, CancellationCallbackCanReplaceTransfer) {
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  auto cancellation = transfer->getCancellationToken();
  std::optional<InFlightTransfers::TransferToken> replacement;
  folly::CancellationCallback callback{
      cancellation, [&] {
        transfer.reset();
        replacement = transfers.tryAcquireTransferToken("key");
      }};
  transfers.cancel("key");
  EXPECT_TRUE(cancellation.isCancellationRequested());
  ASSERT_TRUE(replacement);
  EXPECT_FALSE(replacement->getCancellationToken().isCancellationRequested());
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  replacement.reset();
  EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
}

TEST(InFlightTransfersTest, ConcurrentAcquisitionHasOneWinner) {
  InFlightTransfers transfers;
  std::array<std::optional<InFlightTransfers::TransferToken>, 8> results;
  std::array<std::thread, 8> threads;
  std::promise<void> start;
  auto ready = start.get_future().share();
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i] = std::thread([&, i] {
      ready.wait();
      results[i] = transfers.tryAcquireTransferToken("key");
    });
  }
  start.set_value();
  for (auto& thread : threads) {
    thread.join();
  }
  size_t winners = 0;
  for (auto& result : results) {
    winners += result.has_value();
  }
  EXPECT_EQ(winners, 1);
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  for (auto& result : results) {
    result.reset();
  }
  EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
}

TEST(InFlightTransfersTest, WaitForTransferDoesNotCancelMultipleWaiters) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  auto cancellation = transfer->getCancellationToken();
  auto first =
      folly::coro::co_withExecutor(&executor, transfers.waitForTransfer("key"))
          .start();
  auto second =
      folly::coro::co_withExecutor(&executor, transfers.waitForTransfer("key"))
          .start();
  EXPECT_FALSE(cancellation.isCancellationRequested());
  executor.drain();
  EXPECT_FALSE(cancellation.isCancellationRequested());
  EXPECT_FALSE(first.isReady());
  EXPECT_FALSE(second.isReady());
  transfer.reset();
  executor.drain();
  ASSERT_TRUE(first.isReady());
  ASSERT_TRUE(second.isReady());
  std::move(first).get();
  std::move(second).get();
  EXPECT_FALSE(cancellation.isCancellationRequested());
  EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
}

TEST(InFlightTransfersTest, WaitForTransferSnapshotsBeforeAwait) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  auto task = transfers.waitForTransfer("key");
  EXPECT_FALSE(transfer->getCancellationToken().isCancellationRequested());
  transfer.reset();
  auto replacement = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(replacement);
  auto wait = folly::coro::co_withExecutor(&executor, std::move(task)).start();
  executor.drain();
  ASSERT_TRUE(wait.isReady());
  std::move(wait).get();
  EXPECT_FALSE(replacement->getCancellationToken().isCancellationRequested());
}

TEST(InFlightTransfersTest, WaitForAbsentTransferIgnoresLaterRegistration) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto missing = transfers.waitForTransfer("missing");
  auto transfer = transfers.tryAcquireTransferToken("released");
  ASSERT_TRUE(transfer);
  transfer.reset();
  auto released = transfers.waitForTransfer("released");
  auto first = transfers.tryAcquireTransferToken("missing");
  auto second = transfers.tryAcquireTransferToken("released");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  auto firstWait =
      folly::coro::co_withExecutor(&executor, std::move(missing)).start();
  auto secondWait =
      folly::coro::co_withExecutor(&executor, std::move(released)).start();
  executor.drain();
  ASSERT_TRUE(firstWait.isReady());
  ASSERT_TRUE(secondWait.isReady());
  std::move(firstWait).get();
  std::move(secondWait).get();
  EXPECT_FALSE(first->getCancellationToken().isCancellationRequested());
  EXPECT_FALSE(second->getCancellationToken().isCancellationRequested());
}

TEST(InFlightTransfersTest, WaitForTransferOutlivesRegistryAndKey) {
  folly::ManualExecutor executor;
  auto transfers = std::make_unique<InFlightTransfers>();
  auto transfer = transfers->tryAcquireTransferToken(std::string(100, 'k'));
  ASSERT_TRUE(transfer);
  auto task = transfers->waitForTransfer(std::string(100, 'k'));
  transfers.reset();
  auto wait = folly::coro::co_withExecutor(&executor, std::move(task)).start();
  executor.drain();
  EXPECT_FALSE(transfer->getCancellationToken().isCancellationRequested());
  EXPECT_FALSE(wait.isReady());
  transfer.reset();
  executor.drain();
  ASSERT_TRUE(wait.isReady());
  std::move(wait).get();
}

CO_TEST(InFlightTransfersTest, WaitForMissingOrReleasedTransferCompletes) {
  InFlightTransfers transfers;
  co_await transfers.cancelAndWait("missing");
  auto transfer = transfers.tryAcquireTransferToken("key");
  CO_ASSERT_TRUE(transfer);
  transfer.reset();
  co_await transfers.cancelAndWait("key");
}

TEST(InFlightTransfersTest, MultipleWaitersResumeWhenOwnerReleases) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  auto first =
      folly::coro::co_withExecutor(&executor, transfers.cancelAndWait("key"))
          .start();
  auto second =
      folly::coro::co_withExecutor(&executor, transfers.cancelAndWait("key"))
          .start();
  executor.drain();
  EXPECT_TRUE(transfer->getCancellationToken().isCancellationRequested());
  EXPECT_FALSE(first.isReady());
  EXPECT_FALSE(second.isReady());
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  transfer.reset();
  executor.drain();
  ASSERT_TRUE(first.isReady());
  ASSERT_TRUE(second.isReady());
  std::move(first).get();
  std::move(second).get();
  EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
}

TEST(InFlightTransfersTest, MoveAssignmentCompletesOnlyReplacedTransfer) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto first = transfers.tryAcquireTransferToken("first");
  auto second = transfers.tryAcquireTransferToken("second");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  auto firstWait =
      folly::coro::co_withExecutor(&executor, transfers.cancelAndWait("first"))
          .start();
  auto secondWait =
      folly::coro::co_withExecutor(&executor, transfers.cancelAndWait("second"))
          .start();
  executor.drain();
  auto moved = std::move(*second);
  second.reset();
  executor.drain();
  EXPECT_FALSE(secondWait.isReady());
  *first = std::move(moved);
  executor.drain();
  ASSERT_TRUE(firstWait.isReady());
  std::move(firstWait).get();
  EXPECT_FALSE(secondWait.isReady());
  EXPECT_TRUE(transfers.tryAcquireTransferToken("first"));
  EXPECT_FALSE(transfers.tryAcquireTransferToken("second"));
  first.reset();
  executor.drain();
  ASSERT_TRUE(secondWait.isReady());
  std::move(secondWait).get();
}

CO_TEST(InFlightTransfersTest, DeferredWaitDoesNotRetainOrCancelReplacement) {
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  CO_ASSERT_TRUE(transfer);
  auto wait = transfers.cancelAndWait("key");
  EXPECT_TRUE(transfer->getCancellationToken().isCancellationRequested());
  transfer.reset();
  auto replacement = transfers.tryAcquireTransferToken("key");
  CO_ASSERT_TRUE(replacement);
  co_await std::move(wait);
  EXPECT_FALSE(replacement->getCancellationToken().isCancellationRequested());
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
}

CO_TEST(InFlightTransfersTest, WaitSurvivesRegistryAndKeyDestruction) {
  auto transfers = std::make_unique<InFlightTransfers>();
  auto transfer = transfers->tryAcquireTransferToken("key");
  CO_ASSERT_TRUE(transfer);
  auto wait = transfers->cancelAndWait(std::string{"key"});
  transfers.reset();
  transfer.reset();
  co_await std::move(wait);
}

CO_TEST(InFlightTransfersTest, CancellationCallbackCanFinishBeforeWaitStarts) {
  auto transfers = std::make_unique<InFlightTransfers>();
  auto transfer = transfers->tryAcquireTransferToken("key");
  CO_ASSERT_TRUE(transfer);
  folly::CancellationCallback callback{transfer->getCancellationToken(), [&] {
                                         transfer.reset();
                                         transfers.reset();
                                       }};
  auto wait = transfers->cancelAndWait("key");
  EXPECT_EQ(transfers, nullptr);
  co_await std::move(wait);
}

TEST(InFlightTransfersTest, CallerCancellationDoesNotAbandonCompletionWait) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  folly::CancellationSource callerCancellation;
  callerCancellation.requestCancellation();
  auto wait = folly::coro::co_withExecutor(&executor,
                                           folly::coro::co_withCancellation(
                                               callerCancellation.getToken(),
                                               transfers.cancelAndWait("key")))
                  .start();
  executor.drain();
  EXPECT_FALSE(wait.isReady());
  transfer.reset();
  executor.drain();
  ASSERT_TRUE(wait.isReady());
  std::move(wait).get();
}

TEST(InFlightTransfersTest, OperationWaitsForTransferAndBlocksNewTransfers) {
  for (auto policy : {InFlightTransfers::TransferPolicy::Wait,
                      InFlightTransfers::TransferPolicy::CancelAndWait}) {
    SCOPED_TRACE(static_cast<int>(policy));
    folly::ManualExecutor executor;
    InFlightTransfers transfers;
    auto transfer = transfers.tryAcquireTransferToken("key");
    ASSERT_TRUE(transfer);
    auto mutation = folly::coro::co_withExecutor(
                        &executor, transfers.lockForOperation("key", policy))
                        .start();
    executor.drain();
    EXPECT_EQ(transfer->getCancellationToken().isCancellationRequested(),
              policy == InFlightTransfers::TransferPolicy::CancelAndWait);
    EXPECT_FALSE(mutation.isReady());
    EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
    EXPECT_TRUE(transfers.tryAcquireTransferToken("other"));
    transfer.reset();
    executor.drain();
    ASSERT_TRUE(mutation.isReady());
    EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
    auto guard = std::move(mutation).get();
    EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
    guard = {};
    EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
  }
}

TEST(InFlightTransfersTest, QueuedOperationsWithDifferentPoliciesRunInOrder) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  auto cancellation = transfer->getCancellationToken();
  auto first = folly::coro::co_withExecutor(
                   &executor,
                   transfers.lockForOperation(
                       "key", InFlightTransfers::TransferPolicy::Wait))
                   .start();
  executor.drain();
  EXPECT_FALSE(transfer->getCancellationToken().isCancellationRequested());
  auto second =
      folly::coro::co_withExecutor(
          &executor,
          transfers.lockForOperation(
              "key", InFlightTransfers::TransferPolicy::CancelAndWait))
          .start();
  executor.drain();
  EXPECT_FALSE(cancellation.isCancellationRequested());
  EXPECT_FALSE(first.isReady());
  EXPECT_FALSE(second.isReady());
  transfer.reset();
  executor.drain();
  EXPECT_FALSE(cancellation.canBeCancelled());
  ASSERT_TRUE(first.isReady());
  EXPECT_FALSE(second.isReady());
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  auto firstGuard = std::move(first).get();
  firstGuard = {};
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  executor.drain();
  ASSERT_TRUE(second.isReady());
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  auto secondGuard = std::move(second).get();
  secondGuard = {};
  EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
}

TEST(InFlightTransfersTest,
     CompletionWaitIncludesObservedGuardButNotLaterGuards) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  auto first = folly::coro::co_withExecutor(
                   &executor,
                   transfers.lockForOperation(
                       "key", InFlightTransfers::TransferPolicy::Wait))
                   .start();
  executor.drain();
  transfers.cancel("key");
  EXPECT_FALSE(transfer->getCancellationToken().isCancellationRequested());
  auto completion =
      folly::coro::co_withExecutor(&executor, transfers.cancelAndWait("key"))
          .start();
  executor.drain();
  EXPECT_FALSE(transfer->getCancellationToken().isCancellationRequested());
  EXPECT_FALSE(completion.isReady());
  auto second =
      folly::coro::co_withExecutor(
          &executor,
          transfers.lockForOperation(
              "key", InFlightTransfers::TransferPolicy::CancelAndWait))
          .start();
  executor.drain();
  EXPECT_FALSE(transfer->getCancellationToken().isCancellationRequested());
  transfer.reset();
  executor.drain();
  ASSERT_TRUE(first.isReady());
  EXPECT_FALSE(completion.isReady());
  EXPECT_FALSE(second.isReady());
  auto firstGuard = std::move(first).get();
  firstGuard = {};
  executor.drain();
  ASSERT_TRUE(completion.isReady());
  std::move(completion).get();
  ASSERT_TRUE(second.isReady());
  auto secondGuard = std::move(second).get();
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  secondGuard = {};
  EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
}

TEST(InFlightTransfersTest, UnstartedMutationDoesNotCancelOrReserve) {
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  {
    auto mutation = transfers.lockForOperation(
        "key", InFlightTransfers::TransferPolicy::CancelAndWait);
    EXPECT_FALSE(transfer->getCancellationToken().isCancellationRequested());
    transfer.reset();
    EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
  }
}

CO_TEST(InFlightTransfersTest, MovesMutationOwnershipAndReleasesPreviousKey) {
  InFlightTransfers transfers;
  auto first = co_await transfers.lockForOperation(
      "first", InFlightTransfers::TransferPolicy::CancelAndWait);
  auto second = co_await transfers.lockForOperation(
      "second", InFlightTransfers::TransferPolicy::CancelAndWait);
  auto moved = std::move(first);
  first = {};
  EXPECT_FALSE(transfers.tryAcquireTransferToken("first"));
  second = std::move(moved);
  moved = {};
  EXPECT_FALSE(transfers.tryAcquireTransferToken("first"));
  EXPECT_TRUE(transfers.tryAcquireTransferToken("second"));
  second = {};
  EXPECT_TRUE(transfers.tryAcquireTransferToken("first"));
}

TEST(InFlightTransfersTest, MutationTaskAndGuardOutliveRegistryAndKey) {
  // Exceeds the small-string buffer so ASan reports reads of the freed key.
  constexpr size_t kHeapKeyLength = 100;
  folly::ManualExecutor executor;
  auto transfers = std::make_unique<InFlightTransfers>();
  auto transfer =
      transfers->tryAcquireTransferToken(std::string(kHeapKeyLength, 'k'));
  ASSERT_TRUE(transfer);
  auto task = transfers->lockForOperation(
      std::string(kHeapKeyLength, 'k'),
      InFlightTransfers::TransferPolicy::CancelAndWait);
  transfers.reset();
  auto mutation =
      folly::coro::co_withExecutor(&executor, std::move(task)).start();
  executor.drain();
  EXPECT_TRUE(transfer->getCancellationToken().isCancellationRequested());
  EXPECT_FALSE(mutation.isReady());
  transfer.reset();
  executor.drain();
  ASSERT_TRUE(mutation.isReady());
  auto guard = std::move(mutation).get();
  guard = {};
}

CO_TEST(InFlightTransfersTest,
        TransferCompletionDoesNotWaitForKeyOperationGuard) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  CO_ASSERT_TRUE(transfer);
  auto completion =
      folly::coro::co_withExecutor(&executor, transfers.cancelAndWait("key"))
          .start();
  executor.drain();
  auto passiveCompletion =
      folly::coro::co_withExecutor(&executor, transfers.waitForTransfer("key"))
          .start();
  executor.drain();
  auto mutation =
      folly::coro::co_withExecutor(
          &executor,
          transfers.lockForOperation(
              "key", InFlightTransfers::TransferPolicy::CancelAndWait))
          .start();
  executor.drain();
  auto hiddenTransferWait =
      folly::coro::co_withExecutor(&executor, transfers.waitForTransfer("key"))
          .start();
  executor.drain();
  CO_ASSERT_TRUE(hiddenTransferWait.isReady());
  std::move(hiddenTransferWait).get();
  EXPECT_FALSE(mutation.isReady());
  EXPECT_FALSE(completion.isReady());
  EXPECT_FALSE(passiveCompletion.isReady());
  transfer.reset();
  executor.drain();
  CO_ASSERT_TRUE(mutation.isReady());
  CO_ASSERT_TRUE(completion.isReady());
  CO_ASSERT_TRUE(passiveCompletion.isReady());
  auto guard = std::move(mutation).get();
  std::move(completion).get();
  std::move(passiveCompletion).get();
  transfers.cancel("key");
  auto heldMutationWait =
      folly::coro::co_withExecutor(&executor, transfers.waitForTransfer("key"))
          .start();
  executor.drain();
  CO_ASSERT_TRUE(heldMutationWait.isReady());
  std::move(heldMutationWait).get();
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  guard = {};
  EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
}

TEST(InFlightTransfersTest,
     CallerCancellationDoesNotReleaseMutationReservation) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  folly::CancellationSource callerCancellation;
  callerCancellation.requestCancellation();
  auto mutation =
      folly::coro::co_withExecutor(
          &executor,
          folly::coro::co_withCancellation(
              callerCancellation.getToken(),
              transfers.lockForOperation(
                  "key", InFlightTransfers::TransferPolicy::CancelAndWait)))
          .start();
  executor.drain();
  EXPECT_TRUE(transfer->getCancellationToken().isCancellationRequested());
  EXPECT_FALSE(mutation.isReady());
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  transfer.reset();
  executor.drain();
  ASSERT_TRUE(mutation.isReady());
  auto guard = std::move(mutation).get();
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  guard = {};
  EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
}

TEST(InFlightTransfersTest, MutationReservesBeforeCancellationCallbacksRun) {
  folly::ManualExecutor executor;
  InFlightTransfers transfers;
  auto transfer = transfers.tryAcquireTransferToken("key");
  ASSERT_TRUE(transfer);
  folly::CancellationCallback callback{
      transfer->getCancellationToken(), [&] {
        transfer.reset();
        EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
      }};
  auto mutation =
      folly::coro::co_withExecutor(
          &executor,
          transfers.lockForOperation(
              "key", InFlightTransfers::TransferPolicy::CancelAndWait))
          .start();
  executor.drain();
  ASSERT_TRUE(mutation.isReady());
  EXPECT_FALSE(transfer);
  auto guard = std::move(mutation).get();
  EXPECT_FALSE(transfers.tryAcquireTransferToken("key"));
  guard = {};
  EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
}

namespace {

struct ExclusionCounters {
  std::atomic<int> transfers{0};
  std::atomic<int> mutations{0};
};

folly::coro::Task<void> runTransfersAndMutations(InFlightTransfers& registry,
                                                 ExclusionCounters& active,
                                                 size_t worker,
                                                 size_t iterations) {
  for (size_t iteration = 0; iteration < iterations; ++iteration) {
    registry.cancel("key");
    if ((worker + iteration) % 2 == 0) {
      if (auto transfer = registry.tryAcquireTransferToken("key")) {
        EXPECT_EQ(active.transfers.fetch_add(1), 0);
        std::this_thread::yield();
        EXPECT_EQ(active.mutations.load(), 0);
        active.transfers.fetch_sub(1);
      }
    } else {
      auto guard = co_await registry.lockForOperation(
          "key", worker % 2 == 0
                     ? InFlightTransfers::TransferPolicy::Wait
                     : InFlightTransfers::TransferPolicy::CancelAndWait);
      EXPECT_EQ(active.mutations.fetch_add(1), 0);
      std::this_thread::yield();
      EXPECT_EQ(active.transfers.load(), 0);
      active.mutations.fetch_sub(1);
    }
  }
}

} // namespace

CO_TEST(InFlightTransfersTest, ConcurrentTransfersAndMutationsNeverOverlap) {
  constexpr size_t kWorkers = 4;
  constexpr size_t kIterations = 256;
  InFlightTransfers transfers;
  ExclusionCounters active;
  folly::CPUThreadPoolExecutor executor{kWorkers};
  std::vector<folly::coro::TaskWithExecutor<void>> workers;
  workers.reserve(kWorkers);
  for (size_t worker = 0; worker < kWorkers; ++worker) {
    workers.push_back(folly::coro::co_withExecutor(
        &executor,
        runTransfersAndMutations(transfers, active, worker, kIterations)));
  }
  co_await folly::coro::collectAllRange(std::move(workers));
  EXPECT_EQ(active.transfers.load(), 0);
  EXPECT_EQ(active.mutations.load(), 0);
  EXPECT_TRUE(transfers.tryAcquireTransferToken("key"));
}
