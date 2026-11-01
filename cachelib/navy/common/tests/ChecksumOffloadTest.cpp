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
} // namespace

// Sizes from a few bytes to 1 MiB, so both the DSA path and the internal CPU
// fallbacks (sizes a work queue refuses, device failures) are exercised when
// built with DTO support.
const size_t kSizes[] = {1, 13, 175, 4096, 16 * 1024, 64 * 1024, 1024 * 1024};

TEST(ChecksumOffload, CopyAndChecksumMatchesSoftware) {
  uint32_t seed = 1;
  for (auto size : kSizes) {
    auto src = randomBytes(size, seed++);
    std::vector<uint8_t> dst(size, 0xAA);
    const BufferView view{src.size(), src.data()};

    const uint32_t cs = copyAndChecksum(dst.data(), view, [] {});
    EXPECT_EQ(checksum(view), cs) << "size=" << size;
    EXPECT_EQ(0, std::memcmp(src.data(), dst.data(), size)) << "size=" << size;
  }
}

TEST(ChecksumOffload, ChecksumWithOverlapMatchesSoftware) {
  uint32_t seed = 100;
  for (auto size : kSizes) {
    auto src = randomBytes(size, seed++);
    const BufferView view{src.size(), src.data()};
    EXPECT_EQ(checksum(view), checksumWithOverlap(view, [] {}))
        << "size=" << size;
  }
}

TEST(ChecksumOffload, OverlapRunsExactlyOnce) {
  for (auto size : {175UL, 1024UL * 1024UL}) {
    auto src = randomBytes(size, 7);
    std::vector<uint8_t> dst(size, 0);
    const BufferView view{src.size(), src.data()};

    int copyOverlapRuns = 0;
    copyAndChecksum(dst.data(), view, [&] { copyOverlapRuns++; });
    EXPECT_EQ(1, copyOverlapRuns) << "size=" << size;

    int crcOverlapRuns = 0;
    checksumWithOverlap(view, [&] { crcOverlapRuns++; });
    EXPECT_EQ(1, crcOverlapRuns) << "size=" << size;
  }
}

// The overlap callback is allowed to read the source range concurrently with
// the operation (e.g. BigHash rebuilds its bloom filter from the bucket while
// DSA checksums it).
TEST(ChecksumOffload, OverlapMayReadSource) {
  const size_t size = 64 * 1024;
  auto src = randomBytes(size, 11);
  const BufferView view{src.size(), src.data()};

  uint64_t sum = 0;
  const uint32_t cs = checksumWithOverlap(view, [&] {
    for (auto b : src) {
      sum += b;
    }
  });
  EXPECT_EQ(checksum(view), cs);
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
  EXPECT_EQ(checksum(v2), op2.wait());
  EXPECT_EQ(checksum(v1), op1.wait());
  EXPECT_EQ(0, std::memcmp(src1.data(), dst1.data(), size));

  // An op object is reusable after wait().
  op1.submitChecksum(v2);
  EXPECT_EQ(checksum(v2), op1.wait());
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

  EXPECT_EQ(checksum(view), got);
  EXPECT_TRUE(otherFiberRan);
  EXPECT_EQ(0, std::memcmp(src.data(), dst.data(), size));
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
    return;
  }
  // Built with DTO. The self-check now requires the accelerator to actually
  // execute the descriptors (a CPU fallback returns the right checksum and
  // used to pass it). Probe first so a host with no usable work queue skips
  // rather than fails; a host where the device runs must agree with
  // navy::checksum, or offloaded checksums would not verify against CPU ones.
  const auto src = randomBytes(1024 * 1024, 12345);
  AsyncChecksumOp probe;
  probe.submitChecksum(BufferView{src.size(), src.data()});
  probe.wait();
  if (!probe.completedOnDevice()) {
    GTEST_SKIP() << "built with DTO but no usable DSA work queue on this host";
  }
  EXPECT_TRUE(checksumOffloadSelfCheck());
}

TEST(ChecksumOffload, CompletedOnDeviceIsTruthful) {
  // The parity of the result cannot distinguish a working accelerator from a
  // silently failing one; completedOnDevice() must.
  const auto src = randomBytes(1024 * 1024, 777);
  std::vector<uint8_t> dst(src.size(), 0);
  const BufferView view{src.size(), src.data()};
  AsyncChecksumOp op;
  op.submitCopyAndChecksum(dst.data(), view, true /* cacheControl */);
  EXPECT_EQ(checksum(view), op.wait());
  EXPECT_EQ(0, std::memcmp(dst.data(), src.data(), src.size()));
  if (!checksumOffloadSupported()) {
    EXPECT_FALSE(op.completedOnDevice());
  } else if (checksumOffloadSelfCheck()) {
    // A passing self-check means 1 MiB descriptors run on the device here.
    EXPECT_TRUE(op.completedOnDevice());
  }
}

TEST(ChecksumOffload, CopyLargeDegradesWithoutBatch) {
  // parts > 1 needs the DSA Batch opcode, which DSA 1.0 lacks; the copy must
  // be correct on every path (batch, single-descriptor degrade, memcpy).
  const auto src = randomBytes(1024 * 1024, 779);
  for (size_t parts : {size_t{1}, size_t{4}, size_t{64}}) {
    std::vector<uint8_t> dst(src.size(), 0);
    copyLargeWithOffload(dst.data(), src.data(), src.size(), parts);
    EXPECT_EQ(0, std::memcmp(dst.data(), src.data(), src.size()))
        << "parts=" << parts;
  }
  if (checksumOffloadSupported() && copyOffloadSelfCheck()) {
    // The single-descriptor path the flush uses: reports device completion
    // and does not touch the device-fallback counter.
    const auto before = getCopyLargeWaitStats().fallbacks;
    std::vector<uint8_t> dst(src.size(), 0);
    EXPECT_TRUE(copyLargeWithOffload(dst.data(), src.data(), src.size(), 1));
    EXPECT_EQ(0, std::memcmp(dst.data(), src.data(), src.size()));
    EXPECT_EQ(before, getCopyLargeWaitStats().fallbacks);
  }
}

TEST(ChecksumOffload, WaitStatsExposeFallbacks) {
  // Device failures redone on the CPU are counted, and never exceed the
  // number of operations that reached the device path.
  const auto csum = getChecksumWaitStats();
  const auto copyOut = getCopyOutWaitStats();
  const auto large = getCopyLargeWaitStats();
  EXPECT_LE(csum.fallbacks, csum.calls);
  EXPECT_LE(copyOut.fallbacks, copyOut.calls);
  EXPECT_LE(large.fallbacks, large.calls);
}

// BatchChecksumOp: every member's crc(i) equals navy::checksum() of its
// source, copy members land faithfully, failed/absent device members are
// repaired on the CPU - with or without a work queue.
TEST(ChecksumOffload, BatchChecksumOpParity) {
  std::mt19937 gen{777};
  std::vector<std::vector<uint8_t>> srcs;
  std::vector<std::vector<uint8_t>> dsts;
  const size_t sizes[] = {13, 4096, 16 * 1024, 65 * 1024, 300 * 1024};
  for (size_t s : sizes) {
    srcs.emplace_back(s);
    for (auto& b : srcs.back()) {
      b = static_cast<uint8_t>(gen());
    }
    dsts.emplace_back(s, 0);
  }
  BatchChecksumOp op;
  for (size_t i = 0; i < srcs.size(); ++i) {
    BufferView v{srcs[i].size(), srcs[i].data()};
    if (i % 2 == 0) {
      op.addChecksum(v);
    } else {
      op.addCopyAndChecksum(dsts[i].data(), v, true);
    }
  }
  EXPECT_EQ(srcs.size(), op.size());
  op.submit();
  op.wait();
  for (size_t i = 0; i < srcs.size(); ++i) {
    BufferView v{srcs[i].size(), srcs[i].data()};
    EXPECT_EQ(checksum(v), op.crc(i)) << "member " << i;
    if (i % 2 == 1) {
      EXPECT_EQ(0, std::memcmp(dsts[i].data(), srcs[i].data(), srcs[i].size()))
          << "copy member " << i;
    }
  }
  // reset + reuse up to kMax members
  op.reset();
  std::vector<uint8_t> big(64 * 1024);
  for (auto& b : big) {
    b = static_cast<uint8_t>(gen());
  }
  for (size_t i = 0; i < BatchChecksumOp::kMax; ++i) {
    op.addChecksum(BufferView{big.size(), big.data()});
  }
  EXPECT_TRUE(op.full());
  op.submit();
  op.wait();
  const uint32_t expected = checksum(BufferView{big.size(), big.data()});
  for (size_t i = 0; i < BatchChecksumOp::kMax; ++i) {
    EXPECT_EQ(expected, op.crc(i)) << "member " << i;
  }
}

// checksumBatched / copyAndChecksumBatched return navy::checksum() both off a
// fiber (plain path) and on fibers sharing a thread (coalesced path).
TEST(ChecksumOffload, BatchedCallsMatchSoftware) {
  std::mt19937 gen{4242};
  std::vector<uint8_t> src(96 * 1024);
  for (auto& b : src) {
    b = static_cast<uint8_t>(gen());
  }
  BufferView v{src.size(), src.data()};
  const uint32_t expected = checksum(v);
  EXPECT_EQ(expected, checksumBatched(v, [] {}));
  std::vector<uint8_t> dst(src.size(), 0);
  EXPECT_EQ(expected, copyAndChecksumBatched(dst.data(), v, [] {}, true));
  EXPECT_EQ(0, std::memcmp(dst.data(), src.data(), src.size()));

  folly::EventBase evb;
  auto& fm = folly::fibers::getFiberManager(evb);
  std::vector<uint32_t> got(8, 0);
  std::vector<std::vector<uint8_t>> dsts(8, std::vector<uint8_t>(src.size()));
  for (size_t i = 0; i < 8; ++i) {
    fm.addTask([&, i] {
      got[i] = copyAndChecksumBatched(dsts[i].data(), v, [] {}, true);
    });
  }
  evb.loop();
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(expected, got[i]) << "fiber " << i;
    EXPECT_EQ(0, std::memcmp(dsts[i].data(), src.data(), src.size()));
  }
}
} // namespace facebook::cachelib::navy::tests
