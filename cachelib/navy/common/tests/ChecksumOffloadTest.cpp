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

#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <type_traits>
#include <vector>

#include "cachelib/navy/common/ChecksumOffload.h"
#include "cachelib/navy/common/Hash.h"

namespace facebook::cachelib::navy::tests {
namespace {
std::vector<uint8_t> randomBytes(size_t size, uint32_t seed) {
  std::mt19937 gen{seed};
  std::vector<uint8_t> bytes(size);
  for (auto& b : bytes) {
    b = static_cast<uint8_t>(gen());
  }
  return bytes;
}

void expectChecksumStatsUnchanged(const CopyLargeWaitStats& before,
                                  const CopyLargeWaitStats& after) {
  EXPECT_EQ(before.calls, after.calls);
  EXPECT_EQ(before.fallbacks, after.fallbacks);
  EXPECT_EQ(before.yields, after.yields);
  EXPECT_EQ(before.polls, after.polls);
  EXPECT_EQ(before.sleeps, after.sleeps);
  EXPECT_EQ(before.waitUs, after.waitUs);
}
} // namespace

// Sizes from a few bytes to 1 MiB. These checks apply both without DTO
// support and when a DTO build uses DSA or its CPU fallbacks.
const size_t kSizes[] = {1, 13, 175, 4096, 16 * 1024, 64 * 1024, 1024 * 1024};

TEST(ChecksumOffload, PlainCopyMatchesSoftware) {
  uint32_t seed = 500;
  for (auto size : kSizes) {
    SCOPED_TRACE(size);
    const auto src = randomBytes(size, seed++);
    std::vector<uint8_t> dst(size, 0xAA);
    AsyncChecksumOp op;
    op.submitCopy(dst.data(), BufferView{src.size(), src.data()}, true);
    EXPECT_EQ(0u, op.wait());
    EXPECT_EQ(0, std::memcmp(src.data(), dst.data(), size));
    if (!checksumOffloadSupported()) {
      EXPECT_FALSE(op.completedOnDevice());
    }
  }
}

TEST(ChecksumOffload, PlainCopyOpIsReusable) {
  const size_t size = 64 * 1024;
  AsyncChecksumOp op;
  for (uint32_t seed : {501u, 502u}) {
    SCOPED_TRACE(seed);
    const auto src = randomBytes(size, seed);
    const BufferView view{src.size(), src.data()};
    std::vector<uint8_t> dst(size, 0xAA);
    // Reuse an object that just computed a checksum: a plain copy returns
    // zero and does not count as checksum work.
    op.submitChecksum(view);
    EXPECT_EQ(checksum(view, ChecksumType::Crc32c), op.wait());
    const auto checksumBefore = getChecksumWaitStats();
    op.submitCopy(dst.data(), view, true /* cacheControl */);
    EXPECT_EQ(0u, op.wait());
    EXPECT_EQ(0, std::memcmp(src.data(), dst.data(), size));
    expectChecksumStatsUnchanged(checksumBefore, getChecksumWaitStats());
    if (!checksumOffloadSupported()) {
      EXPECT_FALSE(op.completedOnDevice());
    }
  }
}

TEST(ChecksumOffload, SleepWaitCopiesFaithfully) {
  const auto src = randomBytes(1024 * 1024, 503);
  std::vector<uint8_t> dst(src.size(), 0);
  AsyncChecksumOp op;
  op.submitCopy(dst.data(), BufferView{src.size(), src.data()},
                false /* cacheControl */);
  EXPECT_EQ(0u, op.wait(LargeCopyWait::kSleep, 50 /* us */));
  EXPECT_EQ(0, std::memcmp(src.data(), dst.data(), src.size()));
}

TEST(ChecksumOffload, EmptyCopiesDoNotReportDeviceWork) {
  const uint8_t src = 0x5A;
  uint8_t dst = 0xA5;
  const auto checksumBefore = getChecksumWaitStats();
  AsyncChecksumOp op;
  op.submitCopy(&dst, BufferView{0, &src}, true /* cacheControl */);
  EXPECT_EQ(0u, op.wait());
  EXPECT_FALSE(op.completedOnDevice());
  EXPECT_EQ(0xA5, dst);
  expectChecksumStatsUnchanged(checksumBefore, getChecksumWaitStats());
}

TEST(ChecksumOffload, CopyAndChecksumMatchesSoftware) {
  uint32_t seed = 1;
  for (auto size : kSizes) {
    auto src = randomBytes(size, seed++);
    std::vector<uint8_t> dst(size, 0xAA);
    const BufferView view{src.size(), src.data()};

    AsyncChecksumOp op;
    op.submitCopyAndChecksum(dst.data(), view, true);
    const uint32_t cs = op.wait();
    EXPECT_EQ(checksum(view, ChecksumType::Crc32c), cs) << "size=" << size;
    EXPECT_EQ(0, std::memcmp(src.data(), dst.data(), size)) << "size=" << size;
  }
}

TEST(ChecksumOffload, ChecksumMatchesSoftware) {
  uint32_t seed = 100;
  for (auto size : kSizes) {
    auto src = randomBytes(size, seed++);
    const BufferView view{src.size(), src.data()};
    AsyncChecksumOp op;
    op.submitChecksum(view);
    EXPECT_EQ(checksum(view, ChecksumType::Crc32c), op.wait())
        << "size=" << size;
  }
}

// BigHash can read the source to rebuild its bloom filter while DSA checksums
// the bucket.
TEST(ChecksumOffload, OverlapMayReadSource) {
  const size_t size = 64 * 1024;
  auto src = randomBytes(size, 11);
  const BufferView view{src.size(), src.data()};

  AsyncChecksumOp op;
  op.submitChecksum(view);
  uint64_t sum = 0;
  for (auto b : src) {
    sum += b;
  }
  EXPECT_EQ(checksum(view, ChecksumType::Crc32c), op.wait());
  EXPECT_NE(0ULL, sum);
}

// Multiple async operations in flight from one thread, out-of-order waits.
TEST(ChecksumOffload, AsyncOpsInFlight) {
  const size_t size = 64 * 1024;
  auto src1 = randomBytes(size, 21);
  auto src2 = randomBytes(size, 22);
  std::vector<uint8_t> dst1(size, 0);
  const BufferView v1{src1.size(), src1.data()};
  const BufferView v2{src2.size(), src2.data()};

  AsyncChecksumOp op1;
  AsyncChecksumOp op2;
  op1.submitCopyAndChecksum(dst1.data(), v1, true /* cacheControl */);
  op2.submitChecksum(v2);

  // Complete in reverse submission order.
  EXPECT_EQ(checksum(v2, ChecksumType::Crc32c), op2.wait());
  EXPECT_EQ(checksum(v1, ChecksumType::Crc32c), op1.wait());
  EXPECT_EQ(0, std::memcmp(src1.data(), dst1.data(), size));

  // An op object is reusable after wait().
  op1.submitChecksum(v2);
  EXPECT_EQ(checksum(v2, ChecksumType::Crc32c), op1.wait());
}

// wait() on a fiber takes the yield path: other fibers run on the thread
// while the operation is in flight.
TEST(ChecksumOffload, AsyncOpOnFiber) {
  const size_t size = 1024 * 1024;
  auto src = randomBytes(size, 27);
  std::vector<uint8_t> dst(size, 0);
  const BufferView view{src.size(), src.data()};

  folly::EventBase evb;
  auto& fm = folly::fibers::getFiberManager(evb);
  uint32_t got = 0;
  bool otherFiberRan = false;
  fm.addTask([&] {
    AsyncChecksumOp op;
    op.submitCopyAndChecksum(dst.data(), view, true /* cacheControl */);
    got = op.wait();
  });
  fm.addTask([&] { otherFiberRan = true; });
  evb.loop();

  EXPECT_EQ(checksum(view, ChecksumType::Crc32c), got);
  EXPECT_TRUE(otherFiberRan);
  EXPECT_EQ(0, std::memcmp(src.data(), dst.data(), size));
}

TEST(ChecksumOffload, AsyncOpRequiresDestruction) {
  EXPECT_FALSE(std::is_trivially_destructible_v<AsyncChecksumOp>);
}

// Destructor drains an in-flight op (does not leave the device writing to
// freed stack memory).
TEST(ChecksumOffload, AsyncOpDrainOnDestroy) {
  const size_t size = 256 * 1024;
  auto src = randomBytes(size, 33);
  std::vector<uint8_t> dst(size, 0);
  {
    AsyncChecksumOp op;
    op.submitCopyAndChecksum(dst.data(), BufferView{src.size(), src.data()},
                             false /* cacheControl */);
    // no wait(): destructor must drain
  }
  EXPECT_EQ(0, std::memcmp(src.data(), dst.data(), size));
}

TEST(ChecksumOffload, SelfCheck) {
  if (!checksumOffloadSupported()) {
    EXPECT_FALSE(checksumOffloadSelfCheck());
    EXPECT_FALSE(copyOffloadSelfCheck());
    return;
  }
  // Built with DTO. The self-check now requires the accelerator to actually
  // execute the descriptors (a CPU fallback returns the right checksum and
  // used to pass it). Probe first so a host with no usable work queue skips
  // rather than fails; a host where the device runs must agree with
  // CPU CRC-32C, or offloaded checksums would not verify against CPU ones.
  const auto src = randomBytes(1024 * 1024, 12345);
  AsyncChecksumOp probe;
  probe.submitChecksum(BufferView{src.size(), src.data()});
  probe.wait();
  if (!probe.completedOnDevice()) {
    GTEST_SKIP() << "built with DTO but no usable DSA work queue on this host";
  }
  EXPECT_TRUE(checksumOffloadSelfCheck());
  EXPECT_TRUE(copyOffloadSelfCheck());
}

TEST(ChecksumOffload, CompletedOnDeviceIsTruthful) {
  // The parity of the result cannot distinguish a working accelerator from a
  // silently failing one; completedOnDevice() must.
  const auto src = randomBytes(1024 * 1024, 777);
  std::vector<uint8_t> dst(src.size(), 0);
  const BufferView view{src.size(), src.data()};
  AsyncChecksumOp op;
  op.submitCopyAndChecksum(dst.data(), view, true /* cacheControl */);
  EXPECT_EQ(checksum(view, ChecksumType::Crc32c), op.wait());
  EXPECT_EQ(0, std::memcmp(dst.data(), src.data(), src.size()));
  if (!checksumOffloadSupported()) {
    EXPECT_FALSE(op.completedOnDevice());
  }

  // DTO rejects empty submissions, giving reuse a deterministic CPU fallback.
  op.submitChecksum(BufferView{0, nullptr});
  EXPECT_EQ(0u, op.wait());
  EXPECT_FALSE(op.completedOnDevice());
}

TEST(ChecksumOffload, WaitStatsExposeFallbacks) {
  // Device failures redone on the CPU are counted, and never exceed the
  // number of operations that reached the device path.
  const auto csum = getChecksumWaitStats();
  EXPECT_LE(csum.fallbacks, csum.calls);
}

TEST(ChecksumOffload, CopyToAdjacentRanges) {
  constexpr size_t kSize = 8;
  for (bool destBeforeSource : {false, true}) {
    auto buffer = randomBytes(2 * kSize, 91);
    auto* src = buffer.data() + (destBeforeSource ? kSize : 0);
    auto* dest = buffer.data() + (destBeforeSource ? 0 : kSize);
    const std::vector<uint8_t> expected(src, src + kSize);
    const BufferView view{kSize, src};
    const auto expectedCrc = checksum(view, ChecksumType::Crc32c);

    AsyncChecksumOp op;
    op.submitCopyAndChecksum(dest, view, true);
    EXPECT_EQ(expectedCrc, op.wait());
    EXPECT_EQ(expected, std::vector<uint8_t>(dest, dest + kSize));
  }
}

TEST(ChecksumOffload, EmptyCopyPreservesDestination) {
  uint8_t dest = 0x5A;
  AsyncChecksumOp op;
  op.submitCopyAndChecksum(&dest, BufferView{0, nullptr}, true);
  EXPECT_EQ(0u, op.wait());
  EXPECT_EQ(0x5A, dest);
}

TEST(ChecksumOffloadDeathTest, WaitRequiresSubmittedOperation) {
  EXPECT_DEATH(
      {
        AsyncChecksumOp op;
        op.wait();
      },
      "single waiter");
}

TEST(ChecksumOffloadDeathTest, SubmitRequiresPreviousWait) {
  EXPECT_DEATH(
      {
        AsyncChecksumOp op;
        op.submitChecksum(BufferView{0, nullptr});
        op.submitChecksum(BufferView{0, nullptr});
      },
      "idle AsyncChecksumOp");
}

TEST(ChecksumOffloadDeathTest, DoubleWaitIsRejected) {
  EXPECT_DEATH(
      {
        AsyncChecksumOp op;
        op.submitChecksum(BufferView{0, nullptr});
        op.wait();
        op.wait();
      },
      "single waiter");
}

#ifndef NDEBUG
TEST(ChecksumOffloadDeathTest, CopyRejectsSameBuffer) {
  auto buffer = randomBytes(16, 92);
  const BufferView view{8, buffer.data()};
  AsyncChecksumOp op;
  EXPECT_DEATH(op.submitCopyAndChecksum(buffer.data(), view, true),
               "must not overlap");
}

TEST(ChecksumOffloadDeathTest, CopyRejectsPartialOverlap) {
  auto buffer = randomBytes(16, 93);
  AsyncChecksumOp op;
  EXPECT_DEATH(op.submitCopyAndChecksum(buffer.data() + 1,
                                        BufferView{8, buffer.data()}, true),
               "must not overlap");
  EXPECT_DEATH(op.submitCopyAndChecksum(buffer.data(),
                                        BufferView{8, buffer.data() + 1}, true),
               "must not overlap");
}
#endif

} // namespace facebook::cachelib::navy::tests
