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

#include <folly/Random.h>
#include <folly/container/F14Map.h>
#include <folly/fibers/TimedMutex.h>

#include <memory>
#include <mutex>
#include <utility>

#include "cachelib/common/AtomicCounter.h"
#include "cachelib/common/ConditionVariable.h"
#include "cachelib/navy/block_cache/EvictionPolicy.h"
#include "cachelib/navy/block_cache/Region.h"
#include "cachelib/navy/block_cache/Types.h"
#include "cachelib/navy/common/Buffer.h"
#include "cachelib/navy/common/Device.h"
#include "cachelib/navy/common/NavyThread.h"
#include "cachelib/navy/common/Types.h"

namespace facebook {
namespace cachelib {
namespace interface {
class FlashCacheComponent;
}

namespace navy {
using folly::fibers::TimedMutex;
using CondWaiter = util::ConditionVariable::Waiter;

// Callback that is used to clear index.
//   @rid       Region ID
//   @buffer    Buffer with region data, valid during callback invocation
// Returns number of slots evicted
using RegionEvictCallback =
    std::function<uint32_t(RegionId rid, BufferView buffer)>;

// Callback that is used to clean up region.
//   @rid       Region ID
//   @buffer    Buffer with region data, valid during callback invocation
using RegionCleanupCallback =
    std::function<void(RegionId rid, BufferView buffer)>;

// Size class or stack allocator. Thread safe. Syncs access, reclaims regions
// Controls the allocation of regions, status (open for read/write), and
// eviction.
class RegionManager {
 public:
  // Constructs a Region Manager.
  //
  // @param numRegions                number of regions
  // @param regionSize                size of the region
  // @param baseOffset                base offset of the region
  // @param device                    reference to device
  // @param numCleanRegions           How many regions should be maintained in
  //                                  the clean pool
  // @param numWorkers                Number of threads to run reclaim jobs
  // @param stackSize                 Fiber stack size for each worker thread.
  //                                  0 for default
  // @param evictCb                   Callback invoked when region evicted
  // @param cleanupCb                 Callback invoked when region cleaned up
  // @param policy                    eviction policy
  // @param numInMemBuffers           number of in memory buffers
  // @param numPriorities             max number of priorities allowed for
  //                                  regions
  // @param inMemBufFlushRetryLimit   max number of flushing retry times for
  //                                  in-mem buffer
  // @param workerFlushAsync          whether to schedule async flushes with the
  //                                  workers (during reclaim)
  // @param allowReadDuringReclaim    whether to allow read for the being
  //                                  reclaimed region.
  //                                  TODO: This should be the default behavior
  //                                  and this flag should be removed once we're
  //                                  convinced there's no missing corner cases.
  // @param recoverEvictionPolicy     whether to persist and recover eviction
  //                                  policy ordering across restarts
  // @param directFlush               whether to write region buffer directly
  //                                  to device without intermediate copy
  // @param flushCopyOffload          when not directFlush: copy the region
  //                                  buffer into the write buffer on Intel DSA
  //                                  (DTO batch descriptor) instead of memcpy
  RegionManager(uint32_t numRegions,
                uint64_t regionSize,
                uint64_t baseOffset,
                Device& device,
                uint32_t numCleanRegions,
                uint32_t numWorkers,
                uint32_t stackSize,
                RegionEvictCallback evictCb,
                RegionCleanupCallback cleanupCb,
                std::unique_ptr<EvictionPolicy> policy,
                uint32_t numInMemBuffers,
                uint16_t numPriorities,
                uint16_t inMemBufFlushRetryLimit,
                bool workerFlushAsync,
                bool allowReadDuringReclaim = false,
                bool recoverEvictionPolicy = false,
                bool directFlush = false,
                bool flushCopyOffload = false);
  RegionManager(const RegionManager&) = delete;
  RegionManager& operator=(const RegionManager&) = delete;

  // Destroy the worker thread for safety first
  ~RegionManager() { workers_.clear(); }

  // return the size of usable space
  uint64_t getSize() const {
    return static_cast<uint64_t>(numRegions_) * regionSize_;
  }

  // Gets a region from a valid region ID.
  Region& getRegion(RegionId rid) {
    XDCHECK(rid.valid());
    return *regions_[rid.index()];
  }

  // Gets a const region from a valid region ID.
  const Region& getRegion(RegionId rid) const {
    XDCHECK(rid.valid());
    return *regions_[rid.index()];
  }

  const RegionId getRandomRegion() const {
    return RegionId{folly::Random::rand32(0, numRegions_)};
  }
  // Flushes the in memory buffer attached to a region in either async or
  // sync mode.
  // In async mode, a flush job will be added to a job scheduler;
  // In sync mode, the function will not end until the flush work succeeds.
  void doFlush(RegionId rid, bool async);

  // Returns the size of one region.
  uint64_t regionSize() const { return regionSize_; }

  // Estimated bytes persist() would write, including the eviction policy
  // payload when its recovery is enabled.
  uint64_t estimatePersistSize() const;

  // Gets a region to evict.
  RegionId evict();

  // Promote a region. If this region was still buffered in-mem,
  // this would be a no-op.
  void touch(RegionId rid);

  // Calling track on tracked regions is noop.
  void track(RegionId rid);

  // Resets all region internal state.
  void reset();

  // Atomically loads the current sequence number (in memory_order_acquire
  // order).
  // Sequence number increases when a reclaim finished. Since reclaim
  // may start during reading, by checking whether the sequence number changes,
  // we avoid reading a region that has been reclaimed.
  uint64_t getSeqNumber() const {
    return seqNumber_.load(std::memory_order_acquire);
  }

  // Converts @RelAddress to @AbsAddress.
  AbsAddress toAbsolute(RelAddress ra) const {
    return AbsAddress{ra.offset() + ra.rid().index() * regionSize_};
  }

  // Converts @AbsAddress to @RelAddress.
  RelAddress toRelative(AbsAddress aa) const {
    // Compiler optimizes to use one division instruction
    return RelAddress{RegionId(aa.offset() / regionSize_),
                      uint32_t(aa.offset() % regionSize_)};
  }

  // Assigns a buffer from buffer pool.
  std::pair<std::unique_ptr<Buffer>, std::unique_ptr<CondWaiter>>
  claimBufferFromPool(bool addWaiter);

  // Returns the buffer to the pool.
  void returnBufferToPool(std::unique_ptr<Buffer> buf) {
    {
      std::lock_guard bufLock{bufferMutex_};
      buffers_.push_back(std::move(buf));
      if (bufferCond_.numWaiters() > 0) {
        bufferCond_.notifyAll();
      }
    }
    numInMemBufActive_.dec();
  }

  bool deviceWrite(RelAddress addr, Buffer buf);

  // Returns a buffer with data read from the device the @addr of size bytes
  // @addr must be the address returned by Region::open(OpenMode::Read).
  //
  // On success the returned buffer will have same size as "size" argument.
  // Caller must check the size of the buffer returned to determine if this
  // succeeded or not.
  Buffer read(const RegionDescriptor& desc, RelAddress addr, size_t size) const;

  // Flushes all in memory buffers to the device and then issues device flush.
  void flush();

  // Flushes the in memory buffer attached to a region.
  // Returns true if the flush succeeds and the buffer is detached from the
  // region; false otherwise.
  //
  // Caller is expected to call flushBuffer until true is returned or retry
  // times reach the limit. This routine is idempotent and is safe to call
  // multiple times until detachBuffer is done.
  Region::FlushRes flushBuffer(const RegionId& rid);

  // Records a value whose checksum is computed at flush time (BlockCache).
  void addPendingChecksum(const RegionId& rid, Region::PendingChecksum p) {
    getRegion(rid).addPendingChecksum(p);
  }

  // Detaches the buffer from the region and returns the buffer to pool.
  // This could block if there are active readers
  void detachBuffer(const RegionId& rid);

  // Cleans up the in memory buffer when flushing failure reach the retry limit.
  // This could block if there are active readers or writers
  void cleanupBufferOnFlushFailure(const RegionId& rid);

  // Releases a region that was cleaned up due to in-mem buffer flushing
  // failure.
  void releaseCleanedupRegion(RegionId rid);

  // Stores region information in a Thrift object for all regions.
  void persist(RecordWriter& rw) const;

  // Resets RegionManager and recovers region data. Throws std::exception on
  // failure.
  void recover(RecordReader& rr);

  // Exports RegionManager stats via CounterVisitor.
  void getCounters(const CounterVisitor& visitor) const;

  // Opens a region for reading and returns the region descriptor.
  //
  // @param rid         region ID
  // @param seqNumber   the sequence number aqcuired before opening the region
  //                    for read; it is used to determine whether a reclaim
  //                    happened during reading.
  //                    if it's not providied, this function will just open the
  //                    region and doesn't check about the race between reclaim
  //                    and this open. (Assuming it's caller's responsibility)
  RegionDescriptor openForRead(RegionId rid, std::optional<uint64_t> seqNumber);

  // Closes the region and consumes the region descriptor.
  void close(RegionDescriptor&& desc);

  // Fetches a clean region from the @cleanRegions_ list and schedules reclaim
  // jobs to refill the list. If in-mem buffer mode is enabled, a buffer will be
  // attached to the fetched clean region.
  // Returns OpenStatus::Ready if all the operations are successful;
  // OpenStatus::Retry otherwise.
  std::pair<OpenStatus, std::unique_ptr<CondWaiter>> getCleanRegion(
      RegionId& rid, bool addWaiter);

  // Finish all pending jobs
  void drain();

  // Schedules region reclaim job to create a clean region
  void startReclaim();

  // Releases a region that was evicted during region reclaim.
  //
  // @param rid        region ID
  // @param startTime  time when the reclaim starts;
  //                   it is used to count the reclaim time duration
  void releaseEvictedRegion(RegionId rid, std::chrono::nanoseconds startTime);

  // Evicts a region by calling @evictCb_ during region reclaim.
  void doEviction(RegionId rid, BufferView buffer) const;

  // Convert relative address to phsyical offset on the device
  uint64_t physicalOffset(RelAddress addr) const {
    return baseOffset_ + toAbsolute(addr).offset();
  }

  bool readAllowedDuringReclaim() const { return allowReadDuringReclaim_; }

 private:
  using LockGuard = std::lock_guard<TimedMutex>;

  NavyThread& getNextWorker() {
    return *(workers_[numReclaimScheduled_.add_fetch(1) % workers_.size()]);
  }

  bool isOnWorker() {
    auto* thread = getCurrentNavyThread();
    if (!thread) {
      return false;
    }

    return workerSet_.count(thread) > 0;
  }

  void doReclaim();
  void doFlushInternal(RegionId rid);

  bool deviceWrite(RelAddress addr, BufferView buf);

  bool isValidIORange(uint32_t offset, uint32_t size) const;
  std::pair<OpenStatus, std::unique_ptr<CondWaiter>> assignBufferToRegion(
      RegionId rid, bool addWaiter);

  // Initializes the eviction policy. Even on a clean start, we will track all
  // the regions. The difference is that these regions will have no items in
  // them and can be evicted right away.
  void resetEvictionPolicy();

  // Recomputes external fragmentation from region state. Used by the eviction
  // policy recovery path (resetEvictionPolicy() handles this internally).
  void recomputeFragmentation();

  // After policy_->recover(), reconstruct any region not in the recovered
  // policy queue. Empty untracked regions go to cleanRegions_ (up to pool
  // capacity); the rest are tracked into the policy. Defends against
  // permanent leaks from in-memory state (e.g., cleanRegions_) that isn't
  // covered by the persisted policy data. Also validates region IDs from
  // the persisted policy data.
  void restoreUntrackedRegions(const serialization::EvictionPolicyData& data);

  const uint16_t numPriorities_{};
  const uint16_t inMemBufFlushRetryLimit_{};
  const uint32_t numRegions_{};
  const uint64_t regionSize_{};
  const uint64_t baseOffset_{};
  Device& device_;
  const std::unique_ptr<EvictionPolicy> policy_;
  std::unique_ptr<std::unique_ptr<Region>[]> regions_;
  mutable AtomicCounter externalFragmentation_;

  mutable AtomicCounter physicalWrittenCount_;
  mutable AtomicCounter reclaimRegionErrors_;

  mutable trace::Profiled<util::SharedMutex, "cachelib:navy:bc_clean_regions">
      cleanRegionsMutex_;
  mutable util::ConditionVariable cleanRegionsCond_;
  std::vector<RegionId> cleanRegions_;
  const uint32_t numCleanRegions_{};
  mutable AtomicCounter cleanRegionRetries_;

  std::atomic<uint64_t> seqNumber_{0};

  uint32_t reclaimsOutstanding_{0};

  // Fast-path flag: when true, cleanRegions_ is empty and reclaims are
  // in-flight. getCleanRegion() can skip acquiring cleanRegionsMutex_ and
  // return Retry immediately, avoiding mutex contention that would starve
  // reclaim workers trying to push clean regions back.
  //
  // Uses memory_order_relaxed: all stores are done under
  // cleanRegionsMutex_ so writers are properly serialized, and the
  // unsynchronized load on the fast-path may see a stale value but
  // callers retry in a loop so correctness is not affected.
  std::atomic<bool> cleanRegionsEmpty_{false};

  // The thread that runs the flush and reclaim. For Navy-async thread mode, the
  // async flushes will be run in-line on fiber by the async NavyThread itself
  std::vector<std::unique_ptr<NavyThread>> workers_;
  std::unordered_set<NavyThread*> workerSet_;
  // Whether to schedule async flushes on the workers itself (as opposed to run
  // flushes inline).
  // TODO (@haowux): Evaluate and always flush async.
  const bool workerFlushAsync_{false};
  mutable AtomicCounter numReclaimScheduled_;

  // Whether to allow read for the being reclaimed region
  const bool allowReadDuringReclaim_{false};

  // Whether to persist and recover eviction policy ordering across restarts
  const bool recoverEvictionPolicy_{false};

  // Whether to write region buffer directly without intermediate copy
  const bool directFlush_{false};

  // Flush copy on DSA (see ctor); cleared at construction if directFlush is
  // set or the DTO/DSA self-check fails.
  bool flushCopyOffload_{false};
  mutable AtomicCounter flushCopyOffloadCount_;
  mutable AtomicCounter flushCopyFallbackCount_;
  // Value checksums BlockCache deferred to flush time and the DSA batches
  // that computed them (see drainPendingChecksums).
  mutable AtomicCounter deferredChecksumCount_;
  mutable AtomicCounter deferredChecksumBatches_;

  // Computes every checksum a region deferred and patches it into the
  // buffer. Runs on the flush path once the region has no active writers,
  // before the buffer is copied or written.
  void drainPendingChecksums(Region& region);

  // Pool of device-aligned write buffers for the non-direct flush path.
  // Without it every flush allocates a fresh regionSize_ buffer (16 MiB by
  // default, ~10k times per 150 GB written): the allocation itself, and for
  // the DSA copy fresh pages the device page-faults on. Buffers are handed
  // out for the duration of one flush; the pool keeps at most
  // max(2 x workers, numInMemBuffers) of them - the most flushes that can be
  // in progress at once - so a burst neither allocates under the DMA engine
  // nor grows without bound.
  Buffer acquireWriteBuffer();
  void releaseWriteBuffer(Buffer buf);
  // mutable: getCounters() is const and reports the pool size
  mutable std::mutex writeBufPoolMutex_;
  std::vector<Buffer> writeBufPool_;
  size_t writeBufPoolCap_{0};

  const RegionEvictCallback evictCb_;
  const RegionCleanupCallback cleanupCb_;

  // To understand naming here, let me explain difference between "reclaim"
  // and "eviction". Cache evicts item and makes it inaccessible via lookup. It
  // is an item level operation. When we say "reclaim" about regions we
  // refer to wiping an entire region for reuse. As part of reclaim, every
  // item in the region gets evicted.
  mutable AtomicCounter reclaimCount_;
  mutable AtomicCounter reclaimTimeCountUs_;
  mutable AtomicCounter evictedCount_;
  mutable AtomicCounter readDuringReclaimCount_;

  // Stats to keep track of inmem buffer usage
  mutable AtomicCounter numInMemBufActive_;
  mutable AtomicCounter numInMemBufWaitingFlush_;
  mutable AtomicCounter numInMemBufFlushRetries_;
  mutable AtomicCounter numInMemBufFlushFailures_;

  const uint32_t numInMemBuffers_{0};
  // Locking order is region lock, followed by bufferMutex_;
  mutable trace::Profiled<TimedMutex, "cachelib:navy:bc_buffer"> bufferMutex_;
  mutable util::ConditionVariable bufferCond_;
  std::vector<std::unique_ptr<Buffer>> buffers_;
  int placementHandle_;

  friend class interface::FlashCacheComponent;
};
} // namespace navy
} // namespace cachelib
} // namespace facebook
