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

#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManager.h>
#include <folly/logging/xlog.h>
#include <folly/portability/Asm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "cachelib/navy/common/Hash.h"

#ifdef CACHELIB_BUILD_WITH_DTO
#include <dto.h>
#endif

namespace facebook {
namespace cachelib {
namespace navy {

#ifdef CACHELIB_BUILD_WITH_DTO
namespace {
// Storage for the one-shot op used by copyWithOffload(). It lives for the
// duration of one call on the calling thread/fiber; a fiber that yields keeps
// running on the same NavyThread, and only one call is active per fiber at a
// time, but distinct fibers on one thread could interleave, so the storage
// is per call (stack), not thread-local.
struct alignas(64) OpStorage {
  unsigned char bytes[sizeof(dto_async_op)];
};

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

/* True when dto_async_wait()/dto_batch_wait() park the thread rather than
 * spin, so handing them the wait actually idles the core. None of DTO's wait
 * methods parks today (busypoll, umwait, tpause and yield all keep the
 * thread), so this is 0 and the loops below pause-poll; a wait method that
 * does park would be used without changes here. Not cached: dto_wait_blocks()
 * reads a global that DTO's lazy init sets on the first submit, and a call
 * before that (a stats read, a test) would otherwise pin "does not block" for
 * the life of the process. The read is a single load. */
bool dtoWaitBlocks() { return dto_wait_blocks() != 0; }

// 6.1/6.2 checksum ops and 6.3 copy-out: how much of the wait is spent spinning
std::atomic<uint64_t> gCsumOps{0}, gCsumYields{0}, gCsumPolls{0}, gCsumWaitUs{0};
std::atomic<uint64_t> gCopyOps{0}, gCopyYields{0}, gCopyPolls{0}, gCopyWaitUs{0};
std::atomic<uint64_t> gCsumBlocks{0}, gCopyBlocks{0}, gLargeBlocks{0};
// Device failures redone on the CPU. Without these the only evidence of a
// dead accelerator is DTO's per-op stderr line: the results stay correct.
std::atomic<uint64_t> gCsumFallbacks{0}, gCopyFallbacks{0}, gLargeFallbacks{0};
} // namespace

static_assert(sizeof(dto_async_op) <= 192,
              "AsyncChecksumOp::opStorage_ too small for dto_async_op");
static_assert(alignof(dto_async_op) <= 64,
              "AsyncChecksumOp::opStorage_ under-aligned for dto_async_op");

bool checksumOffloadSupported() { return true; }

AsyncChecksumOp::~AsyncChecksumOp() {
  // A submitted DSA operation writes to opStorage_ (completion record) and,
  // for copies, to dest_. It must be drained before this object dies.
  if (state_ == State::kDsaPending) {
    wait();
  }
}

void AsyncChecksumOp::submitImpl(Kind kind,
                                 uint8_t* dest,
                                 BufferView src,
                                 bool cacheControl) {
  XDCHECK(state_ == State::kIdle);
  dest_ = dest;
  src_ = src;
  kind_ = kind;

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
  // DTO_ASYNC_FALLBACK: nothing submitted or copied; run on CPU now.
  onDevice_ = false;
  if (kind != Kind::kCrc) {
    std::memcpy(dest, src.data(), src.size());
  }
  crc_ = kind == Kind::kCopy ? 0 : checksum(src);
  state_ = State::kCpuDone;
}

uint32_t AsyncChecksumOp::wait() {
  XDCHECK(state_ != State::kIdle);
  if (state_ == State::kCpuDone) {
    state_ = State::kIdle;
    return crc_;
  }

  auto* op = reinterpret_cast<dto_async_op*>(opStorage_);
  // Yielding suspends this fiber and lets other request fibers run on the
  // same NavyThread while the accelerator works; that is the actual
  // "free the writer thread" mechanism. Yield ONLY when another fiber is
  // ready to run: with a lone fiber, yield() returns immediately and the
  // loop would busy-spin through the FiberManager at full rate for the
  // whole accelerator operation, which is far more expensive than a pause
  // poll. Outside fiber context (plain thread-pool schedulers, tests) this
  // reduces to a pause-poll.
  auto* fm = folly::fibers::onFiber()
                 ? folly::fibers::FiberManager::getFiberManagerUnsafe()
                 : nullptr;
  int rc;
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t yields = 0, polls = 0, blocks = 0;
  while ((rc = dto_async_poll(op)) == DTO_ASYNC_PENDING) {
    if (fm && fm->hasReadyTasks()) {
      // Another request on this thread can run: switching to it is always
      // better than giving the core back, and it is what keeps a blocking
      // wait from stalling a NavyThread's other fibers.
      ++yields;
      folly::fibers::yield();
    } else if (dtoWaitBlocks()) {
      // Nothing else to run here and DTO's wait method parks the thread, so
      // hand the wait to the library. intel/DTO 1.2 never reports this
      // (dto_wait_blocks() is 0: busypoll, umwait, tpause and yield all keep
      // the thread), so today the pause-poll branch below runs instead.
      // dto_async_wait() returns only once the status byte is written, so
      // the enclosing poll settles the result on the next turn.
      ++blocks;
      dto_async_wait(op);
    } else {
      ++polls;
      folly::asm_volatile_pause();
    }
  }
  gCsumBlocks.fetch_add(blocks, std::memory_order_relaxed);
  gCsumOps.fetch_add(1, std::memory_order_relaxed);
  gCsumYields.fetch_add(yields, std::memory_order_relaxed);
  gCsumPolls.fetch_add(polls, std::memory_order_relaxed);
  gCsumWaitUs.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count(),
                        std::memory_order_relaxed);
  state_ = State::kIdle;
  if (rc == DTO_ASYNC_DONE) {
    onDevice_ = true;
    return kind_ == Kind::kCopy ? 0 : dto_async_crc_val(op);
  }
  // Accelerator failure: destination contents are unspecified, so redo the
  // whole operation on the CPU. dest_ is not yet visible to readers per the
  // submit contract, so overwriting is safe.
  onDevice_ = false;
  gCsumFallbacks.fetch_add(1, std::memory_order_relaxed);
  if (kind_ != Kind::kCrc) {
    std::memcpy(dest_, src_.data(), src_.size());
  }
  return kind_ == Kind::kCopy ? 0 : checksum(src_);
}

#else // !CACHELIB_BUILD_WITH_DTO

bool checksumOffloadSupported() { return false; }

AsyncChecksumOp::~AsyncChecksumOp() = default;

void AsyncChecksumOp::submitImpl(Kind kind,
                                 uint8_t* dest,
                                 BufferView src,
                                 bool /* cacheControl */) {
  XDCHECK(state_ == State::kIdle);
  kind_ = kind;
  onDevice_ = false;
  if (kind != Kind::kCrc) {
    std::memcpy(dest, src.data(), src.size());
  }
  crc_ = kind == Kind::kCopy ? 0 : checksum(src);
  state_ = State::kCpuDone;
}

uint32_t AsyncChecksumOp::wait() {
  XDCHECK(state_ == State::kCpuDone);
  state_ = State::kIdle;
  return crc_;
}

#endif // CACHELIB_BUILD_WITH_DTO

void AsyncChecksumOp::submitCopyAndChecksum(uint8_t* dest,
                                            BufferView src,
                                            bool cacheControl) {
  XDCHECK(dest);
  submitImpl(Kind::kCopyCrc, dest, src, cacheControl);
}

void AsyncChecksumOp::submitChecksum(BufferView src) {
  submitImpl(Kind::kCrc, nullptr, src, false);
}

void AsyncChecksumOp::submitCopy(uint8_t* dest,
                                 BufferView src,
                                 bool cacheControl) {
  XDCHECK(dest);
  submitImpl(Kind::kCopy, dest, src, cacheControl);
}

bool copyWithOffload(uint8_t* dest, const uint8_t* src, size_t n) {
#ifdef CACHELIB_BUILD_WITH_DTO
  OpStorage storage;
  auto* op = reinterpret_cast<dto_async_op*>(storage.bytes);
  // Cache control on: the DRAM item is about to be handed to the caller and
  // read (served) right away.
  const int rc =
      dto_submit_memcpy(op, dest, src, n, dtoFlags(true /* cacheControl */));
  if (rc != DTO_ASYNC_SUBMITTED) {
    std::memcpy(dest, src, n);
    return false;
  }
  auto* fm = folly::fibers::onFiber()
                 ? folly::fibers::FiberManager::getFiberManagerUnsafe()
                 : nullptr;
  int st;
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t yields = 0, polls = 0, blocks = 0;
  while ((st = dto_async_poll(op)) == DTO_ASYNC_PENDING) {
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
  gCopyBlocks.fetch_add(blocks, std::memory_order_relaxed);
  gCopyOps.fetch_add(1, std::memory_order_relaxed);
  gCopyYields.fetch_add(yields, std::memory_order_relaxed);
  gCopyPolls.fetch_add(polls, std::memory_order_relaxed);
  gCopyWaitUs.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count(),
                        std::memory_order_relaxed);
  if (st == DTO_ASYNC_DONE) {
    return true;
  }
  gCopyFallbacks.fetch_add(1, std::memory_order_relaxed);
  std::memcpy(dest, src, n);
  return false;
#else
  std::memcpy(dest, src, n);
  return false;
#endif
}

namespace {
std::atomic<uint64_t> gCopyLargeYields{0};
std::atomic<uint64_t> gCopyLargePolls{0};
std::atomic<uint64_t> gCopyLargeSleeps{0};
std::atomic<uint64_t> gCopyLargeCalls{0};
std::atomic<uint64_t> gCopyLargeSubmitUs{0};
std::atomic<uint64_t> gCopyLargeWaitUs{0};
} // namespace

// ---------------------------------------------------------------------------
// Batched checksums
// ---------------------------------------------------------------------------
namespace {
std::atomic<uint64_t> gBatchBatches{0}, gBatchMembers{0}, gBatchSingles{0},
    gBatchLeaders{0}, gBatchWaiters{0}, gBatchDeferred{0}, gBatchFallbacks{0};
} // namespace

BatchChecksumOp::BatchChecksumOp() {
#ifdef CACHELIB_BUILD_WITH_DTO
  // One DTO batch of kMax members, reused across reset(): the descriptor
  // list and completion records are allocated once per BatchChecksumOp.
  op_ = dto_batch_create(static_cast<int>(kMax));
#endif
}

BatchChecksumOp::~BatchChecksumOp() {
  if (submitted_ && !done_) {
    wait();
  }
#ifdef CACHELIB_BUILD_WITH_DTO
  if (op_) {
    dto_batch_destroy(static_cast<dto_batch*>(op_));
  }
#endif
}

size_t BatchChecksumOp::addChecksum(BufferView src) {
  XDCHECK(!full());
  XDCHECK(!submitted_);
  members_[count_] = Member{nullptr, src, false, false};
  return count_++;
}

size_t BatchChecksumOp::addCopyAndChecksum(uint8_t* dest,
                                           BufferView src,
                                           bool cacheControl) {
  XDCHECK(!full());
  XDCHECK(!submitted_);
  XDCHECK(dest);
  members_[count_] = Member{dest, src, true, cacheControl};
  return count_++;
}

void BatchChecksumOp::reset() {
  XDCHECK(!submitted_ || done_);
  count_ = 0;
  fallbacks_ = 0;
  submitted_ = false;
  onDevice_ = false;
  done_ = false;
}

bool BatchChecksumOp::submit() {
  XDCHECK(!submitted_);
  submitted_ = true;
  gBatchBatches.fetch_add(1, std::memory_order_relaxed);
  gBatchMembers.fetch_add(count_, std::memory_order_relaxed);
  if (count_ == 1) {
    gBatchSingles.fetch_add(1, std::memory_order_relaxed);
  }
#ifdef CACHELIB_BUILD_WITH_DTO
  // A zero-length member makes DTO refuse the whole batch, and an empty
  // buffer has nothing to accelerate anyway: such batches go to the CPU.
  bool allNonEmpty = count_ > 0;
  for (size_t i = 0; i < count_ && allNonEmpty; ++i) {
    allNonEmpty = members_[i].src.size() > 0;
  }
  if (op_ && allNonEmpty) {
    auto* b = static_cast<dto_batch*>(op_);
    dto_batch_reset(b);
    bool added = true;
    for (size_t i = 0; i < count_ && added; ++i) {
      const Member& m = members_[i];
      const int idx =
          m.copy ? dto_batch_add_memcpy_crc(b, m.dest, m.src.data(),
                                            m.src.size(),
                                            dtoFlags(m.cacheControl))
                 : dto_batch_add_crc(b, m.src.data(), m.src.size(),
                                     dtoFlags(false));
      added = idx == static_cast<int>(i);
    }
    // A single member goes out as a plain descriptor; more than the work
    // queue's max_batch_size, or a member above its max_transfer_size, is
    // refused with nothing submitted.
    if (added && dto_batch_submit(b) == DTO_ASYNC_SUBMITTED) {
      onDevice_ = true;
      return true;
    }
  }
#endif
  // Refused (no DTO, no WQ, oversized member, submit failure): CPU for all.
  for (size_t i = 0; i < count_; ++i) {
    if (members_[i].copy) {
      std::memcpy(members_[i].dest, members_[i].src.data(),
                  members_[i].src.size());
    }
    crc_[i] = checksum(members_[i].src);
  }
  onDevice_ = false;
  done_ = true;
  return false;
}

void BatchChecksumOp::finish() {
#ifdef CACHELIB_BUILD_WITH_DTO
  auto* b = static_cast<dto_batch*>(op_);
  for (size_t i = 0; i < count_; ++i) {
    // Once dto_batch_poll reports DONE every result is valid: members the
    // device did not finish were redone on the CPU inside DTO, copy and CRC
    // alike. dto_batch_status() is the device's status for the member, 1
    // when the device did it; anything else was a CPU repair.
    crc_[i] = dto_batch_crc(b, static_cast<int>(i));
    if (dto_batch_status(b, static_cast<int>(i)) != 1) {
      ++fallbacks_;
    }
  }
  if (fallbacks_) {
    gBatchFallbacks.fetch_add(fallbacks_, std::memory_order_relaxed);
    gCsumFallbacks.fetch_add(fallbacks_, std::memory_order_relaxed);
  }
#endif
  done_ = true;
}

bool BatchChecksumOp::poll() {
  if (done_) {
    return true;
  }
#ifdef CACHELIB_BUILD_WITH_DTO
  if (dto_batch_poll(static_cast<dto_batch*>(op_)) == DTO_ASYNC_PENDING) {
    return false;
  }
  finish();
  return true;
#else
  done_ = true;
  return true;
#endif
}

void BatchChecksumOp::wait() {
  if (done_) {
    return;
  }
#ifdef CACHELIB_BUILD_WITH_DTO
  auto* op = static_cast<dto_batch*>(op_);
  // Same policy as AsyncChecksumOp::wait(): yield when a sibling fiber is
  // runnable, park through DTO when its wait method blocks, else pause-poll.
  auto* fm = folly::fibers::onFiber()
                 ? folly::fibers::FiberManager::getFiberManagerUnsafe()
                 : nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t yields = 0, polls = 0, blocks = 0;
  while (dto_batch_poll(op) == DTO_ASYNC_PENDING) {
    if (fm && fm->hasReadyTasks()) {
      ++yields;
      folly::fibers::yield();
    } else if (dtoWaitBlocks()) {
      ++blocks;
      dto_batch_wait(op);
    } else {
      ++polls;
      folly::asm_volatile_pause();
    }
  }
  // One wait per batch, in the same counters the per-op path uses, so
  // csum_wait_ops / polls / us keep describing what the thread really did.
  gCsumOps.fetch_add(1, std::memory_order_relaxed);
  gCsumYields.fetch_add(yields, std::memory_order_relaxed);
  gCsumPolls.fetch_add(polls, std::memory_order_relaxed);
  gCsumBlocks.fetch_add(blocks, std::memory_order_relaxed);
  gCsumWaitUs.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count(),
                        std::memory_order_relaxed);
  finish();
#else
  done_ = true;
#endif
}

namespace {
// One batch being filled by the fibers of a NavyThread. Everything below runs
// on that thread only (fibers are cooperatively scheduled), so no locking.
struct OpenBatch {
  BatchChecksumOp op;
  folly::fibers::Baton batons[BatchChecksumOp::kMax];
  bool submitted{false};
  size_t outstanding{0}; // members that have not read their result yet
};

struct ThreadBatcher {
  OpenBatch* open{nullptr};
  std::vector<std::unique_ptr<OpenBatch>> pool;

  OpenBatch* acquire() {
    if (pool.empty()) {
      return new OpenBatch();
    }
    auto* b = pool.back().release();
    pool.pop_back();
    return b;
  }
  void release(OpenBatch* b) {
    b->op.reset();
    b->submitted = false;
    for (auto& baton : b->batons) {
      baton.reset();
    }
    pool.emplace_back(b);
  }
  ~ThreadBatcher() {
    delete open;
  }
};

thread_local ThreadBatcher tlBatcher;

uint32_t batched(uint8_t* dest,
                 BufferView src,
                 bool copy,
                 bool cacheControl,
                 folly::FunctionRef<void()> overlap) {
  if (!folly::fibers::onFiber() || src.size() == 0) {
    return copy ? copyAndChecksum(dest, src, overlap, cacheControl)
                : checksumWithOverlap(src, overlap);
  }
  auto& tb = tlBatcher;
  if (tb.open == nullptr) {
    tb.open = tb.acquire();
  }
  OpenBatch* b = tb.open;
  const size_t idx = copy ? b->op.addCopyAndChecksum(dest, src, cacheControl)
                          : b->op.addChecksum(src);
  ++b->outstanding;
  overlap();
  if (!b->op.full()) {
    // Give sibling fibers one scheduler turn to add their requests - but only
    // when there are any runnable: an unconditional yield cost +2-4% CPU and
    // +1 us insert p50 on 24-48 client threads, where a sibling was ready
    // 1.5-17% of the time. Alone, submit at once (one member = DTO sends a
    // plain descriptor: the per-op path).
    auto* fm = folly::fibers::FiberManager::getFiberManagerUnsafe();
    if (fm && fm->hasReadyTasks()) {
      folly::fibers::yield();
    }
  }
  if (!b->submitted) {
    // Nobody submitted while we yielded (or we filled it): we are the
    // leader. Close the batch to new members, submit, wait, wake the rest.
    b->submitted = true;
    if (tb.open == b) {
      tb.open = nullptr;
    }
    gBatchLeaders.fetch_add(1, std::memory_order_relaxed);
    b->op.submit();
    b->op.wait();
    for (size_t i = 0; i < b->op.size(); ++i) {
      if (i != idx) {
        b->batons[i].post();
      }
    }
  } else {
    gBatchWaiters.fetch_add(1, std::memory_order_relaxed);
    b->batons[idx].wait();
  }
  const uint32_t crc = b->op.crc(idx);
  if (--b->outstanding == 0) {
    tb.release(b);
  }
  return crc;
}
} // namespace

uint32_t checksumBatched(BufferView src, folly::FunctionRef<void()> overlap) {
  return batched(nullptr, src, false, false, overlap);
}

uint32_t copyAndChecksumBatched(uint8_t* dest,
                                BufferView src,
                                folly::FunctionRef<void()> overlap,
                                bool cacheControl) {
  return batched(dest, src, true, cacheControl, overlap);
}

ChecksumBatchStats getChecksumBatchStats() {
  ChecksumBatchStats st;
  st.batches = gBatchBatches.load();
  st.members = gBatchMembers.load();
  st.singles = gBatchSingles.load();
  st.leaders = gBatchLeaders.load();
  st.waiters = gBatchWaiters.load();
  st.deferred = gBatchDeferred.load();
  st.fallbacks = gBatchFallbacks.load();
  return st;
}

void addDeferredChecksumStats(uint64_t members) {
  gBatchDeferred.fetch_add(members, std::memory_order_relaxed);
}

CopyLargeWaitStats getChecksumWaitStats() {
  CopyLargeWaitStats st;
  st.yields = gCsumYields.load();
  st.polls = gCsumPolls.load();
  st.sleeps = gCsumBlocks.load();
  st.calls = gCsumOps.load();
  st.waitUs = gCsumWaitUs.load();
  st.fallbacks = gCsumFallbacks.load();
  return st;
}

CopyLargeWaitStats getCopyOutWaitStats() {
  CopyLargeWaitStats st;
  st.yields = gCopyYields.load();
  st.polls = gCopyPolls.load();
  st.sleeps = gCopyBlocks.load();
  st.calls = gCopyOps.load();
  st.waitUs = gCopyWaitUs.load();
  st.fallbacks = gCopyFallbacks.load();
  return st;
}

CopyLargeWaitStats getCopyLargeWaitStats() {
  CopyLargeWaitStats st;
  st.yields = gCopyLargeYields.load();
  st.polls = gCopyLargePolls.load();
  st.sleeps = gCopyLargeSleeps.load() + gLargeBlocks.load();
  st.calls = gCopyLargeCalls.load();
  st.submitUs = gCopyLargeSubmitUs.load();
  st.waitUs = gCopyLargeWaitUs.load();
  st.fallbacks = gLargeFallbacks.load();
  return st;
}

bool copyLargeWithOffload(uint8_t* dest,
                          const uint8_t* src,
                          size_t n,
                          size_t parts,
                          bool cacheControl,
                          LargeCopyWait wait,
                          uint32_t sleepPollUs) {
#ifdef CACHELIB_BUILD_WITH_DTO
  if (n == 0) {
    return true;
  }
  gCopyLargeCalls.fetch_add(1, std::memory_order_relaxed);
  const auto tSubmit = std::chrono::steady_clock::now();
  // Pieces of one DSA batch descriptor. DTO accepts up to DTO_BATCH_MAX
  // (1024) members, but the piece tables below live on the caller's stack,
  // which on a Navy fiber is 64 KiB, so cap at 64: a 16 MiB region still
  // goes out as 256 KiB pieces, more than the engines of one device can run
  // in parallel anyway.
  constexpr size_t kMaxParts = 64;
  parts = std::max<size_t>(1, std::min<size_t>(parts, kMaxParts));
  // Piece boundaries 4 KiB aligned so the device works on whole pages.
  size_t piece = (n + parts - 1) / parts;
  piece = (piece + 4095) & ~size_t{4095};
  void* dst[kMaxParts];
  void* srcs[kMaxParts];
  size_t sizes[kMaxParts];
  int count = 0;
  for (size_t off = 0; off < n && count < static_cast<int>(kMaxParts);
       off += piece, ++count) {
    dst[count] = dest + off;
    srcs[count] = const_cast<uint8_t*>(src) + off;
    sizes[count] = std::min(piece, n - off);
  }
  const unsigned int flags = dtoFlags(cacheControl);
  int rc;
  dto_batch* op = nullptr;
  alignas(64) dto_async_op single;
  if (count == 1) {
    rc = dto_submit_memcpy(&single, dest, src, n, flags);
  } else {
    op = dto_batch_create(count);
    rc = op ? dto_submit_batch_copy(op, dst, srcs, sizes, count, flags)
            : DTO_ASYNC_FALLBACK;
    if (rc != DTO_ASYNC_SUBMITTED) {
      // The device may lack the Batch opcode (DSA 1.0), or the WQ refused the
      // descriptor. One Memory Move covers the range on any DSA; try that
      // before giving the copy to memcpy.
      if (op) {
        dto_batch_destroy(op);
        op = nullptr;
      }
      rc = dto_submit_memcpy(&single, dest, src, n, flags);
    }
  }
  const auto tWait = std::chrono::steady_clock::now();
  gCopyLargeSubmitUs.fetch_add(
      std::chrono::duration_cast<std::chrono::microseconds>(tWait - tSubmit)
          .count(),
      std::memory_order_relaxed);
  if (rc != DTO_ASYNC_SUBMITTED) {
    if (op) {
      dto_batch_destroy(op);
    }
    std::memcpy(dest, src, n);
    return false;
  }
  auto* fm = folly::fibers::onFiber()
                 ? folly::fibers::FiberManager::getFiberManagerUnsafe()
                 : nullptr;
  int st;
  uint64_t yields = 0, polls = 0, sleeps = 0, blocks = 0;
  while ((st = op ? dto_batch_poll(op) : dto_async_poll(&single)) ==
         DTO_ASYNC_PENDING) {
    if (fm && fm->hasReadyTasks()) {
      // Regardless of wait policy: another fiber on this thread can run, and
      // parking the thread (dto_*_wait or sleep_for below block the whole
      // NavyThread, not just this fiber) would starve it. Only park when
      // idle.
      ++yields;
      folly::fibers::yield();
    } else if (wait == LargeCopyWait::kSleep && dtoWaitBlocks()) {
      // DTO parks this thread and the poller wakes it when the device is
      // done: no sleep granularity to overshoot, which for a 16 MiB flush
      // was most of the measured wait.
      ++blocks;
      if (op) {
        dto_batch_wait(op);
      } else {
        dto_async_wait(&single);
      }
    } else if (wait == LargeCopyWait::kSleep) {
      // No blocking wait configured and nothing runnable. nanosleep, even on
      // a fiber: the FiberManager's timed baton rides the EventBase wheel
      // timer whose tick is 10 ms, which made every wait 10 ms. Blocking the
      // thread for sleepPollUs delays fibers that become runnable meanwhile by
      // at most that much per iteration, and the CPU idles.
      ++sleeps;
      std::this_thread::sleep_for(std::chrono::microseconds(sleepPollUs));
    } else {
      ++polls;
      folly::asm_volatile_pause();
    }
  }
  gCopyLargeYields.fetch_add(yields, std::memory_order_relaxed);
  gCopyLargePolls.fetch_add(polls, std::memory_order_relaxed);
  gCopyLargeSleeps.fetch_add(sleeps, std::memory_order_relaxed);
  gLargeBlocks.fetch_add(blocks, std::memory_order_relaxed);
  gCopyLargeWaitUs.fetch_add(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - tWait)
          .count(),
      std::memory_order_relaxed);
  if (op) {
    dto_batch_destroy(op);
    // dto_batch_poll repairs failed members on the CPU before returning DONE.
    return true;
  }
  if (st == DTO_ASYNC_DONE) {
    return true;
  }
  gLargeFallbacks.fetch_add(1, std::memory_order_relaxed);
  std::memcpy(dest, src, n);
  return false;
#else
  (void)parts;
  (void)cacheControl;
  (void)wait;
  (void)sleepPollUs;
  std::memcpy(dest, src, n);
  return false;
#endif
}

bool copyOffloadSelfCheck(size_t size) {
  if (!checksumOffloadSupported()) {
    return false;
  }
  const size_t kSize = std::max<size_t>(size, 4096);
  std::vector<uint8_t> src(kSize);
  std::vector<uint8_t> dst(kSize, 0);
  std::mt19937 gen{54321};
  for (auto& b : src) {
    b = static_cast<uint8_t>(gen());
  }
  const bool offloaded = copyWithOffload(dst.data(), src.data(), kSize);
  if (!offloaded || std::memcmp(dst.data(), src.data(), kSize) != 0) {
    XLOGF(WARN,
          "copyOffloadSelfCheck: a {}-byte DSA Memory Move did not complete on "
          "the device or did not verify (a work queue whose max_transfer_size "
          "is below this size rejects it with XFER_ERANGE)",
          kSize);
    return false;
  }
  // The flush call site uses a single descriptor (parts = 1); require exactly
  // that. A multi-part copy needs the DSA Batch opcode, which DSA 1.0 lacks -
  // probe it for the log, but its absence must not disable the feature.
  std::fill(dst.begin(), dst.end(), 0);
  const bool single = copyLargeWithOffload(dst.data(), src.data(), kSize, 1);
  if (!single || std::memcmp(dst.data(), src.data(), kSize) != 0) {
    XLOG(WARN) << "copyOffloadSelfCheck: copyLargeWithOffload(parts=1) did not "
                  "complete on the device or did not verify";
    return false;
  }
  std::fill(dst.begin(), dst.end(), 0);
  const bool batched = copyLargeWithOffload(dst.data(), src.data(), kSize, 4);
  const bool batchOk =
      batched && std::memcmp(dst.data(), src.data(), kSize) == 0;
  // A refused Batch degrades to one descriptor inside copyLargeWithOffload,
  // so "OK" here means the multi-part path works, not that the device has
  // the Batch opcode.
  XLOGF(INFO,
        "copyOffloadSelfCheck: {}-byte single-descriptor copy OK on DSA; "
        "multi-part path {}",
        kSize, batchOk ? "OK (Batch opcode or single-descriptor degrade)"
                       : "unavailable (single-descriptor copies will be used)");
  return true;
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
  // gate) and verify parity with navy::checksum() plus copy fidelity. If DSA
  // is unavailable, the CPU fallback must also match.
  constexpr size_t kSize = 1024 * 1024;
  std::vector<uint8_t> src(kSize);
  std::vector<uint8_t> dst(kSize, 0);
  std::mt19937 gen{12345};
  for (auto& b : src) {
    b = static_cast<uint8_t>(gen());
  }

  const BufferView view{src.size(), src.data()};
  const uint32_t sw = checksum(view);

  // A device failure is redone on the CPU and still returns the right
  // checksum, so parity alone cannot tell a working accelerator from one that
  // rejects every descriptor (e.g. a WQ whose max_transfer_size is below the
  // request). Require that both operations actually completed on the device.
  AsyncChecksumOp crcOp;
  crcOp.submitChecksum(view);
  const uint32_t viaCrc = crcOp.wait();
  if (!crcOp.completedOnDevice()) {
    XLOG(WARN) << "checksumOffloadSelfCheck: CRC descriptor did not complete "
                  "on the device (submission refused or device failure); "
                  "offload would run on the CPU";
    return false;
  }
  AsyncChecksumOp copyOp;
  copyOp.submitCopyAndChecksum(dst.data(), view, true /* cacheControl */);
  const uint32_t viaCopy = copyOp.wait();
  if (!copyOp.completedOnDevice()) {
    XLOG(WARN) << "checksumOffloadSelfCheck: fused copy+CRC descriptor did not "
                  "complete on the device; offload would run on the CPU";
    return false;
  }
  if (viaCrc != sw || viaCopy != sw) {
    XLOGF(WARN,
          "checksumOffloadSelfCheck: accelerator checksum disagrees with CPU "
          "(cpu {:#x} crc {:#x} copy+crc {:#x})",
          sw, viaCrc, viaCopy);
    return false;
  }
  if (std::memcmp(dst.data(), src.data(), kSize) != 0) {
    XLOG(WARN) << "checksumOffloadSelfCheck: fused copy is not faithful";
    return false;
  }
  return true;
}

} // namespace navy
} // namespace cachelib
} // namespace facebook
