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

#include <folly/ScopeGuard.h>
#include <folly/coro/Baton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/coro/Task.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/synchronization/Baton.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "cachelib/interface/InFlightTransfers.h"
#include "cachelib/interface/MemcpyConnector.h"
#include "cachelib/interface/TransferExecutor.h"
#include "cachelib/interface/components/tests/CacheComponentFactory.h"
#include "cachelib/interface/tests/TransferTestUtils.h"
#include "cachelib/interface/tests/Utils.h"

using namespace facebook::cachelib::interface;
using namespace facebook::cachelib::interface::test;

namespace {

std::string valueOf(const ReadDescriptor& descriptor) {
  return std::string(static_cast<const char*>(descriptor.data()),
                     descriptor.size());
}

class BlockingConnector final : public Connector {
 public:
  explicit BlockingConnector(size_t expectedConcurrentTransfers)
      : expectedConcurrentTransfers_(expectedConcurrentTransfers) {}

  folly::coro::Task<Result<AllocatedDescriptor>> transfer(
      ReadDescriptor source, AllocatedDescriptor destination) override {
    {
      std::unique_lock lock{mutex_};
      ++startedTransfers_;
      startedCv_.notify_all();
      releaseCv_.wait(lock, [this] { return released_; });
    }

    std::memcpy(destination.mutableData(), source.data(), source.size());
    co_return std::move(destination);
  }

  bool waitForConcurrentTransfers() {
    std::unique_lock lock{mutex_};
    return startedCv_.wait_for(lock, std::chrono::seconds(5), [this] {
      return startedTransfers_ >= expectedConcurrentTransfers_;
    });
  }

  void release() {
    {
      std::lock_guard lock{mutex_};
      released_ = true;
    }
    releaseCv_.notify_all();
  }

 private:
  const size_t expectedConcurrentTransfers_;
  std::mutex mutex_;
  std::condition_variable startedCv_;
  std::condition_variable releaseCv_;
  size_t startedTransfers_{0};
  bool released_{false};
};

// Suspend without blocking an executor thread. The host wait is bounded, and
// callers release the operation on assertion failures before draining work.
class TransferPause {
 public:
  folly::coro::Task<void> wait() {
    const auto token = co_await folly::coro::co_current_cancellation_token;
    EXPECT_FALSE(token.canBeCancelled());
    started_.post();
    co_await released_;
    co_await folly::coro::co_safe_point;
  }

  bool waitUntilStarted() {
    return started_.try_wait_for(std::chrono::seconds(5));
  }

  void release() { released_.post(); }

 private:
  folly::Baton<> started_;
  folly::coro::Baton released_;
};

class PausingConnector final : public Connector {
 public:
  explicit PausingConnector(TransferPause* pause = nullptr, bool fail = false)
      : pause_(pause), fail_(fail) {}

  folly::coro::Task<Result<AllocatedDescriptor>> transfer(
      ReadDescriptor source, AllocatedDescriptor destination) override {
    ++calls;
    if (pause_) {
      co_await pause_->wait();
    }
    if (fail_) {
      co_return makeError(Error::Code::INVALID_ARGUMENTS, "transfer failed");
    }
    co_return co_await connector_.transfer(std::move(source),
                                           std::move(destination));
  }

  size_t calls{0};

 private:
  TransferPause* pause_;
  bool fail_;
  MemcpyConnector connector_;
};

// Keep real RAM allocations and cleanup, pausing only the selected operation.
class PausingCacheComponent final : public CacheComponent {
 public:
  PausingCacheComponent(CacheComponent& component,
                        TransferPause* allocationPause,
                        TransferPause* insertionPause)
      : CacheComponent({}),
        component_(component),
        allocationPause_(allocationPause),
        insertionPause_(insertionPause) {}

  folly::coro::Task<Result<AllocatedDescriptor>> allocate(
      Key key,
      uint32_t size,
      uint32_t creationTime,
      uint32_t ttlSecs) override {
    auto result =
        co_await component_.allocate(key, size, creationTime, ttlSecs);
    if (allocationPause_) {
      co_await allocationPause_->wait();
    }
    co_return result;
  }

  folly::coro::Task<Result<std::optional<AllocatedHandle>>> insertOrReplace(
      AllocatedHandle&& handle) override {
    if (insertionPause_) {
      co_await insertionPause_->wait();
    }
    co_return co_await component_.insertOrReplace(std::move(handle));
  }

  const std::string& getName() const noexcept override {
    return component_.getName();
  }
  folly::coro::Task<UnitResult> insert(AllocatedHandle&& handle) override {
    return component_.insert(std::move(handle));
  }
  folly::coro::Task<Result<std::optional<ReadDescriptor>>> find(
      Key key) override {
    return component_.find(key);
  }
  folly::coro::Task<Result<std::optional<WriteDescriptor>>> findToWrite(
      Key key) override {
    return component_.findToWrite(key);
  }
  folly::coro::AsyncGenerator<ReadDescriptor> iterator() override {
    return component_.iterator();
  }
  folly::coro::Task<Result<bool>> remove(Key key) override {
    return component_.remove(key);
  }
  folly::coro::Task<UnitResult> remove(ReadHandle&& handle) override {
    return component_.remove(std::move(handle));
  }
  UnitResult shutdown() override { return component_.shutdown(); }
  CacheComponentStats getStats() const noexcept override {
    return component_.getStats();
  }

 private:
  // Handles are owned by the underlying component, never this adapter.
  UnitResult writeBack(CacheItem&) override { std::terminate(); }
  void release(CacheItem&, bool) override { std::terminate(); }

  CacheComponent& component_;
  TransferPause* allocationPause_;
  TransferPause* insertionPause_;
};

class TransferExecutorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    destination_ = factory_.create();
    ASSERT_NE(destination_, nullptr);
  }

  void TearDown() override { EXPECT_OK(destination_->shutdown()); }

  InFlightTransfers::TransferToken acquire(Key key) {
    return inFlight_.tryAcquireTransferToken(key).value();
  }

  CacheComponentStats componentStats() const {
    return static_cast<const CacheComponentWithStats&>(*destination_)
        .CacheComponentWithStats::getStats();
  }

  folly::coro::Task<void> cancelAndWaitForPausedTransfer(
      TransferPause& pause, CacheComponent& destination, Connector& connector) {
    folly::ManualExecutor waiterExecutor;
    TransferExecutor executor;
    SCOPE_EXIT { pause.release(); };

    EXPECT_OK(executor.submit(makeDetachedItem("key", "value"), destination,
                              connector, acquire("key")));
    CO_ASSERT_TRUE(pause.waitUntilStarted());
    auto completed = folly::coro::co_withExecutor(
                         &waiterExecutor, inFlight_.cancelAndWait("key"))
                         .start();
    waiterExecutor.drain();
    EXPECT_FALSE(completed.isReady());
    EXPECT_FALSE(inFlight_.tryAcquireTransferToken("key").has_value());
    EXPECT_EQ(componentStats().release_.throughput_.calls_, 0);

    pause.release();
    while (!completed.isReady()) {
      waiterExecutor.drive();
    }
    EXPECT_NO_THROW(std::move(completed).get());
    EXPECT_TRUE(inFlight_.tryAcquireTransferToken("key").has_value());
    const auto stats = componentStats();
    EXPECT_EQ(stats.allocate_.throughput_.calls_, 1);
    EXPECT_EQ(stats.insertOrReplace_.throughput_.calls_, 0);
    EXPECT_EQ(stats.release_.throughput_.successes_, 1);
    auto found = CO_ASSERT_OK(co_await destination_->find("key"));
    EXPECT_FALSE(found.has_value());
    executor.closeAndDrain();
  }

  InFlightTransfers inFlight_;
  RAMCacheFactory factory_;
  std::unique_ptr<CacheComponent> destination_;
  MemcpyConnector connector_;
};

TEST_F(TransferExecutorTest, TransfersItem) {
  TransferExecutor executor;

  EXPECT_OK(executor.submit(makeDetachedItem("key", "value", 100, 0),
                            *destination_, connector_, acquire("key")));
  executor.closeAndDrain();

  auto found = ASSERT_OK(folly::coro::blockingWait(destination_->find("key")));
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->key(), "key");
  EXPECT_EQ(valueOf(*found), "value");
  EXPECT_EQ(found->creationTime(), 100);
  EXPECT_EQ(found->expiryTime(), 0);
}

TEST_F(TransferExecutorTest, UsesConfiguredThreadCount) {
  TransferExecutor executor(2);
  BlockingConnector connector(2);

  EXPECT_OK(executor.submit(makeDetachedItem("key1", "value1"), *destination_,
                            connector, acquire("key1")));
  EXPECT_OK(executor.submit(makeDetachedItem("key2", "value2"), *destination_,
                            connector, acquire("key2")));

  EXPECT_TRUE(connector.waitForConcurrentTransfers());
  connector.release();
  executor.closeAndDrain();
}

TEST(TransferExecutorConstructionTest, RejectsZeroThreads) {
  EXPECT_THROW((void)TransferExecutor(0), std::invalid_argument);
}

TEST_F(TransferExecutorTest, DropsItemWithInvalidExpiry) {
  TransferExecutor executor;

  EXPECT_OK(executor.submit(makeDetachedItem("key", "value", 100, 99),
                            *destination_, connector_, acquire("key")));
  executor.closeAndDrain();

  auto found = ASSERT_OK(folly::coro::blockingWait(destination_->find("key")));
  EXPECT_FALSE(found.has_value());
}

TEST_F(TransferExecutorTest, DestructorDrainsAcceptedWork) {
  {
    TransferExecutor executor;
    EXPECT_OK(executor.submit(makeDetachedItem("key", "value"), *destination_,
                              connector_, acquire("key")));
  }

  auto found = ASSERT_OK(folly::coro::blockingWait(destination_->find("key")));
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(valueOf(*found), "value");
}

TEST_F(TransferExecutorTest, CancellationBeforeStartSkipsAllocation) {
  TransferExecutor executor;
  PausingConnector connector;
  auto transfer = acquire("key");
  inFlight_.cancel("key");

  EXPECT_OK(executor.submit(makeDetachedItem("key", "value"), *destination_,
                            connector, std::move(transfer)));
  executor.closeAndDrain();

  EXPECT_EQ(componentStats().allocate_.throughput_.calls_, 0);
  EXPECT_EQ(connector.calls, 0);
  EXPECT_TRUE(inFlight_.tryAcquireTransferToken("key").has_value());
}

CO_TEST_F(TransferExecutorTest, CancelAndWaitDuringAllocationSkipsCopy) {
  TransferPause pause;
  PausingCacheComponent destination(*destination_, &pause, nullptr);
  PausingConnector connector;

  co_await cancelAndWaitForPausedTransfer(pause, destination, connector);
  EXPECT_EQ(connector.calls, 0);
}

CO_TEST_F(TransferExecutorTest, CancelAndWaitDuringCopySkipsInsertion) {
  TransferPause pause;
  PausingConnector connector(&pause);

  co_await cancelAndWaitForPausedTransfer(pause, *destination_, connector);
  EXPECT_EQ(connector.calls, 1);
}

CO_TEST_F(TransferExecutorTest, CancelAndWaitWaitsForCleanupAfterFailure) {
  TransferPause pause;
  PausingConnector connector(&pause, true);

  co_await cancelAndWaitForPausedTransfer(pause, *destination_, connector);
  EXPECT_EQ(connector.calls, 1);
}

CO_TEST_F(TransferExecutorTest, CancelAndWaitDuringInsertionLetsItFinish) {
  TransferPause pause;
  PausingCacheComponent destination(*destination_, nullptr, &pause);
  folly::ManualExecutor waiterExecutor;
  TransferExecutor executor;
  SCOPE_EXIT { pause.release(); };

  EXPECT_OK(executor.submit(makeDetachedItem("key", "value"), destination,
                            connector_, acquire("key")));
  CO_ASSERT_TRUE(pause.waitUntilStarted());
  auto completed = folly::coro::co_withExecutor(&waiterExecutor,
                                                inFlight_.cancelAndWait("key"))
                       .start();
  waiterExecutor.drain();
  EXPECT_FALSE(completed.isReady());
  EXPECT_FALSE(inFlight_.tryAcquireTransferToken("key").has_value());
  auto before = CO_ASSERT_OK(co_await destination_->find("key"));
  EXPECT_FALSE(before.has_value());

  pause.release();
  while (!completed.isReady()) {
    waiterExecutor.drive();
  }
  EXPECT_NO_THROW(std::move(completed).get());

  EXPECT_TRUE(inFlight_.tryAcquireTransferToken("key").has_value());
  auto after = CO_ASSERT_OK(co_await destination_->find("key"));
  CO_ASSERT_TRUE(after.has_value());
  EXPECT_EQ(valueOf(*after), "value");
  executor.closeAndDrain();
}

TEST_F(TransferExecutorTest, RejectsWorkAfterClose) {
  folly::ManualExecutor waiterExecutor;
  TransferExecutor executor;
  executor.closeAndDrain();
  auto transfer = acquire("key");
  auto completed = folly::coro::co_withExecutor(&waiterExecutor,
                                                inFlight_.cancelAndWait("key"))
                       .start();
  waiterExecutor.drain();
  EXPECT_FALSE(completed.isReady());

  EXPECT_ERROR(executor.submit(makeDetachedItem("key", "value"), *destination_,
                               connector_, std::move(transfer)),
               Error::Code::SHUTDOWN_FAILED);
  waiterExecutor.drain();
  ASSERT_TRUE(completed.isReady());
  EXPECT_NO_THROW(std::move(completed).get());
  EXPECT_TRUE(inFlight_.tryAcquireTransferToken("key").has_value());

  auto found = ASSERT_OK(folly::coro::blockingWait(destination_->find("key")));
  EXPECT_FALSE(found.has_value());
}

} // namespace
