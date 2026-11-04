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

#include <folly/CPortability.h>
#include <folly/lang/Aligned.h>
#include <folly/synchronization/DistributedMutex.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#if FOLLY_SANITIZE_ADDRESS
#include "cachelib/allocator/datastruct/AsanSList.h"
#else
#include "cachelib/allocator/datastruct/SList.h"
#endif
#include "cachelib/allocator/memory/CompressedPtr.h"
#include "cachelib/allocator/memory/MemoryAllocatorStats.h"
#include "cachelib/allocator/memory/Slab.h"
#include "cachelib/allocator/memory/SlabAllocator.h"
#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"

namespace facebook {
namespace cachelib {

// forward declaration
namespace tests {
class AllocTestBase;
}

enum class SlabIterationStatus {
  kFinishedCurrentSlabAndContinue,
  kSkippedCurrentSlabAndContinue,
  kAbortIteration
};

// An AllocationClass is used to allocate memory for a given allocation size
// from Slabs
class AllocationClass {
 public:
  // @param classId   the id corresponding to this allocation class
  // @param poolId    the poolId corresponding to this allocation class
  // @param allocSize the size of allocations that this allocation class
  //                  handles.
  // @param s         the slab allocator for fetching the header info.
  //
  // @throw std::invalid_argument if the classId is invalid or the allocSize
  //        is invalid.
  AllocationClass(ClassId classId,
                  PoolId poolId,
                  uint32_t allocSize,
                  const SlabAllocator& s);

  // restore this AllocationClass from the serialized data.
  // @param object  Object that contains the data to restore AllocationClass
  // @param poolId  the poolId corresponding to this allocation class
  // @param s       the slab allocator for fetching the header info. s must be
  //                a restorable slab allocator which was previously used with
  //                the same allocation class object.
  //
  // @throw std::invalid_argument if the classId is invalid or the allocSize
  //        is invalid.
  // @throw std::logic_error if the allocation class cannot be restored with
  //        this allocator
  AllocationClass(const serialization::AllocationClassObject& object,
                  PoolId poolId,
                  const SlabAllocator& s);

  AllocationClass(const AllocationClass&) = delete;
  AllocationClass& operator=(const AllocationClass&) = delete;

  // returns the id corresponding to the allocation class.
  ClassId getId() const noexcept { return classId_; }

  // returns the poolId corresponding to the allocation class.
  PoolId getPoolId() const noexcept { return poolId_; }

  // returns the allocation size handled by this  allocation class.
  uint32_t getAllocSize() const noexcept { return allocationSize_; }

  // returns the number of allocations that can be made out of a Slab.
  unsigned int getAllocsPerSlab() const noexcept {
    return static_cast<unsigned int>(Slab::kSize / allocationSize_);
  }

  // fetch stats about this allocation class.
  ACStats getStats() const;

  // returns the number of slabs this allocation class owns, in use or free.
  size_t getNumSlabs() const {
    return lock_->lock_combine(
        [this]() { return allocatedSlabs_.size() + freeSlabs_.size(); });
  }

  // Whether the pool is full or free to allocate more in the current state.
  // This is only a hint and not a gurantee that subsequent allocate will
  // fail/succeed.
  bool isFull() const noexcept { return !canAllocate_; }

  // allocate memory corresponding to the allocation size of this
  // AllocationClass.
  //
  // @return  ptr to the memory of allocationSize_ chunk or nullptr if we
  //          don't have any free memory. The caller will have to add a slab
  //          to this slab class to make further allocations out of it.
  void* allocate();

  // @param ctx     release context for the slab owning this alloc
  // @param memory  memory to check
  //
  // @return true  if the memory corresponds to an alloc that has been freed
  //
  // @throws std::invalid_argument  if the memory does not belong to a slab of
  //         this slab class, or if the slab is not actively being released, or
  //         if the context belongs to a different slab.
  // @throws std::runtime_error  if the slab cannot be found inside
  //         slabReleaseAllocMap_
  bool isAllocFreed(const SlabReleaseContext& ctx, void* memory) const;

  // The callback is executed under the lock, immediately after checking if the
  // alloc has been freed.
  //
  // @param ctx       release context for the slab owning this alloc
  // @param memory    memory to check
  // @param callback  callback to execute if the alloc has not been freed. This
  //                  takes a single argument - the alloc being processed.
  //
  // @throws std::invalid_argument  if the memory does not belong to a slab of
  //         this slab class, or if the slab is not actively being released, or
  //         if the context belongs to a different slab.
  // @throws std::runtime_error  if the slab cannot be found inside
  //         slabReleaseAllocMap_
  void processAllocForRelease(const SlabReleaseContext& ctx,
                              void* memory,
                              const std::function<void(void*)>& callback) const;

  // Function takes the startSlabReleaseLock_, gets the slab header and if
  // the slab is in a valid state invokes a user defined callback for each
  // allocation in the slab.
  //
  // @param slab      Slab to visit.
  // @param callback  Callback function to invoke on each allocation.
  //
  // @return          true to continue with the iteration, false to abort.
  //
  // AllocTraversalFn    Allocator traversal function
  // @param ptr          pointer to allocation
  // @param allocInfo    AllocInfo of the allocation
  // @return SlabIterationStatus
  template <typename AllocTraversalFn>
  SlabIterationStatus forEachAllocation(Slab* slab,
                                        AllocTraversalFn&& callback) {
    // Take a try_lock on this allocation class beginning any new slab release.
    std::unique_lock<std::mutex> startSlabReleaseLockHolder(
        startSlabReleaseLock_, std::defer_lock);

    // If the try_lock fails, skip this slab
    if (!startSlabReleaseLockHolder.try_lock()) {
      return SlabIterationStatus::kSkippedCurrentSlabAndContinue;
    }

    // check for the header to be valid.
    using Return = folly::Optional<AllocInfo>;
    auto allocInfo = lock_->lock_combine([this, slab]() -> Return {
      auto slabHdr = slabAlloc_.getSlabHeader(slab);

      if (!slabHdr || slabHdr->getClassId() != classId_ ||
          slabHdr->getPoolId() != poolId_ || slabHdr->isAdvised() ||
          slabHdr->isMarkedForRelease()) {
        return folly::none;
      }

      return Return{{slabHdr->getPoolId(), slabHdr->getClassId(),
                     slabHdr->getAllocSize()}};
    });
    if (!allocInfo) {
      return SlabIterationStatus::kSkippedCurrentSlabAndContinue;
    }

    // Prefetch the first kForEachAllocPrefetchOffset items in the slab.
    // Note that the prefetch is for read with no temporal locality.
    auto* prefetchPtr = reinterpret_cast<uint8_t*>(slab);
    for (unsigned int i = 0; i < kForEachAllocPrefetchOffset; i++) {
      prefetchPtr += allocationSize_;
      __builtin_prefetch(prefetchPtr, 0, 0);
    }
    uint8_t* ptr = reinterpret_cast<uint8_t*>(slab);
    unsigned int allocsPerSlab = getAllocsPerSlab();
    for (unsigned int i = 0; i < allocsPerSlab; ++i) {
      prefetchPtr += allocationSize_;
      // Prefetch ahead the kForEachAllocPrefetchOffset item.
      __builtin_prefetch(prefetchPtr, 0, 0);
      if (!callback(ptr, allocInfo.value())) {
        return SlabIterationStatus::kAbortIteration;
      }
      ptr += allocationSize_;
    }
    return SlabIterationStatus::kFinishedCurrentSlabAndContinue;
  }

  // release the memory back to the slab class.
  //
  // @param memory  memory to be released.
  // @throws std::invalid_argument if the memory does not belong to a slab of
  // this slab class.
  void free(void* memory);

  // acquires a new slab for this allocation class.
  // @param slab    a new slab to be added. This can NOT be nullptr.
  void addSlab(Slab* slab);

  // acquires a new slab and return an allocation right away.
  // @param slab    a new slab to be added. This can NOT be nullptr.
  // @return  new allocation. This cannot fail.
  void* addSlabAndAllocate(Slab* slab);

  // Releasing a slab is a two step process.
  // 1. Mark a slab for release, by calling `startSlabRelease`.
  // 2. Free all the activeAllocations
  // 3. Actually release the slab, by calling `completeSlabRelease`.
  //    In some scenario (i.e. when the slab is already released in step 1),
  //    there is no need to do step 2.
  //
  // In between the two steps, the user must ensure any active allocation
  // from the slab is freed by calling ac->free(alloc). completeSlabRelease
  // will block until all the active allocations for the slab are freed back.
  //
  // These allocations will not be moved to the free allocation list. Instead
  // the free simply becomes an no-op. This is fine since the slab will be
  // released eventually, and we do not want the freed allocations to be used
  // again in the meanwhile.
  //
  // @param  mode   slab release mode
  //
  // @param  hint   hint of an allocation belong to the slab that we want
  //                released. If this is nullptr, a random slab will be
  //                selected for releasing.
  //
  // @param  shouldAbortFn invoked in the code to see if this release slab
  //         process should be aborted
  //
  // @return  SlabReleaseContext
  //          isReleased == true means the slab is already released
  //          as there was no active allocation to be freed. If not, the
  //          caller is responsible for ensuring that all active alocations
  //          returned by getActiveAllocs are freed back and
  //
  // @throw std::invalid_argument if the hint is invalid.
  //
  // @throw exception::SlabReleaseAborted if slab release is aborted due to
  //        shouldAbortFn returning true.
  SlabReleaseContext startSlabRelease(
      SlabReleaseMode mode,
      const void* hint,
      SlabReleaseAbortFn shouldAbortFn = []() { return false; });

  // Aborting a previously started SlabRelease will not restore already
  // freed allocations. So the end state may not be exactly same as
  // pre-startSlabRelease.
  //
  // precondition: startSlabRelease must be called before this, and the
  //               context must be valid and the slab has not yet been
  //               released.
  //
  // @param context  the slab release context returned by startSlabRelease
  // @throw std::invalid_argument
  //        a invalid_argument is thrown when the context is invalid or
  //        the context is already released or all allocs are freed.
  void abortSlabRelease(const SlabReleaseContext& context);

  // precondition: startSlabRelease must be called before this, and the
  //               context must be valid and the slab has not yet been
  //               released. If context.isReleased() == true there is no
  //               need to call completeSlabRelease.
  //
  // @param context  the slab release context returned by startSlabRelease
  // @throw std::runtime_error
  //        a runtime_error is thrown when the context is invalid or
  //        the slab associated with the context is not in a valid state.
  void completeSlabRelease(const SlabReleaseContext& context);

  // check if the slab has all its allocations freed back to the
  // AllocationClass.  This must be called only for a slab that has an active
  // slab release.
  //
  // @param slab  the slab that we are interested in.
  // @return    True if all the allocations are freed back to the allocator.
  //            False if not.
  // @throw   std::runtime_error if the slab does not have the allocStateMap
  //          entry.
  bool allFreed(const Slab* slab) const;

  // for saving and restoring the state of the allocation class
  //
  // precondition:  The object must have been instantiated with a restorable
  // slab allocator does not own the memory. serialization must happen without
  // any reader or writer present. All active slab releases must have
  // completed.  Any modification of this object afterwards
  // will result in an invalid, inconsistent state for the serialized data.
  //
  // @throw std::logic_error if the object state can not be serialized
  serialization::AllocationClassObject saveState() const;

 private:
  // check if the state of the AllocationClass is valid and if not, throws an
  // std::invalid_argument exception. This is intended for use in
  // constructors.
  void checkState() const;

  // grabs a slab from the free slabs and makes it the currentSlab_
  // precondition: freeSlabs_ must not be empty.
  void setupCurrentSlabLocked();

  // returns true if the allocation can be satisfied from the current slab.
  bool canAllocateFromCurrentSlabLocked() const noexcept;

  // returns a new allocation from the current slab. Caller needs to ensure
  // that precondition canAllocateFromCurrentSlabLocked is satisfied
  void* allocateFromCurrentSlabLocked() noexcept;

  // get a suitable slab for being released from either the set of free slabs
  // or the allocated slabs.
  const Slab* getSlabForReleaseLocked() const noexcept;

  // prune the freeAllocs_ to eliminate any  allocs belonging to this slab and
  // also return a list of active allocations. If there are any active
  // allocations, it maintains the freeState for the slab release.
  //
  // @param  slab   Eliminate allocs belonging to this slab
  //
  // @param  shouldAbortFn  invoked in the code to see if this release slab
  //         process should be aborted
  //
  // @return a pair with
  //         a bool indicating if slab release should be aborted or not and
  //         a list of active allocations if should abort is false.
  //
  // @throw exception::SlabReleaseAborted if slab release is aborted due to
  //        shouldAbortFn returning true.
  std::pair<bool, std::vector<void*>> pruneFreeAllocs(
      const Slab* slab,
      SlabReleaseAbortFn shouldAbortFn = []() { return false; });

  // wraps around allFreed and blocks until all the allocations belonging to
  // the slab are freed back.
  void waitUntilAllFreed(const Slab* slab);

  // return the allocation's index into the slab. It is the caller's
  // reponsibility to ensure that the alloc belongs to the slab and is valid.
  size_t getAllocIdx(const Slab* slab, void* alloc) const noexcept;

  // return the allocation pointer into the slab for a given index.
  void* getAllocForIdx(const Slab* slab, size_t idx) const;

  uintptr_t getSlabPtrValue(const Slab* slab) const noexcept {
    return reinterpret_cast<uintptr_t>(slab);
  }

  // Internal logic for checking if an allocation has been freed. This should
  // be called under a lock.
  //
  // @param ctx     release context for the slab owning the alloc
  // @param memory  memory to check
  //
  // @throws std::runtime_error  if the slab cannot be found inside
  //         slabReleaseAllocMap_
  bool isAllocFreedLocked(const SlabReleaseContext& ctx, void* memory) const;

  // Checks if the memory belongs to a slab being released, and if that slab
  // matches with the provided release context.
  //
  // @param ctx     release context for the slab owning this alloc
  // @param memory  memory to check
  //
  // @throws std::invalid_argument if the memory does not belong to a slab of
  //         this slab class, or if the slab is not actively being released, or
  //         if the context belongs to a different slab.
  void checkSlabInRelease(const SlabReleaseContext& ctx,
                          const void* memory) const;

  // @param slab    the slab to create a new release alloc map
  //
  // @return        iterator to the newly created release alloc map
  // throw std::runtime_error if fail to create a new release alloc map
  std::unordered_map<uintptr_t, std::vector<bool>>::iterator
  createSlabReleaseAllocMapLocked(const Slab* slab);

  // @param slab    the slab associated with a release alloc map
  //
  // @return  std::vector<bool>&    this is the alloc state map
  // @throws std::out_of_range if alloc map does not exist
  std::vector<bool>& getSlabReleaseAllocMapLocked(const Slab* slab);

  // Returns allocations that were marked as freed in the slab's release alloc
  // map back onto freedAllocations_. Used when unwinding an aborted slab
  // release so free allocs are not leaked. Must be called with lock_ held.
  //
  // @param slab  the slab whose release alloc map is being unwound
  void restoreFreedAllocsLocked(const Slab* slab);

  // acquires a new slab for this allocation class.
  void addSlabLocked(Slab* slab);

  // allocate memory corresponding to the allocation size of this
  // AllocationClass.
  //
  // @return  ptr to the memory of allocationSize_ chunk or nullptr if we
  //          don't have any free memory. The caller will have to add a slab
  //          to this slab class to make further allocations out of it.
  void* allocateLocked();

  // lock for serializing access to currSlab_, currOffset_, allocatedSlabs_,
  // freeSlabs_, freedAllocations_.
  mutable folly::cacheline_aligned<folly::DistributedMutex> lock_;

  // the allocation class id.
  const ClassId classId_{-1};

  // the allocation pool id.
  const PoolId poolId_{-1};

  // the chunk size for the allocations of this allocation class.
  const uint32_t allocationSize_{0};

  // the offset of the next available allocation.
  uint32_t currOffset_{0};

  // the next available chunk that can be allocated from the current active
  // slab. If nullptr, then there are no active slabs that are being chunked
  // out.
  Slab* currSlab_{nullptr};

  const SlabAllocator& slabAlloc_;

  // slabs that belong to this allocation class and are not entirely free. The
  // un-used allocations in this are present in freedAllocations_.
  // TODO store the index of the slab instead of the actual pointer. Pointer
  // is 8byte vs index which can be half of it.
  std::vector<Slab*> allocatedSlabs_;

  // slabs which are empty and can be used for allocations.
  // TODO use an intrusive container on the freed slabs.
  std::vector<Slab*> freeSlabs_;

  // void* is re-interpreted as FreeAlloc* before being stored in the free
  // list.
  struct CACHELIB_PACKED_ATTR FreeAlloc {
    using CompressedPtrType = facebook::cachelib::CompressedPtr5B;
    using PtrCompressor = facebook::cachelib::
        PtrCompressor<FreeAlloc, SlabAllocator, CompressedPtrType>;
    SListHook<FreeAlloc> hook_{};
  };

  // list of freed allocations for this allocation class.
#if FOLLY_SANITIZE_ADDRESS
  using FreeList = AsanSList<FreeAlloc, &FreeAlloc::hook_>;
#else
  using FreeList = SList<FreeAlloc, &FreeAlloc::hook_>;
#endif
  FreeList freedAllocations_;

  // Partition the 'freeAllocs' into two different SList depending on whether
  // they are in slab memory or outside. Does not take a lock. If access to
  // 'freeAllocs' requires a lock, it should be taken by the caller.
  void partitionFreeAllocs(const Slab* slab,
                           FreeList& freeAllocs,
                           FreeList& inSlab,
                           FreeList& notInSlab);

  // if this is false, then we have run out of memory to do any more
  // allocations. Reading this outside the lock_ will be racy.
  std::atomic<bool> canAllocate_{true};

  std::atomic<int64_t> activeReleases_{0};

  // stores the list of outstanding allocations for a given slab. This is
  // created when we start a slab release process and if there are any active
  // allocations need to be marked as free.
  std::unordered_map<uintptr_t, std::vector<bool>> slabReleaseAllocMap_;

  // Starting releasing a slab is serialized across threads.
  // Afterwards, the multiple threads can proceed in parallel to
  // complete the slab release
  std::mutex startSlabReleaseLock_;

  // maximum number of free allocs to walk through during pruning
  // before dropping the lock
  static constexpr unsigned int kFreeAllocsPruneLimit = 4 * 1024;

  // Number of micro seconds to sleep between the batches during pruning.
  // This is needed to avoid other threads from starving for lock.
  static constexpr unsigned int kFreeAllocsPruneSleepMicroSecs = 1000;

  // Numer of allocations ahead to prefetch when iterating over each allocation
  // in a slab.
  static constexpr unsigned int kForEachAllocPrefetchOffset = 16;

  // Allow access to private members by unit tests
  friend class facebook::cachelib::tests::AllocTestBase;
  FRIEND_TEST(AllocationClassTest, ReleaseSlabMultithread);
};
} // namespace cachelib
} // namespace facebook
