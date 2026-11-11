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

#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

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
  unsigned int submissions{0};
  unsigned int polls{0};
  unsigned int completions{0};
  folly::Function<void()> onPoll;
};
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
  if (++pending.polls == 1) {
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
  while (__wrap_dto_async_poll(op) == DTO_ASYNC_PENDING) {
  }
}

uint32_t __wrap_dto_async_crc_val(const dto_async_op* op) {
  return std::launder(reinterpret_cast<const FakeOperation*>(op))->crc;
}

int __wrap_dto_wait_blocks() { return 0; }
} // extern "C"

namespace facebook::cachelib::navy::tests {
class ChecksumOffloadDtoTest : public testing::Test {
 protected:
  void SetUp() override {
    fakeBackend() = FakeBackend{};
    ASSERT_TRUE(checksumOffloadSupported());
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
