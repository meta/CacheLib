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
// software implementations. In all cases the returned checksum is
// navy::checksum() (raw CRC-32C, seed 0) of @src, so offloaded and
// CPU-computed checksums verify against each other interchangeably.

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
// with exactly one wait() before reuse or destruction.
class AsyncChecksumOp {
 public:
  AsyncChecksumOp() = default;
  AsyncChecksumOp(const AsyncChecksumOp&) = delete;
  AsyncChecksumOp& operator=(const AsyncChecksumOp&) = delete;
  ~AsyncChecksumOp();

  // Copies @src to @dest and computes checksum(src) as one fused DSA
  // operation. @dest must not overlap @src and must not be accessed until
  // wait() returns: on a (rare) accelerator failure the copy is redone on
  // the CPU, so intermediate destination contents are unspecified.
  // @cacheControl directs the DSA copy output toward the CPU cache; use it
  // when the destination will be read again soon (e.g. an in-memory region
  // buffer that serves lookups and is flushed to the device shortly after).
  void submitCopyAndChecksum(uint8_t* dest, BufferView src, bool cacheControl);

  // Computes checksum(src). @src may be read concurrently (e.g. to rebuild
  // a bloom filter) but must not be modified until wait() returns.
  void submitChecksum(BufferView src);

  // Plain copy of @src to @dest as a DSA Memory Move (no checksum); wait()
  // returns 0. Same destination contract as submitCopyAndChecksum. Used for
  // the flash-hit copy-out (Navy read buffer -> DRAM item) where Navy has
  // already verified the value checksum.
  void submitCopy(uint8_t* dest, BufferView src, bool cacheControl);

  // Completes the submitted operation and returns checksum(src).
  uint32_t wait();

  // True iff the most recently waited operation was executed by the
  // accelerator. False when it ran on the CPU: submission refused (DTO size
  // gate, enqueue failure, non-DTO build) or a device failure redone in
  // software. Lets a self-check tell a working accelerator from a silently
  // failing one - both return the correct checksum.
  bool completedOnDevice() const { return onDevice_; }

 private:
  enum class State : uint8_t { kIdle, kCpuDone, kDsaPending };
  // What the submitted operation computes: a checksum only, a fused copy
  // plus checksum, or a plain copy.
  enum class Kind : uint8_t { kCrc, kCopyCrc, kCopy };

  void submitImpl(Kind kind, uint8_t* dest, BufferView src, bool cacheControl);

  State state_{State::kIdle};
  Kind kind_{Kind::kCrc};
  bool onDevice_{false};
  uint32_t crc_{0};
  uint8_t* dest_{nullptr};
  BufferView src_;
  // Opaque storage for the DTO async operation (descriptor + completion
  // record); sized/aligned to hold dto_async_op without exposing dto.h here.
  alignas(64) unsigned char opStorage_[192];
};

// Convenience wrappers over AsyncChecksumOp. The @overlap callback is
// invoked exactly once after submission. Use it for CPU work that can
// proceed while the accelerator operates. It may read the source range but
// must not write it, and must not access the destination range at all.
// Note: when the operation falls back to software (non-DTO build, or DSA
// submission failure), the copy+checksum completes synchronously inside the
// submit step, so @overlap runs after the operation rather than overlapping
// it — the callback's constraints above still apply either way.

// Returns true if this binary was built with DTO/DSA support.
bool checksumOffloadSupported();

// Verifies at runtime that the DTO/DSA checksum matches navy::checksum() and
// that the fused copy is faithful. Returns true iff offload is usable and
// consistent; returns false when built without DTO support. Callers should
// enable offload only if this returns true.
bool checksumOffloadSelfCheck();

// Copies @src to @dest and returns the checksum of @src, as a single fused
// DSA "Memory Copy with CRC Generation" operation when available. The copy
// lands toward the CPU cache (cache control) since such destinations are
// typically read again soon.
// @cacheControl steers the copy output toward the CPU cache; keep it on when
// the destination is read again by the CPU soon (in-memory lookup hits, a CPU
// flush copy), off when the next reader is a DMA engine.
uint32_t copyAndChecksum(uint8_t* dest,
                         BufferView src,
                         folly::FunctionRef<void()> overlap,
                         bool cacheControl = true);

// Returns the checksum of @src, computed by DSA when available.
uint32_t checksumWithOverlap(BufferView src,
                             folly::FunctionRef<void()> overlap);

/**
 * Up to kMax checksum / copy+checksum operations submitted as ONE DSA batch
 * descriptor (one ENQCMD, one completion record to poll). Caller-owned, like
 * AsyncChecksumOp; results are read back per member after poll()/wait().
 * Members the device failed are repaired on the CPU before wait() returns
 * (copy redone, CRC computed in software), so crc(i) is always valid.
 */
class BatchChecksumOp {
 public:
  static constexpr size_t kMax = 64;

  BatchChecksumOp();
  BatchChecksumOp(const BatchChecksumOp&) = delete;
  BatchChecksumOp& operator=(const BatchChecksumOp&) = delete;
  ~BatchChecksumOp();

  // Returns the member's index. Must not be called when full().
  size_t addChecksum(BufferView src);
  size_t addCopyAndChecksum(uint8_t* dest, BufferView src, bool cacheControl);

  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  bool full() const { return count_ == kMax; }

  // Submits everything added so far. Returns true when the device took the
  // batch (poll()/wait() required), false when it was done on the CPU
  // instead (results already available).
  bool submit();
  // True once complete. After completion every member has a valid crc().
  bool poll();
  // Waits like AsyncChecksumOp::wait(): yields to ready fibers, blocks
  // through DTO when its wait method parks, otherwise pause-polls.
  void wait();

  uint32_t crc(size_t i) const { return crc_[i]; }
  bool completedOnDevice() const { return onDevice_; }
  // Members the device failed and the CPU redid (0 when all completed).
  size_t fallbacks() const { return fallbacks_; }
  // Reuse for another batch once complete.
  void reset();

 private:
  struct Member {
    uint8_t* dest{nullptr};
    BufferView src;
    bool copy{false};
    bool cacheControl{false};
  };
  void finish();

  Member members_[kMax];
  uint32_t crc_[kMax]{};
  size_t count_{0};
  size_t fallbacks_{0};
  bool submitted_{false};
  bool onDevice_{false};
  bool done_{false};
  void* op_{nullptr}; // dto_batch*, owned; created with capacity kMax
};

/**
 * Fiber-coalesced checksum: when called on a fiber, the request joins the
 * batch that this NavyThread is currently filling, yields once so sibling
 * fibers can add theirs, and the fiber that finds the batch still open after
 * its yield submits it for everyone (one descriptor and one wait per batch;
 * the others park on fiber batons). The yield happens only when the fiber
 * manager has runnable fibers - with nothing to coalesce the request is
 * submitted at once, as one member, which DTO sends as a plain descriptor.
 * Off a fiber this is exactly checksumWithOverlap / copyAndChecksum. The
 * caller decides whether to use it (BlockCache::Config::checksumOffloadBatch).
 */
uint32_t checksumBatched(BufferView src, folly::FunctionRef<void()> overlap);
uint32_t copyAndChecksumBatched(uint8_t* dest,
                                BufferView src,
                                folly::FunctionRef<void()> overlap,
                                bool cacheControl = true);

struct ChecksumBatchStats {
  uint64_t batches{0};   // batches submitted (device or CPU)
  uint64_t members{0};   // operations carried by those batches
  uint64_t singles{0};   // batches that ended up with one member
  uint64_t leaders{0};   // requests that submitted and waited for a batch
  uint64_t waiters{0};   // requests that parked on a baton instead
  uint64_t deferred{0};  // members computed at region flush (BlockCache)
  uint64_t fallbacks{0}; // members the device failed, redone on the CPU
};
ChecksumBatchStats getChecksumBatchStats();
void addDeferredChecksumStats(uint64_t members);

// Plain copy offload (no checksum). copyWithOffload() copies @n bytes from
// @src to @dest, on DSA when the build has DTO support, the size passes DTO's
// gate and submission succeeds, otherwise with memcpy; it returns true iff
// the accelerator performed the copy. The destination pages should be
// resident (or the WQ configured with block-on-fault): a device page fault
// fails the descriptor and the copy is redone on the CPU. Yields the calling
// fiber while the device works, like the checksum ops.
bool copyWithOffload(uint8_t* dest, const uint8_t* src, size_t n);

// Runtime check that a DSA copy is faithful; false when built without DTO or
// when DSA is unusable. Callers should enable copy offload only if true.
// @size is the largest copy the caller will submit: a work queue's maximum
// transfer size (a WQ attribute, 2 MiB by default on some configurations)
// rejects bigger descriptors with XFER_ERANGE, so a self-check at a smaller
// size would pass and every real copy would then fail on the device.
bool copyOffloadSelfCheck(size_t size = 1024 * 1024);

// Large copy offload (e.g. a 16 MiB region buffer). With @parts == 1 the
// range is one DSA Memory Move descriptor; with @parts > 1 it is split into
// consecutive pieces submitted as ONE DSA Batch descriptor, so the pieces run
// in parallel on the engines of the work queue's device. Not every device
// implements the Batch opcode (DSA 1.0 does not): if the batch submit is
// refused, the copy degrades to a single descriptor before falling back to
// memcpy. Waits like the checksum ops (yields the fiber when others are
// runnable, else pause-polls). @cacheControl=false keeps the destination out
// of the CPU caches, right for buffers the device DMAs next.
//
// Return value: true iff the copy was submitted to DSA and completed. For
// @parts == 1 that means the device did the whole copy (a device failure is
// redone with memcpy and returns false). For @parts > 1 DTO repairs any piece
// the device failed on the CPU inside its poll and reports only success, so
// "true" attributes the batch to DSA even if some pieces were repaired; use
// parts == 1 where exact attribution matters. Destination pages should be
// resident (pooled, pre-touched buffers) or the WQ block-on-fault.
// How to wait for a large copy. kSpinOrYield is the checksum ops' policy
// (yield to runnable fibers, else pause-poll) and burns the thread for the
// whole device latency when no fiber is runnable. kSleep nanosleeps the
// calling thread for @sleepPollUs between completion checks (the CPU idles;
// the thread's other fibers wait at most one interval); right for flush-type
// copies whose latency is not critical and whose thread has little else
// queued.
enum class LargeCopyWait : uint8_t { kSpinOrYield, kSleep };

bool copyLargeWithOffload(uint8_t* dest,
                          const uint8_t* src,
                          size_t n,
                          size_t parts = 1,
                          bool cacheControl = false,
                          LargeCopyWait wait = LargeCopyWait::kSpinOrYield,
                          uint32_t sleepPollUs = 200);

// Diagnostics for copyLargeWithOffload waits: cumulative fiber yields and
// pause-polls across all calls (process-wide).
struct CopyLargeWaitStats {
  uint64_t yields{0};
  uint64_t polls{0};
  uint64_t sleeps{0};  // waits that parked the thread (DTO blocking wait or nanosleep)
  uint64_t calls{0};
  uint64_t submitUs{0}; // time in submit (descriptor build + ENQCMD)
  uint64_t waitUs{0};   // time waiting for completion
  uint64_t fallbacks{0}; // device failures redone on the CPU (silent otherwise)
};
CopyLargeWaitStats getCopyLargeWaitStats();
// Same shape for the checksum ops (6.1/6.2) and the flash-hit copy-out (6.3):
// calls, yields, polls and total wait time, to size what a blocking wait
// (e.g. a shared completion poller) could recover.
CopyLargeWaitStats getChecksumWaitStats();
CopyLargeWaitStats getCopyOutWaitStats();

} // namespace navy
} // namespace cachelib
} // namespace facebook
