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

#include <folly/Function.h>

#include "cachelib/navy/common/Buffer.h"

namespace facebook {
namespace cachelib {
namespace navy {

// Checksum (and fused copy+checksum) helpers that can offload to Intel DSA
// via the DTO library when built with CACHELIB_BUILD_WITH_DTO. Without DTO
// support (or when DSA is unavailable at runtime), they run equivalent
// software implementations. In all cases they return raw CRC-32C of @src
// (seed 0, no final inversion), matching
// checksum(src, ChecksumType::Crc32c).

// A single asynchronous checksum or fused copy+checksum operation.
// submit*() enqueues the operation on DSA and returns immediately (or runs
// it synchronously on the CPU when offload is unavailable); wait() completes
// it. While the accelerator works, wait() yields the calling fiber (when
// running on one, e.g. on a NavyThread) so other requests can execute on
// this thread — this is what makes the offload truly asynchronous. Off
// fibers it polls with a CPU pause.
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

  // Completes the operation and returns raw CRC-32C.
  uint32_t wait();

  // True iff the most recently waited operation was executed by the
  // accelerator. False when it ran on the CPU: submission refused (DTO size
  // gate, enqueue failure, non-DTO build) or a device failure redone in
  // software. Lets a self-check tell a working accelerator from a silently
  // failing one - both return the correct checksum.
  bool completedOnDevice() const { return onDevice_; }

 private:
  enum class State : uint8_t { kIdle, kCpuDone, kDsaPending, kWaiting };
  // What the submitted operation computes: a checksum only or a fused
  // copy plus checksum.
  enum class Kind : uint8_t { kCrc, kCopyCrc };

  void submitImpl(Kind kind, uint8_t* dest, BufferView src, bool cacheControl);
  uint32_t computeCpuResult() const;
  uint32_t waitImpl(bool allowFiberYield);

  State state_{State::kIdle};
  Kind kind_{Kind::kCrc};
  bool onDevice_{false};
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

// Copies @src to @dest and returns raw CRC-32C of @src, as a single fused
// DSA "Memory Copy with CRC Generation" operation when available. The copy
// lands toward the CPU cache (cache control) since such destinations are
// typically read again soon.
// @cacheControl steers the copy output toward the CPU cache; keep it on when
// the destination is read again by the CPU soon (in-memory lookup hits, a CPU
// flush copy), off when the next reader is a DMA engine.
//
// The @overlap callback is invoked exactly once after submission. Use it for
// CPU work that can proceed while the accelerator operates. It may read the
// source range but must not write it, and must not access the destination
// range at all.
// Note: when the operation falls back to software (non-DTO build, or DSA
// submission failure), the copy+checksum completes synchronously inside the
// submit step, so @overlap runs after the operation rather than overlapping
// it — the callback's constraints above still apply either way.
uint32_t copyAndChecksum(uint8_t* dest,
                         BufferView src,
                         folly::FunctionRef<void()> overlap,
                         bool cacheControl = true);

// Returns raw CRC-32C of @src, computed by DSA when available.
// @overlap runs once after submission and may read @src but must not modify it.
// With CPU fallback, it runs after the checksum completes.
uint32_t checksumWithOverlap(BufferView src,
                             folly::FunctionRef<void()> overlap);

// Diagnostics for checksum waits: cumulative fiber yields and
// pause-polls across all calls (process-wide).
struct CopyLargeWaitStats {
  uint64_t yields{0};
  uint64_t polls{0};
  uint64_t sleeps{0}; // waits that parked the thread (DTO blocking wait or
                      // nanosleep)
  uint64_t calls{0};
  uint64_t submitUs{0};  // time in submit (descriptor build + ENQCMD)
  uint64_t waitUs{0};    // time waiting for completion
  uint64_t fallbacks{0}; // device failures redone on the CPU (silent otherwise)
};
CopyLargeWaitStats getChecksumWaitStats();

} // namespace navy
} // namespace cachelib
} // namespace facebook
