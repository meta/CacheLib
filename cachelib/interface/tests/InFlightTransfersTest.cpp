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
#include <folly/coro/GtestHelpers.h>
#include <folly/coro/WithCancellation.h>
#include <folly/executors/ManualExecutor.h>
#include <gtest/gtest.h>

#include <array>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

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
