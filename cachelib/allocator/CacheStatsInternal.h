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
#include <array>
#include <numeric>

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/memory/MemoryAllocator.h"
#include "cachelib/common/AtomicCounter.h"

namespace facebook {
namespace cachelib {

// forward declaration
struct GlobalCacheStats;

namespace detail {

// collection of stats that are updated at a high frequency, making it
// necessary to track them as thread local counters that are aggregated.
struct Stats {
  // overall hit ratio related stats for the cache.
  // number of calls to CacheAllocator::find
  TLCounter numCacheGets{0};

  // number of such calls being a miss in the cache.
  TLCounter numCacheGetMiss{0};

  // number of such calls being an expiry in the cache. This is also included
  // in the numCacheGetMiss stats above.
  TLCounter numCacheGetExpiries{0};

  // number of remove calls to CacheAllocator::remove that requires
  // a lookup first and then remove the item
  TLCounter numCacheRemoves{0};

  // number of remove calls that resulted in a ram hit
  TLCounter numCacheRemoveRamHits{0};

  // number of evictions where items leave both RAM and NvmCache entirely
  AtomicCounter numCacheEvictions{0};

  // number of item destructor calls from ram
  TLCounter numRamDestructorCalls{0};

  // number of nvm gets
  TLCounter numNvmGets{0};

  // number of nvm get miss that happened synchronously
  TLCounter numNvmGetMissFast{0};

  // number of nvm misses
  TLCounter numNvmGetMiss{0};

  // number of nvm isses due to internal errors
  TLCounter numNvmGetMissErrs{0};

  // number of nvm misses due to inflight remove on the same key
  TLCounter numNvmGetMissDueToInflightRemove{0};

  // number of nvm gets that are expired
  TLCounter numNvmGetMissExpired{0};

  // number of gets that joined a concurrent fill for same item
  AtomicCounter numNvmGetCoalesced{0};

  // number of deletes issues to nvm
  TLCounter numNvmDeletes{0};

  // number of deletes skipped and not issued to nvm
  TLCounter numNvmSkippedDeletes{0};

  // number of writes to nvm
  TLCounter numNvmPuts{0};

  // number of put errors;
  TLCounter numNvmPutErrs{0};

  // number of put failures due to encode call back
  AtomicCounter numNvmPutEncodeFailure{0};

  // number of puts that observed an inflight delete and aborted
  AtomicCounter numNvmAbortedPutOnTombstone{0};

  // number of puts that observed an inflight concurrent get and aborted
  AtomicCounter numNvmAbortedPutOnInflightGet{0};

  // number of items that are filtered by compaction
  AtomicCounter numNvmCompactionFiltered{0};

  // number of evictions from NvmCache
  TLCounter numNvmEvictions{0};

  // number of evictions from nvm that found an inconsistent state in RAM
  AtomicCounter numNvmUncleanEvict{0};

  // number of evictions that were issued for an item that was in RAM in clean
  // state
  AtomicCounter numNvmCleanEvict{0};

  // number of evictions that were issued more than once on an unclean item.
  AtomicCounter numNvmCleanDoubleEvict{0};

  // number of evictions that were already expired
  AtomicCounter numNvmExpiredEvict{0};

  // number of item destructor calls from nvm
  AtomicCounter numNvmDestructorCalls{0};

  // number of RefcountOverflow happens causing item destructor
  // being skipped in nvm
  AtomicCounter numNvmDestructorRefcountOverflow{0};

  // number of entries that were clean in RAM, but evicted and rewritten to
  // nvmcache because the nvmcache version was evicted
  AtomicCounter numNvmPutFromClean{0};

  // Decryption and Encryption errors
  AtomicCounter numNvmEncryptionErrors{0};
  AtomicCounter numNvmDecryptionErrors{0};

  // basic admission policy stats
  TLCounter numNvmRejectsByExpiry{0};
  TLCounter numNvmRejectsByClean{0};
  TLCounter numNvmRejectsByAP{0};

  // attempts made from nvm cache to allocate an item for promotion
  TLCounter numNvmAllocAttempts{0};

  // flash-hit copy-outs (Navy buffer -> DRAM item) performed by Intel DSA,
  // the bytes they moved, and offload attempts that fell back to the CPU
  TLCounter numNvmCopyOutOffloaded{0};
  TLCounter numNvmCopyOutOffloadedBytes{0};
  TLCounter numNvmCopyOutFallbacks{0};

  // attempts made from nvm cache to allocate an item for its destructor
  TLCounter numNvmAllocForItemDestructor{0};
  // heap allocate errors for item destructor
  TLCounter numNvmItemDestructorAllocErrors{0};

  // the number of allocated items that are permanent
  TLCounter numPermanentItems{0};

  // Number of insertOrReplace calls that resulted in insert
  AtomicCounter numInsertOrReplaceInserted{0};

  // Number of insertOrReplace calls that resulted in replace
  AtomicCounter numInsertOrReplaceReplaced{0};

  // the number of allocated and CHAINED items that are parents (i.e.,
  // consisting of at least one chained child)
  TLCounter numChainedParentItems{0};

  // the number of allocated and CHAINED items that are children (i.e.,
  // allocated with a parent handle that it's chained to)
  TLCounter numChainedChildItems{0};

  // the numbers for move and evictions in the process of slab release.
  AtomicCounter numMoveAttempts{0};
  AtomicCounter numMoveSuccesses{0};
  AtomicCounter numEvictionAttempts{0};
  AtomicCounter numEvictionSuccesses{0};

  // the number times a refcount overflow occurred, resulting in an exception
  // being thrown
  AtomicCounter numRefcountOverflow{0};

  // number of exception occurred inside item destructor
  AtomicCounter numDestructorExceptions{0};

  // The number of slabs being released right now.
  // This must be zero when `saveState()` is called.
  AtomicCounter numActiveSlabReleases{0};
  // Number of different slab releases.
  AtomicCounter numReleasedForRebalance{0};
  AtomicCounter numReleasedForResize{0};
  AtomicCounter numReleasedForAdvise{0};
  AtomicCounter numAbortedSlabReleases{0};
  AtomicCounter numReaperSkippedSlabs{0};

  // Flag indicating the slab release stuck
  AtomicCounter numSlabReleaseStuck{0};

  // allocations with invalid parameters
  AtomicCounter invalidAllocs{0};

  // latency stats of various cachelib operations
  mutable util::PercentileStats allocateLatency_;
  mutable util::PercentileStats moveChainedLatency_;
  mutable util::PercentileStats moveRegularLatency_;
  mutable util::PercentileStats nvmLookupLatency_;
  mutable util::PercentileStats nvmInsertLatency_;
  mutable util::PercentileStats nvmRemoveLatency_;
  mutable util::PercentileStats nvmMakeBlobCbLatency_;
  mutable util::PercentileStats nvmMakeObjCbLatency_;

  // percentile stats for various cache statistics
  mutable util::PercentileStats ramEvictionAgeSecs_;
  mutable util::PercentileStats ramItemLifeTimeSecs_;
  mutable util::PercentileStats nvmSmallLifetimeSecs_;
  mutable util::PercentileStats nvmLargeLifetimeSecs_;
  mutable util::PercentileStats nvmEvictionSecondsPastExpiry_;
  mutable util::PercentileStats nvmEvictionSecondsToExpiry_;
  mutable util::PercentileStats nvmHitTTASecs_;

  // per-pool percentile stats for eviction age
  std::array<util::PercentileStats, MemoryPoolManager::kMaxPools>
      perPoolEvictionAgeSecs_;

  // This tracks in each window what are the percentiles of the sizes of
  // items that we have written to flash. This is at-the-moment view of what
  // we're currently writing into flash.
  mutable util::PercentileStats nvmPutSize_;

  using PerPoolClassAtomicCounters =
      std::array<std::array<AtomicCounter, MemoryAllocator::kMaxClasses>,
                 MemoryPoolManager::kMaxPools>;

  // count of a stat for a specific allocation class
  using PerPoolClassTLCounters =
      std::array<std::array<TLCounter, MemoryAllocator::kMaxClasses>,
                 MemoryPoolManager::kMaxPools>;

  // hit count for every alloc class in every pool
  std::unique_ptr<PerPoolClassTLCounters> cacheHits{};
  std::unique_ptr<PerPoolClassAtomicCounters> allocAttempts{};
  std::unique_ptr<PerPoolClassAtomicCounters> evictionAttempts{};
  std::unique_ptr<PerPoolClassAtomicCounters> allocFailures{};
  std::unique_ptr<PerPoolClassAtomicCounters> fragmentationSize{};
  std::unique_ptr<PerPoolClassAtomicCounters> chainedItemEvictions{};
  std::unique_ptr<PerPoolClassAtomicCounters> regularItemEvictions{};

  // Evictions skipped due to filtering
  AtomicCounter evictFiltered{0};

  // Eviction failures due to parent cannot be removed from access container
  AtomicCounter evictFailParentAC{0};

  // Breakdown of evictFailParentAC by the reason the parent could not be
  // marked for eviction.
  // Parent was no longer in the MMContainer (kLinked cleared).
  AtomicCounter evictFailParentUnlinkedAC{0};
  // Parent was already being evicted or moved (kExclusive set).
  AtomicCounter evictFailParentExclusiveAC{0};
  // Parent had an outstanding access refcount (a handle was held).
  AtomicCounter evictFailParentRefHeldAC{0};

  // Eviction failures due to parent cannot be removed because it's being
  // moved
  AtomicCounter evictFailParentMove{0};

  // Eviction failures because this item cannot be removed from access
  // container
  AtomicCounter evictFailAC{0};

  // Eviction failures because this item has a potential concurrent fill
  // from nvm cache. For consistency reason, we cannot evict it. Refer
  // to NvmCache.h for more details.
  AtomicCounter evictFailConcurrentFill{0};

  // Eviction failures because we failed to acquire the lock and cannot
  // prove the absence of concurrent fills, so we can't evict it. Refer
  // to NvmCache.h for more details.
  AtomicCounter evictFailPutTokenLock{0};

  // Eviction failures because we failed to mark the item as being evicted
  // due to a concurrent ongoing read. Refer to NvmCache.h for more details.
  AtomicCounter evictFailConcurrentAccess{0};

  // Eviction failures because this item is being moved
  AtomicCounter evictFailMove{0};

  // Number of times wait() blocks for an item handle
  TLCounter numHandleWaitBlocks{0};

  // Number of times "expensive" cachelib stats are polled. This is useful as
  // polling these stats can be expensive. We shouldn't do it too often.
  TLCounter numExpensiveStatsPolled{0};

  void init();

  void populateGlobalCacheStats(GlobalCacheStats& ret) const;
};

} // namespace detail
} // namespace cachelib
} // namespace facebook
