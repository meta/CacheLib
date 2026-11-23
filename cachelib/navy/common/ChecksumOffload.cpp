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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "cachelib/common/AtomicCounter.h"
#include "cachelib/navy/common/Hash.h"

#ifdef CACHELIB_BUILD_WITH_DTO
#include <dto.h>
#include <folly/Indestructible.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/EventBaseLoopController.h>
#include <folly/io/async/EventBaseLocal.h>
#include <folly/io/async/EventHandler.h>
#include <folly/io/async/TimerFDTimeoutManager.h>

#include <chrono>
#include <memory>
#include <thread>
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

struct PollWakeup final : folly::TimerFDTimeoutManager::Callback {
  void timeoutExpired() noexcept override { baton.post(); }
  void callbackCanceled() noexcept override {}

  folly::fibers::Baton baton;
};

std::unique_ptr<folly::TimerFDTimeoutManager> makePollTimer(
    folly::EventBase& eventBase) {
  auto timer = std::make_unique<folly::TimerFDTimeoutManager>(&eventBase);
  // The persistent descriptor must not keep an idle event loop alive.
  if (!timer->isHandlerRegistered() ||
      !timer->registerInternalHandler(folly::EventHandler::READ |
                                      folly::EventHandler::PERSIST)) {
    return nullptr;
  }
  return timer;
}

folly::TimerFDTimeoutManager* getPollTimer(folly::fibers::FiberManager& fm) {
  auto* controller = dynamic_cast<folly::fibers::EventBaseLoopController*>(
      &fm.loopController());
  if (!controller || !controller->getEventBase()) {
    return nullptr;
  }

  using TimerPtr = std::unique_ptr<folly::TimerFDTimeoutManager>;
  static folly::Indestructible<folly::EventBaseLocal<TimerPtr>> timers;
  auto& eventBase = controller->getEventBase()->getEventBase();
  const auto& timer = timers->try_emplace_with(
      eventBase, [&] { return makePollTimer(eventBase); });
  return timer.get();
}

void sleepFiber(folly::fibers::FiberManager& fm,
                std::chrono::microseconds interval) {
  interval = std::max(interval, std::chrono::microseconds{1});
  auto* timer = getPollTimer(fm);
  PollWakeup wakeup;
  if (timer) {
    timer->scheduleTimeout(&wakeup, interval);
  }
  // Folly's scheduleTimeout() does not expose timerfd_settime() failures.
  // The Baton timeout wakes the fiber if the timerfd never fires.
  (void)wakeup.baton.try_wait_for(
      std::chrono::ceil<std::chrono::milliseconds>(interval) +
      std::chrono::milliseconds{1});
  if (timer) {
    timer->cancelTimeout(&wakeup);
  }
}

struct DtoWaitResult {
  int status;
  CopyLargeWaitStats stats;
};

// Destruction drains without yielding or suspending, even if another request
// is runnable.
template <typename Poll, typename Wait>
DtoWaitResult waitForDtoCompletion(Poll poll,
                                   Wait wait,
                                   bool allowFiberYield,
                                   LargeCopyWait policy,
                                   uint32_t sleepPollUs) {
  auto* fm = allowFiberYield && folly::fibers::onFiber()
                 ? folly::fibers::FiberManager::getFiberManagerUnsafe()
                 : nullptr;
  const auto interval = std::chrono::microseconds(sleepPollUs);
  const auto start = std::chrono::steady_clock::now();
  DtoWaitResult result{};
  while ((result.status = poll()) == DTO_ASYNC_PENDING) {
    if (fm && policy == LargeCopyWait::kSleep) {
      ++result.stats.sleeps;
      sleepFiber(*fm, interval);
    } else if (fm && fm->hasReadyTasks()) {
      ++result.stats.yields;
      folly::fibers::yield();
    } else if (dtoWaitBlocks()) {
      ++result.stats.sleeps;
      wait();
    } else if (policy == LargeCopyWait::kSleep) {
      ++result.stats.sleeps;
      std::this_thread::sleep_for(interval);
    } else {
      ++result.stats.polls;
      folly::asm_volatile_pause();
    }
  }
  result.stats.calls = 1;
  result.stats.waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
  return result;
}

void recordChecksumWait(const CopyLargeWaitStats& wait) {
  auto& stats = checksumCounters();
  stats.ops.inc();
  stats.yields.add(wait.yields);
  stats.polls.add(wait.polls);
  stats.blocks.add(wait.sleeps);
  stats.waitUs.add(wait.waitUs);
}
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
  if (kind_ != Kind::kCrc && src_.size() != 0) {
    std::memcpy(dest_, src_.data(), src_.size());
  }
  return kind_ == Kind::kCopy ? 0 : checksum(src_, ChecksumType::Crc32c);
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
  int rc;
  switch (kind) {
  case Kind::kCopyCrc:
    rc = dto_submit_memcpy_crc(op, dest, src.data(), src.size(),
                               dtoFlags(cacheControl));
    break;
  case Kind::kCopy:
    rc = dto_submit_memcpy(op, dest, src.data(), src.size(),
                           dtoFlags(cacheControl));
    break;
  case Kind::kCrc:
  default:
    rc = dto_submit_crc(op, src.data(), src.size(), dtoFlags(false));
    break;
  }
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

uint32_t AsyncChecksumOp::wait(LargeCopyWait policy, uint32_t sleepPollUs) {
  return waitImpl(true, policy, sleepPollUs);
}

uint32_t AsyncChecksumOp::waitImpl(bool allowFiberYield,
                                   LargeCopyWait policy,
                                   uint32_t sleepPollUs) {
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
  const auto result = waitForDtoCompletion(
      [&] { return dto_async_poll(op); }, [&] { dto_async_wait(op); },
      allowFiberYield, policy, sleepPollUs);
  // The checksum counters describe checksum operations only.
  const bool checksumOp = kind_ != Kind::kCopy;
  if (checksumOp) {
    recordChecksumWait(result.stats);
  }
  onDevice_ = result.status == DTO_ASYNC_DONE;
  if (onDevice_) {
    crc_ = checksumOp ? dto_async_crc_val(op) : 0;
  } else {
    // Failed copies may be partial; redo them before exposing the destination.
    if (checksumOp) {
      checksumCounters().fallbacks.inc();
    }
    crc_ = computeCpuResult();
  }
#else
  (void)allowFiberYield;
  (void)policy;
  (void)sleepPollUs;
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

void AsyncChecksumOp::submitCopy(uint8_t* dest,
                                 BufferView src,
                                 bool cacheControl) {
  XDCHECK(dest);
  // NOLINTNEXTLINE(facebook-hte-DetailCall): public logging macro
  XDCHECK(copyRangesDoNotOverlap(dest, src))
      << "copy source and destination must not overlap";
  submitImpl(Kind::kCopy, dest, src, cacheControl);
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

bool copyOffloadSelfCheck(size_t size) {
  if (!checksumOffloadSupported()) {
    return false;
  }
  const size_t probeSize = std::max<size_t>(size, 4096);
  std::vector<uint8_t> src(probeSize);
  std::vector<uint8_t> dst(probeSize, 0);
  std::mt19937 gen{54321};
  for (auto& b : src) {
    b = static_cast<uint8_t>(gen());
  }
  AsyncChecksumOp op;
  op.submitCopy(dst.data(), BufferView{probeSize, src.data()}, true);
  op.wait();
  if (!op.completedOnDevice() ||
      std::memcmp(dst.data(), src.data(), probeSize) != 0) {
    // NOLINTNEXTLINE(facebook-hte-DetailCall): public logging macro
    XLOGF(WARN,
          "copyOffloadSelfCheck: a {}-byte DSA Memory Move did not complete on "
          "the device or did not verify; check block_on_fault and "
          "max_transfer_size in the work queue configuration",
          probeSize);
    return false;
  }
  return true;
}

} // namespace navy
} // namespace cachelib
} // namespace facebook
