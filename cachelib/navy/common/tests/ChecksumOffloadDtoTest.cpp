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

#include <dto.h>
#include <folly/Function.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>
#include <sys/timerfd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/allocator/nvmcache/NavySetup.h"
#include "cachelib/navy/common/ChecksumOffload.h"
#include "cachelib/navy/common/Hash.h"

// This test executable wraps the DTO calls used by ChecksumOffload.cpp.
// Device work remains pending until polled, without requiring a DSA work queue.
namespace {
struct FakeOperation {
  uint8_t* dest{nullptr};
  const uint8_t* src{nullptr};
  size_t size{0};
  unsigned int polls{0};
  int status{DTO_ASYNC_PENDING};
  uint32_t crc{0};
};
static_assert(sizeof(FakeOperation) <= sizeof(dto_async_op));
static_assert(alignof(FakeOperation) <= alignof(dto_async_op));

struct FakeBackend {
  bool refuseSubmission{false};
  bool failCompletion{false};
  bool waitBlocks{false};
  bool failTimerCreate{false};
  bool failTimerArm{false};
  unsigned int pendingPolls{1};
  unsigned int submissions{0};
  unsigned int polls{0};
  unsigned int waits{0};
  unsigned int completions{0};
  unsigned int timerCreates{0};
  std::vector<uint64_t> timerDelaysNs;
  folly::Function<void()> onPoll;
};
thread_local bool captureTimerCalls = false;

FakeBackend& fakeBackend() {
  static FakeBackend backend;
  return backend;
}

int submitFake(dto_async_op* op, void* dest, const void* src, size_t size) {
  ++fakeBackend().submissions;
  if (fakeBackend().refuseSubmission || size == 0) {
    return DTO_ASYNC_FALLBACK;
  }
  new (op) FakeOperation{static_cast<uint8_t*>(dest),
                         static_cast<const uint8_t*>(src), size};
  return DTO_ASYNC_SUBMITTED;
}

FakeOperation& operation(dto_async_op* op) {
  return *std::launder(reinterpret_cast<FakeOperation*>(op));
}

[[noreturn]] void failOverlap() { throw std::runtime_error("overlap failed"); }
} // namespace

extern "C" {
int __real_timerfd_create(int clockId, int flags);
int __real_timerfd_settime(int fd,
                           int flags,
                           const itimerspec* spec,
                           itimerspec* oldSpec);

int __wrap_timerfd_create(int clockId, int flags) {
  if (captureTimerCalls) {
    ++fakeBackend().timerCreates;
    if (fakeBackend().failTimerCreate) {
      fakeBackend().failTimerCreate = false;
      errno = EMFILE;
      return -1;
    }
  }
  return __real_timerfd_create(clockId, flags);
}

int __wrap_timerfd_settime(int fd,
                           int flags,
                           const itimerspec* spec,
                           itimerspec* oldSpec) {
  if (captureTimerCalls) {
    if (spec->it_value.tv_sec != 0 || spec->it_value.tv_nsec != 0) {
      fakeBackend().timerDelaysNs.push_back(
          static_cast<uint64_t>(spec->it_value.tv_sec) * 1'000'000'000ULL +
          static_cast<uint64_t>(spec->it_value.tv_nsec));
    }
    if (fakeBackend().failTimerArm) {
      fakeBackend().failTimerArm = false;
      errno = EINVAL;
      return -1;
    }
  }
  return __real_timerfd_settime(fd, flags, spec, oldSpec);
}

int __wrap_dto_submit_crc(dto_async_op* op,
                          const void* src,
                          size_t size,
                          unsigned int /* flags */) {
  return submitFake(op, nullptr, src, size);
}

int __wrap_dto_submit_memcpy_crc(dto_async_op* op,
                                 void* dest,
                                 const void* src,
                                 size_t size,
                                 unsigned int /* flags */) {
  return submitFake(op, dest, src, size);
}

int __wrap_dto_submit_memcpy(dto_async_op* op,
                             void* dest,
                             const void* src,
                             size_t size,
                             unsigned int /* flags */) {
  return submitFake(op, dest, src, size);
}

int __wrap_dto_async_poll(dto_async_op* op) {
  ++fakeBackend().polls;
  if (fakeBackend().onPoll) {
    auto callback = std::move(fakeBackend().onPoll);
    callback();
  }
  auto& pending = operation(op);
  if (pending.status != DTO_ASYNC_PENDING) {
    return pending.status;
  }
  if (++pending.polls <= fakeBackend().pendingPolls) {
    return DTO_ASYNC_PENDING;
  }
  ++fakeBackend().completions;
  if (fakeBackend().failCompletion) {
    if (pending.dest) {
      pending.dest[0] = 0xFF;
    }
    pending.status = DTO_ASYNC_FAILED;
  } else {
    pending.crc = facebook::cachelib::navy::checksum(
        facebook::cachelib::navy::BufferView{pending.size, pending.src},
        facebook::cachelib::navy::ChecksumType::Crc32c);
    if (pending.dest) {
      std::memcpy(pending.dest, pending.src, pending.size);
    }
    pending.status = DTO_ASYNC_DONE;
  }
  return pending.status;
}

void __wrap_dto_async_wait(dto_async_op* op) {
  ++fakeBackend().waits;
  while (__wrap_dto_async_poll(op) == DTO_ASYNC_PENDING) {
  }
}

uint32_t __wrap_dto_async_crc_val(const dto_async_op* op) {
  return std::launder(reinterpret_cast<const FakeOperation*>(op))->crc;
}

int __wrap_dto_wait_blocks() { return fakeBackend().waitBlocks ? 1 : 0; }
} // extern "C"

namespace facebook::cachelib::navy::tests {
class ChecksumOffloadDtoTest : public testing::Test {
 protected:
  void SetUp() override {
    fakeBackend() = FakeBackend{};
    captureTimerCalls = true;
    ASSERT_TRUE(checksumOffloadSupported());
  }

  void TearDown() override { captureTimerCalls = false; }

  void expectCooperativeSleep(uint32_t sleepPollUs = 200,
                              bool addReadyFiber = false) {
    captureTimerCalls = false;
    const std::vector<uint8_t> src(4096, 0x3C);
    std::vector<uint8_t> dest(src.size());
    const bool expectedOnDevice = !fakeBackend().failCompletion;
    bool callbackRan = false;
    bool otherFiberRan = false;
    folly::EventBase evb;
    auto& fm = folly::fibers::getFiberManager(evb);
    fm.addTask([&] {
      evb.runInLoop([&] { callbackRan = true; });
      AsyncChecksumOp op;
      op.submitCopy(dest.data(), BufferView{src.size(), src.data()}, false);
      captureTimerCalls = true;
      const auto crc = op.wait(LargeCopyWait::kSleep, sleepPollUs);
      captureTimerCalls = false;
      EXPECT_EQ(0u, crc);
      EXPECT_TRUE(callbackRan);
      EXPECT_EQ(expectedOnDevice, op.completedOnDevice());
      EXPECT_EQ(src, dest);
      if (addReadyFiber) {
        EXPECT_TRUE(otherFiberRan);
      }
    });
    if (addReadyFiber) {
      fm.addTask([&] { otherFiberRan = true; });
    }
    evb.loop();
    EXPECT_TRUE(callbackRan);
  }
};

TEST_F(ChecksumOffloadDtoTest, RefusedSubmissionCompletesOnCpuWithoutPolling) {
  fakeBackend().refuseSubmission = true;
  const std::vector<uint8_t> src{1, 2, 3, 4, 5, 6, 7, 8};
  const BufferView view{src.size(), src.data()};
  std::vector<uint8_t> dest(src.size());
  AsyncChecksumOp op;

  op.submitCopyAndChecksum(dest.data(), view, true);
  EXPECT_EQ(checksum(view, ChecksumType::Crc32c), op.wait());
  EXPECT_EQ(src, dest);
  EXPECT_FALSE(op.completedOnDevice());
  op.submitChecksum(view);
  EXPECT_EQ(checksum(view, ChecksumType::Crc32c), op.wait());
  EXPECT_FALSE(op.completedOnDevice());
  EXPECT_EQ(2u, fakeBackend().submissions);
  EXPECT_EQ(0u, fakeBackend().polls);
}

TEST_F(ChecksumOffloadDtoTest, FailedCompletionIsRepairedOnCpu) {
  fakeBackend().failCompletion = true;
  const std::vector<uint8_t> src{9, 10, 11, 12, 13, 14, 15, 16};
  const BufferView view{src.size(), src.data()};
  std::vector<uint8_t> dest(src.size());
  const auto before = getChecksumWaitStats();
  AsyncChecksumOp op;

  op.submitCopyAndChecksum(dest.data(), view, true);
  EXPECT_EQ(checksum(view, ChecksumType::Crc32c), op.wait());
  EXPECT_EQ(src, dest);
  EXPECT_FALSE(op.completedOnDevice());
  op.submitChecksum(view);
  EXPECT_EQ(checksum(view, ChecksumType::Crc32c), op.wait());
  EXPECT_FALSE(op.completedOnDevice());
  EXPECT_EQ(before.fallbacks + 2, getChecksumWaitStats().fallbacks);
}

TEST_F(ChecksumOffloadDtoTest, PlainCopiesDistinguishRefusalAndRepair) {
  const std::vector<uint8_t> src(8193, 0x5A);
  std::vector<uint8_t> dest(src.size());
  for (bool refused : {false, true}) {
    for (bool failed : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << "refused=" << refused << " failed=" << failed);
      fakeBackend() = FakeBackend{};
      fakeBackend().refuseSubmission = refused;
      fakeBackend().failCompletion = failed;
      std::fill(dest.begin(), dest.end(), 0);
      const auto checksumBefore = getChecksumWaitStats();
      const bool onDevice =
          copyWithOffload(dest.data(), src.data(), src.size());
      EXPECT_EQ(!refused && !failed, onDevice);
      EXPECT_EQ(src, dest);
      EXPECT_EQ(1u, fakeBackend().submissions);
      EXPECT_EQ(refused ? 0u : 2u, fakeBackend().polls);
      // Plain copies are not checksum work.
      EXPECT_EQ(checksumBefore.calls, getChecksumWaitStats().calls);
      EXPECT_EQ(checksumBefore.fallbacks, getChecksumWaitStats().fallbacks);
    }
  }
}

TEST_F(ChecksumOffloadDtoTest, SleepWaitSleepsBetweenPolls) {
  const std::vector<uint8_t> src(4096, 0x3C);
  std::vector<uint8_t> dest(src.size());
  AsyncChecksumOp op;
  op.submitCopy(dest.data(), BufferView{src.size(), src.data()}, false);
  constexpr uint32_t kSleepUs = 5000;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(0u, op.wait(LargeCopyWait::kSleep, kSleepUs));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_TRUE(op.completedOnDevice());
  EXPECT_EQ(src, dest);
  // The fake reports one pending poll, so the wait slept once.
  EXPECT_EQ(2u, fakeBackend().polls);
  EXPECT_GE(elapsed, std::chrono::microseconds(kSleepUs));
}

TEST_F(ChecksumOffloadDtoTest, SleepWaitRunsEventBaseCallbacks) {
  for (bool addReadyFiber : {false, true}) {
    SCOPED_TRACE(addReadyFiber);
    fakeBackend() = FakeBackend{};
    expectCooperativeSleep(200, addReadyFiber);
  }
}

TEST_F(ChecksumOffloadDtoTest, SleepWaitKeepsMicrosecondIntervals) {
  fakeBackend().pendingPolls = 3;
  expectCooperativeSleep(200);
  EXPECT_EQ(4u, fakeBackend().polls);
  EXPECT_EQ(1u, fakeBackend().timerCreates);
  const std::vector<uint64_t> expected{200'000, 200'000, 200'000};
  EXPECT_EQ(expected, fakeBackend().timerDelaysNs);
}

TEST_F(ChecksumOffloadDtoTest, SleepWaitReusesOneTimerAcrossCopies) {
  captureTimerCalls = false;
  folly::EventBase evb;
  auto& fm = folly::fibers::getFiberManager(evb);
  const std::vector<uint8_t> src(4096, 0x3C);
  unsigned int copiesStarted = 0;
  auto copy = [&] {
    ++copiesStarted;
    std::vector<uint8_t> dest(src.size());
    AsyncChecksumOp op;
    op.submitCopy(dest.data(), BufferView{src.size(), src.data()}, false);
    EXPECT_EQ(0u, op.wait(LargeCopyWait::kSleep));
    EXPECT_GE(copiesStarted, 2u);
    EXPECT_TRUE(op.completedOnDevice());
    EXPECT_EQ(src, dest);
  };
  captureTimerCalls = true;
  fm.addTask(copy);
  fm.addTask(copy);
  EXPECT_TRUE(evb.loop());
  EXPECT_EQ(2u, fakeBackend().completions);

  fm.addTask(copy);
  EXPECT_TRUE(evb.loop());
  EXPECT_EQ(3u, fakeBackend().completions);
  EXPECT_EQ(1u, fakeBackend().timerCreates);
  captureTimerCalls = false;
}

TEST_F(ChecksumOffloadDtoTest, SleepWaitFallsBackOnTimerErrors) {
  for (bool failCreate : {false, true}) {
    SCOPED_TRACE(failCreate);
    fakeBackend() = FakeBackend{};
    fakeBackend().failTimerCreate = failCreate;
    fakeBackend().failTimerArm = !failCreate;
    expectCooperativeSleep();
    EXPECT_FALSE(fakeBackend().failTimerCreate);
    EXPECT_FALSE(fakeBackend().failTimerArm);
  }
}

TEST_F(ChecksumOffloadDtoTest, SleepWaitDoesNotUseBlockingDtoWait) {
  fakeBackend().waitBlocks = true;
  expectCooperativeSleep();
  EXPECT_EQ(0u, fakeBackend().waits);
}

TEST_F(ChecksumOffloadDtoTest, SleepWaitRepairsFailedCopiesAfterSuspension) {
  fakeBackend().failCompletion = true;
  expectCooperativeSleep();
  EXPECT_EQ(1u, fakeBackend().completions);
}

TEST_F(ChecksumOffloadDtoTest, SleepWaitWithZeroIntervalRunsEventBase) {
  expectCooperativeSleep(0);
  const std::vector<uint64_t> expected{1'000};
  EXPECT_EQ(expected, fakeBackend().timerDelaysNs);
}

TEST_F(ChecksumOffloadDtoTest, FlushCopyOffloadIsForwardedFromNavyConfig) {
  for (bool enabled : {false, true}) {
    for (bool directFlush : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << "enabled=" << enabled << " directFlush=" << directFlush);
      fakeBackend() = FakeBackend{};
      NavyConfig config;
      config.setMemoryFile(64 * 1024 * 1024);
      config.setDeviceMetadataSize(1024 * 1024);
      config.setReaderAndWriterThreads(1, 1);
      config.setNavyReqOrderingShards(1);
      config.blockCache()
          .setRegionSize(4096)
          .setFlushCopyOffload(enabled)
          .setDirectFlush(directFlush);
      auto cache = facebook::cachelib::createNavyCache(
          config, {}, {}, true /* truncate */, nullptr,
          false /* itemDestructorEnabled */);
      ASSERT_NE(nullptr, cache);
      const auto expectedProbes = enabled && !directFlush ? 1u : 0u;
      EXPECT_EQ(expectedProbes, fakeBackend().submissions);
      EXPECT_EQ(expectedProbes, fakeBackend().completions);
    }
  }
}

TEST_F(ChecksumOffloadDtoTest, CopyOutOffloadsBlobsFromMinSize) {
  constexpr uint32_t kMinSize = 4096;
  LruAllocator::NvmCacheConfig nvmConfig;
  nvmConfig.navyConfig.setMemoryFile(64 * 1024 * 1024);
  nvmConfig.navyConfig.setDeviceMetadataSize(1024 * 1024);
  nvmConfig.navyConfig.setReaderAndWriterThreads(1, 1);
  nvmConfig.navyConfig.setNavyReqOrderingShards(1);
  nvmConfig.navyConfig.blockCache().setRegionSize(1024 * 1024);
  nvmConfig.copyOutOffload = true;
  nvmConfig.copyOutOffloadMinSize = kMinSize;
  nvmConfig.truncateItemToOriginalAllocSizeInNvm = true;
  LruAllocator::Config config;
  config.setCacheSize(20 * 1024 * 1024);
  config.enableNvmCache(nvmConfig);
  LruAllocator cache(config);
  // The startup self-check is the only device work so far.
  ASSERT_EQ(1u, fakeBackend().submissions);
  const auto pool = cache.addPool("default", 8 * 1024 * 1024);
  const auto insert = [&](const std::string& key, uint32_t size, uint8_t fill) {
    auto handle = cache.allocate(pool, key, size);
    ASSERT_NE(nullptr, handle);
    std::memset(handle->getMemory(), fill, size);
    cache.insertOrReplace(handle);
  };

  for (const auto [size, fill] :
       {std::pair<uint32_t, uint8_t>{kMinSize - 1, 0x5A}, {kMinSize, 0xA5}}) {
    SCOPED_TRACE(size);
    const std::string key = "copy-out-" + std::to_string(size);
    const std::vector<uint8_t> value(size, fill);
    insert(key, size, fill);
    // Drain insertOrReplace's flash delete so it cannot drop the eviction
    // write.
    cache.flushNvmCache();
    // Same-size fillers evict the key from DRAM, which writes it to flash.
    for (uint32_t i = 0; cache.peek(key) != nullptr; ++i) {
      ASSERT_LT(i, 10'000u);
      insert("filler-" + std::to_string(size) + "-" + std::to_string(i), size,
             0);
    }
    cache.flushNvmCache();

    const auto submissionsBefore = fakeBackend().submissions;
    auto promoted = cache.find(key);
    promoted.wait();
    ASSERT_NE(nullptr, promoted);
    EXPECT_TRUE(promoted.wentToNvm());
    EXPECT_EQ(size >= kMinSize ? 1u : 0u,
              fakeBackend().submissions - submissionsBefore);
    EXPECT_EQ(0, std::memcmp(value.data(), promoted->getMemory(), size));
  }
}

TEST_F(ChecksumOffloadDtoTest, NormalWaitYieldsToReadyFiber) {
  const std::vector<uint8_t> src{17, 18, 19, 20};
  const BufferView view{src.size(), src.data()};
  std::vector<uint8_t> dest(src.size());
  bool otherFiberRan = false;
  folly::EventBase evb;
  auto& fm = folly::fibers::getFiberManager(evb);
  fm.addTask([&] {
    AsyncChecksumOp op;
    op.submitCopyAndChecksum(dest.data(), view, true);
    EXPECT_EQ(checksum(view, ChecksumType::Crc32c), op.wait());
    EXPECT_TRUE(op.completedOnDevice());
    EXPECT_TRUE(otherFiberRan);
  });
  fm.addTask([&] { otherFiberRan = true; });
  evb.loop();

  EXPECT_EQ(src, dest);
  EXPECT_EQ(1u, fakeBackend().completions);
}

TEST_F(ChecksumOffloadDtoTest, UnwindingDrainsWithoutYieldingToReadyFiber) {
  const std::vector<uint8_t> src{21, 22, 23, 24};
  const BufferView view{src.size(), src.data()};
  std::vector<uint8_t> dest(src.size());
  bool otherFiberRan = false;
  folly::EventBase evb;
  auto& fm = folly::fibers::getFiberManager(evb);
  fm.addTask([&] {
    EXPECT_THROW(copyAndChecksum(dest.data(), view, failOverlap),
                 std::runtime_error);
    EXPECT_FALSE(otherFiberRan);
    EXPECT_EQ(1u, fakeBackend().completions);
    EXPECT_EQ(src, dest);
  });
  fm.addTask([&] { otherFiberRan = true; });
  evb.loop();

  EXPECT_TRUE(otherFiberRan);
  EXPECT_EQ(2u, fakeBackend().polls);
}

class ChecksumOffloadDtoDeathTest : public ChecksumOffloadDtoTest {};

TEST_F(ChecksumOffloadDtoDeathTest, ReentrantWaitIsRejected) {
  const std::vector<uint8_t> src{25, 26, 27, 28};
  const BufferView view{src.size(), src.data()};
  EXPECT_DEATH(
      {
        AsyncChecksumOp op;
        op.submitChecksum(view);
        fakeBackend().onPoll = [&] { op.wait(); };
        op.wait();
      },
      "single waiter");
}

TEST_F(ChecksumOffloadDtoDeathTest, SubmitWhileDeviceWorkIsPendingIsRejected) {
  const std::vector<uint8_t> src{29, 30, 31, 32};
  const BufferView view{src.size(), src.data()};
  EXPECT_DEATH(
      {
        AsyncChecksumOp op;
        op.submitChecksum(view);
        op.submitChecksum(view);
      },
      "idle AsyncChecksumOp");
}
TEST_F(ChecksumOffloadDtoDeathTest, DestructionWhileWaitingIsRejected) {
  const std::vector<uint8_t> src{33, 34, 35, 36};
  const BufferView view{src.size(), src.data()};
  EXPECT_DEATH(
      {
        auto op = std::make_unique<AsyncChecksumOp>();
        op->submitChecksum(view);
        fakeBackend().onPoll = [&] { op.reset(); };
        op->wait();
      },
      "destroyed while wait");
}

} // namespace facebook::cachelib::navy::tests
