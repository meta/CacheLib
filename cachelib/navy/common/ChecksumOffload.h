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

#pragma once

#include <folly/Range.h>

#include <array>
#include <magic_enum/magic_enum.hpp>

#include "cachelib/common/AtomicCounter.h"
#include "cachelib/navy/common/Buffer.h"

namespace facebook {
namespace cachelib {
namespace util {
class CounterVisitor;
}
namespace navy {

enum class OffloadOutcome : uint8_t {
  OFFLOADED,
  BELOW_MIN,
  SUBMIT_FALLBACK,
  COMPLETION_FALLBACK,
};

class OffloadStats {
 public:
  void record(OffloadOutcome outcome, size_t bytes) {
    ops_[*magic_enum::enum_index(outcome)].inc();
    if (outcome == OffloadOutcome::OFFLOADED) {
      offloadedBytes_.add(bytes);
    }
  }

  void getCounters(const util::CounterVisitor& visitor,
                   folly::StringPiece prefix,
                   bool hasSizeCutoff = true) const;

 private:
  std::array<TLCounter, magic_enum::enum_count<OffloadOutcome>()> ops_;
  TLCounter offloadedBytes_;
};

// How AsyncChecksumOp::wait() waits for the device. kSpinOrYield yields to
// ready fibers, otherwise pause-polls. kSleep suspends EventBase-backed fibers
// on a microsecond timer, or sleeps the thread off fibers. Other fiber loop
// controllers and timer resource failures use the fiber timer's resolution.
// Wakeups can be delayed by scheduling. kSleep suits latency-insensitive flush
// copies.
enum class LargeCopyWait : uint8_t { kSpinOrYield, kSleep };

// A single asynchronous checksum, fused copy+checksum, or plain copy.
// submit*() enqueues the operation on DSA and returns immediately (or runs
// it synchronously on the CPU when offload is unavailable); wait() completes
// it. While the accelerator works, wait() yields or suspends the calling
// fiber (e.g. on a NavyThread) so other requests can execute on this thread.
// Off fibers it pause-polls or sleeps according to the wait policy.
//
// The object is not copyable or movable: the device holds a pointer into
// its storage while the operation is in flight. Each submit must be paired
// with exactly one wait() before reuse. Destruction drains pending operations
// without yielding fibers. The object is single-owner and not thread-safe;
// overlapping submit/wait calls and re-entrant waiters are invalid and
// terminate in all builds.
class AsyncChecksumOp {
 public:
  AsyncChecksumOp() = default;
  AsyncChecksumOp(const AsyncChecksumOp&) = delete;
  AsyncChecksumOp& operator=(const AsyncChecksumOp&) = delete;
  AsyncChecksumOp(AsyncChecksumOp&&) = delete;
  AsyncChecksumOp& operator=(AsyncChecksumOp&&) = delete;
  ~AsyncChecksumOp();

  // Copies @src to @dest and computes raw CRC-32C as one fused DSA
  // operation. @dest must not overlap @src and must not be accessed until
  // wait() returns: on a (rare) accelerator failure the copy is redone on
  // the CPU, so intermediate destination contents are unspecified.
  // @cacheControl directs the DSA copy output toward the CPU cache; use it
  // when the destination will be read again soon (e.g. an in-memory region
  // buffer that serves lookups and is flushed to the device shortly after).
  void submitCopyAndChecksum(uint8_t* dest, BufferView src, bool cacheControl);

  // Computes raw CRC-32C. @src may be read concurrently (e.g. to rebuild
  // a bloom filter) but must not be modified until wait() returns.
  void submitChecksum(BufferView src);

  // Plain copy of @src to @dest as a DSA Memory Move (no checksum); wait()
  // returns 0. Same destination contract as submitCopyAndChecksum.
  void submitCopy(uint8_t* dest, BufferView src, bool cacheControl);

  // Completes the operation and returns raw CRC-32C (seed 0, no final
  // inversion, matching checksum(src, ChecksumType::Crc32c)), or 0 for copy.
  uint32_t wait(LargeCopyWait policy = LargeCopyWait::kSpinOrYield,
                uint32_t sleepPollUs = 200);

  // True iff the most recently waited operation was executed by the
  // accelerator. False when it ran on the CPU: submission refused (DTO size
  // gate, enqueue failure, non-DTO build) or a device failure redone in
  // software. Lets a self-check tell a working accelerator from a silently
  // failing one - both return the correct checksum.
  bool completedOnDevice() const { return onDevice_; }

  // Result of the most recent operation after wait(). Size skips are accounted
  // for by callers and never reach this helper.
  OffloadOutcome outcome() const {
    if (onDevice_) {
      return OffloadOutcome::OFFLOADED;
    }
    return submitted_ ? OffloadOutcome::COMPLETION_FALLBACK
                      : OffloadOutcome::SUBMIT_FALLBACK;
  }

 private:
  enum class State : uint8_t { kIdle, kCpuDone, kDsaPending, kWaiting };
  // What the submitted operation computes: a checksum only, a fused copy
  // plus checksum, or a plain copy.
  enum class Kind : uint8_t { kCrc, kCopyCrc, kCopy };

  void submitImpl(Kind kind, uint8_t* dest, BufferView src, bool cacheControl);
  uint32_t computeCpuResult() const;
  uint32_t waitImpl(bool allowFiberYield,
                    LargeCopyWait policy = LargeCopyWait::kSpinOrYield,
                    uint32_t sleepPollUs = 0);

  State state_{State::kIdle};
  Kind kind_{Kind::kCrc};
  bool onDevice_{false};
  bool submitted_{false};
  uint32_t crc_{0};
  [[maybe_unused]] uint8_t* dest_{nullptr};
  BufferView src_;
  // Opaque storage for the DTO async operation (descriptor + completion
  // record); sized/aligned to hold dto_async_op without exposing dto.h here.
  [[maybe_unused]] alignas(64) unsigned char opStorage_[192]{};
};

// Returns true if this binary was built with DTO/DSA support.
bool checksumOffloadSupported();

// Verifies at runtime that the DTO/DSA checksum matches CPU CRC-32C and
// that the fused copy is faithful. Returns true iff offload is usable and
// consistent; returns false when built without DTO support. Callers should
// enable offload only if this returns true.
bool checksumOffloadSelfCheck();

// Runtime check that a @size-byte DSA copy completes on the device and is
// faithful; false when built without DTO or when DSA is unusable. Callers
// should enable copy offload only if true. DTO does not submit copies larger
// than the work queue's max_transfer_size and runs them on the CPU instead, so
// pass the size whose offload you want to confirm: a smaller probe can pass
// while the caller's copies never reach the device.
bool copyOffloadSelfCheck(size_t size = 1024 * 1024);

// Diagnostics for checksum waits: cumulative fiber yields and
// pause-polls across all calls (process-wide).
struct CopyLargeWaitStats {
  uint64_t yields{0};
  uint64_t polls{0};
  uint64_t sleeps{0}; // timed fiber suspensions or blocking thread waits
  uint64_t calls{0};
  uint64_t submitUs{0};  // time in submit (descriptor build + ENQCMD)
  uint64_t waitUs{0};    // time waiting for completion
  uint64_t fallbacks{0}; // device failures redone on the CPU (silent otherwise)
};
CopyLargeWaitStats getChecksumWaitStats();

} // namespace navy
} // namespace cachelib
} // namespace facebook
