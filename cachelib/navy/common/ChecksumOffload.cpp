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

#include "cachelib/navy/common/ChecksumOffload.h"

#include <folly/fibers/FiberManager.h>
#include <folly/logging/xlog.h>
#include <folly/portability/Asm.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "cachelib/common/AtomicCounter.h"
#include "cachelib/navy/common/Hash.h"

#ifdef CACHELIB_BUILD_WITH_DTO
#include <dto.h>

#include <chrono>
#endif

namespace facebook {
namespace cachelib {
namespace navy {

namespace {
struct ChecksumCounters {
  TLCounter ops, yields, polls, waitUs, blocks, fallbacks;
};

ChecksumCounters& checksumCounters() {
  static ChecksumCounters counters;
  return counters;
}

bool copyRangesDoNotOverlap(const uint8_t* dest, BufferView src) {
  const auto d = reinterpret_cast<uintptr_t>(dest);
  const auto s = reinterpret_cast<uintptr_t>(src.data());
  return src.size() == 0 || (d < s ? s - d >= src.size() : d - s >= src.size());
}
} // namespace

#ifdef CACHELIB_BUILD_WITH_DTO
namespace {
// Submission flags for every descriptor this file sends. DTO_SUBMIT_CC steers
// the copy output toward the CPU cache, for destinations read again soon.
// DTO_SUBMIT_BOF asks the device to block on a page fault and finish the
// operation instead of failing it; DTO applies it only on work queues
// configured with block-on-fault (the device rejects it elsewhere), so on
// other queues a fault surfaces as DTO_ASYNC_FAILED and the operation is
// redone on the CPU below. Either way the result is correct; the difference
// is whether the device or the CPU pays for the fault.
unsigned int dtoFlags(bool cacheControl) {
  return (cacheControl ? DTO_SUBMIT_CC : 0u) | DTO_SUBMIT_BOF;
}

/* True when dto_async_wait() parks the thread rather than
 * spin, so handing it the wait actually idles the core. None of DTO's wait
 * methods parks today (busypoll, umwait, tpause and yield all keep the
 * thread), so this is 0 and the loops below pause-poll; a wait method that
 * does park would be used without changes here. Not cached: dto_wait_blocks()
 * reads a global that DTO's lazy init sets on the first submit, and a call
 * before that (a stats read, a test) would otherwise pin "does not block" for
 * the life of the process. The read is a single load. */
bool dtoWaitBlocks() { return dto_wait_blocks() != 0; }
} // namespace

static_assert(sizeof(dto_async_op) <= 192,
              "AsyncChecksumOp::opStorage_ too small for dto_async_op");
static_assert(alignof(dto_async_op) <= 64,
              "AsyncChecksumOp::opStorage_ under-aligned for dto_async_op");

bool checksumOffloadSupported() { return true; }

#else // !CACHELIB_BUILD_WITH_DTO

bool checksumOffloadSupported() { return false; }

#endif // CACHELIB_BUILD_WITH_DTO

AsyncChecksumOp::~AsyncChecksumOp() {
  // NOLINTNEXTLINE(facebook-hte-DetailCall): public logging macro
  XCHECK(state_ != State::kWaiting)
      << "AsyncChecksumOp cannot be destroyed while wait() is active";
  // The device owns the completion record and buffers until it finishes.
  // Do not yield to other fibers during destruction or exception unwinding.
  if (state_ == State::kDsaPending) {
    waitImpl(false);
  }
}

uint32_t AsyncChecksumOp::computeCpuResult() const {
  if (kind_ == Kind::kCopyCrc && src_.size() != 0) {
    std::memcpy(dest_, src_.data(), src_.size());
  }
  return checksum(src_, ChecksumType::Crc32c);
}

void AsyncChecksumOp::submitImpl(Kind kind,
                                 uint8_t* dest,
                                 BufferView src,
                                 bool cacheControl) {
  // NOLINTNEXTLINE(facebook-hte-DetailCall): public logging macro
  XCHECK(state_ == State::kIdle) << "submit requires an idle AsyncChecksumOp";
  dest_ = dest;
  src_ = src;
  kind_ = kind;

#ifdef CACHELIB_BUILD_WITH_DTO
  auto* op = reinterpret_cast<dto_async_op*>(opStorage_);
  const int rc =
      kind == Kind::kCopyCrc
          ? dto_submit_memcpy_crc(op, dest, src.data(), src.size(),
                                  dtoFlags(cacheControl))
          : dto_submit_crc(op, src.data(), src.size(), dtoFlags(false));
  if (rc == DTO_ASYNC_SUBMITTED) {
    state_ = State::kDsaPending;
    return;
  }
#else
  (void)cacheControl;
#endif
  onDevice_ = false;
  crc_ = computeCpuResult();
  state_ = State::kCpuDone;
}

uint32_t AsyncChecksumOp::wait() { return waitImpl(true); }

uint32_t AsyncChecksumOp::waitImpl(bool allowFiberYield) {
  // NOLINTNEXTLINE(facebook-hte-DetailCall): public logging macro
  XCHECK(state_ == State::kCpuDone || state_ == State::kDsaPending)
      << "wait() requires a submitted operation and a single waiter";
  if (state_ == State::kCpuDone) {
    state_ = State::kIdle;
    return crc_;
  }
  state_ = State::kWaiting;

#ifdef CACHELIB_BUILD_WITH_DTO
  auto* op = reinterpret_cast<dto_async_op*>(opStorage_);
  // A normal wait yields only when another fiber is ready. Destruction
  // drains without yielding, even if another request is runnable.
  auto* fm = allowFiberYield && folly::fibers::onFiber()
                 ? folly::fibers::FiberManager::getFiberManagerUnsafe()
                 : nullptr;
  int rc;
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t yields = 0, polls = 0, blocks = 0;
  while ((rc = dto_async_poll(op)) == DTO_ASYNC_PENDING) {
    if (fm && fm->hasReadyTasks()) {
      ++yields;
      folly::fibers::yield();
    } else if (dtoWaitBlocks()) {
      ++blocks;
      dto_async_wait(op);
    } else {
      ++polls;
      folly::asm_volatile_pause();
    }
  }
  auto& stats = checksumCounters();
  stats.blocks.add(blocks);
  stats.ops.inc();
  stats.yields.add(yields);
  stats.polls.add(polls);
  stats.waitUs.add(std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count());
  if (rc == DTO_ASYNC_DONE) {
    onDevice_ = true;
    crc_ = dto_async_crc_val(op);
  } else {
    // Failed copies may be partial; redo them before exposing the destination.
    onDevice_ = false;
    stats.fallbacks.inc();
    crc_ = computeCpuResult();
  }
#else
  (void)allowFiberYield;
  // NOLINTNEXTLINE(facebook-hte-DetailCall): public logging macro
  XLOG(FATAL) << "A non-DTO AsyncChecksumOp cannot have device work pending";
#endif
  state_ = State::kIdle;
  return crc_;
}

void AsyncChecksumOp::submitCopyAndChecksum(uint8_t* dest,
                                            BufferView src,
                                            bool cacheControl) {
  XDCHECK(dest);
  // NOLINTNEXTLINE(facebook-hte-DetailCall): public logging macro
  XDCHECK(copyRangesDoNotOverlap(dest, src))
      << "copy source and destination must not overlap";
  submitImpl(Kind::kCopyCrc, dest, src, cacheControl);
}

void AsyncChecksumOp::submitChecksum(BufferView src) {
  submitImpl(Kind::kCrc, nullptr, src, false);
}

CopyLargeWaitStats getChecksumWaitStats() {
  const auto& stats = checksumCounters();
  CopyLargeWaitStats st;
  st.yields = stats.yields.get();
  st.polls = stats.polls.get();
  st.sleeps = stats.blocks.get();
  st.calls = stats.ops.get();
  st.waitUs = stats.waitUs.get();
  st.fallbacks = stats.fallbacks.get();
  return st;
}

uint32_t copyAndChecksum(uint8_t* dest,
                         BufferView src,
                         folly::FunctionRef<void()> overlap,
                         bool cacheControl) {
  AsyncChecksumOp op;
  op.submitCopyAndChecksum(dest, src, cacheControl);
  overlap();
  return op.wait();
}

uint32_t checksumWithOverlap(BufferView src,
                             folly::FunctionRef<void()> overlap) {
  AsyncChecksumOp op;
  op.submitChecksum(src);
  overlap();
  return op.wait();
}

bool checksumOffloadSelfCheck() {
  if (!checksumOffloadSupported()) {
    return false;
  }
  // Exercise both operations on a buffer the size of a large value (explicit
  // DTO submits are not size-gated; Navy's checksumOffloadMinSize is the only
  // gate) and verify parity with CPU CRC-32C plus copy fidelity. If DSA is
  // unavailable, the CPU fallback must also match.
  constexpr size_t kSize = 1024 * 1024;
  std::vector<uint8_t> src(kSize);
  std::vector<uint8_t> dst(kSize, 0);
  std::mt19937 gen{12345};
  for (auto& b : src) {
    b = static_cast<uint8_t>(gen());
  }

  const BufferView view{src.size(), src.data()};
  const uint32_t sw = checksum(view, ChecksumType::Crc32c);

  // A device failure is redone on the CPU and still returns the right
  // checksum, so parity alone cannot tell a working accelerator from one that
  // rejects every descriptor (e.g. a WQ whose max_transfer_size is below the
  // request). Require that both operations actually completed on the device.
  AsyncChecksumOp crcOp;
  crcOp.submitChecksum(view);
  const uint32_t viaCrc = crcOp.wait();
  if (!crcOp.completedOnDevice()) {
    // NOLINTNEXTLINE(facebook-hte-DetailCall): public logging macro
    XLOG(WARN) << "checksumOffloadSelfCheck: CRC descriptor did not complete "
                  "on the device (submission refused or device failure); "
                  "offload would run on the CPU";
    return false;
  }
  AsyncChecksumOp copyOp;
  copyOp.submitCopyAndChecksum(dst.data(), view, true /* cacheControl */);
  const uint32_t viaCopy = copyOp.wait();
  if (!copyOp.completedOnDevice()) {
    // NOLINTNEXTLINE(facebook-hte-DetailCall): public logging macro
    XLOG(WARN) << "checksumOffloadSelfCheck: fused copy+CRC descriptor did not "
                  "complete on the device; offload would run on the CPU";
    return false;
  }
  if (viaCrc != sw || viaCopy != sw) {
    // NOLINTNEXTLINE(facebook-hte-DetailCall): public logging macro
    XLOGF(WARN,
          "checksumOffloadSelfCheck: accelerator checksum disagrees with CPU "
          "(cpu {:#x} crc {:#x} copy+crc {:#x})",
          sw, viaCrc, viaCopy);
    return false;
  }
  if (std::memcmp(dst.data(), src.data(), kSize) != 0) {
    // NOLINTNEXTLINE(facebook-hte-DetailCall): public logging macro
    XLOG(WARN) << "checksumOffloadSelfCheck: fused copy is not faithful";
    return false;
  }
  return true;
}

} // namespace navy
} // namespace cachelib
} // namespace facebook
