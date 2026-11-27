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

#include <chrono>
#include <limits>

#include "cachelib/allocator/memory/AllocationClass.h"
#include "cachelib/allocator/memory/MemoryPool.h"
#include "cachelib/allocator/memory/MemoryPoolManager.h"
#include "cachelib/allocator/memory/Slab.h"
#include "cachelib/allocator/memory/SlabAllocator.h"
#include "cachelib/shm/ShmCommon.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <folly/Math.h>
#include <folly/Range.h>
#pragma GCC diagnostic pop

#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"

namespace facebook {
namespace cachelib {

// forward declaration.
namespace tests {
class AllocTestBase;
}

/* The following is a brief overview of the different hierarchies in the
 * implementation.
 *
 * MemoryAllocator -- provides allocation by any size up to Slab::kSize.  It
 * consists of a set of MemoryPools. To make an allocation from a pool, the
 * corresponding pool id is to be used. The memory allocator uses the slab
 * allocator to make allocations of Slab::kSize and divides that into smaller
 * allocations. It also takes care of dividing the available memory into
 * different pools at the granularity of a slab.
 *
 * MemoryPool -- deals with memory allocation for a given pool. It contains a
 * collection of AllocationClass instances to actually handle allocations of any
 * size.  MemoryPools are configured to grow up to a given size by the
 * MemoryAllocator that owns it.
 *
 * AllocationClass -- creates allocations of a particular size from slabs
 * belonging to a given memory pool.
 *
 * SlabAllocator -- divides up a contiguous piece of memory into slabs. A slab
 * is a contiguous piece of memory of a pre-defined size (Slab::kSize).
 * Allocated slabs are distributed to different memory pools. The slab
 * allocator maintains the memory required for the slab headers and provides
 * an interface to fetch the header for given slab.
 *
 */

// uses the slab allocator and slab memory pool to actually allocate the memory.
// Read the description at the beginning of the file for more info
class MemoryAllocator {
 public:
  using SerializationType = serialization::MemoryAllocatorObject;

  // maximum number of allocation classes that we support.
  static constexpr unsigned int kMaxClasses = 1 << 7;
  static constexpr ClassId kMaxClassId = kMaxClasses - 1;

  // maximum number of memory pools that we support.
  static constexpr unsigned int kMaxPools = MemoryPoolManager::kMaxPools;
  static constexpr PoolId kMaxPoolId = kMaxPools - 1;
  // default of 8 byte aligned.
  static constexpr uint32_t kAlignment = sizeof(void*);

  // config for the slab memory allocator.
  struct Config {
    Config() {}
    Config(std::set<uint32_t> sizes,
           bool zeroOnRelease,
           bool disableCoredump,
           bool _lockMemory,
           bool _enableAsanPoisoning,
           PageSize _hugePageSize = PageSize())
        : allocSizes(std::move(sizes)),
          enableZeroedSlabAllocs(zeroOnRelease),
          disableFullCoredump(disableCoredump),
          lockMemory(_lockMemory),
          enableAsanPoisoning(_enableAsanPoisoning),
          hugePageSize(_hugePageSize) {}

    // Hint to determine the allocation class sizes
    std::set<uint32_t> allocSizes;

    // Must enable this in order to call `allocateZeroedSlab`.
    // Otherwise, it will throw.
    bool enableZeroedSlabAllocs{false};

    // Exclude memory regions from core dumps
    bool disableFullCoredump{false};

    // Lock and page in the memory for the MemoryAllocator on startup. This is
    // done asynchronously. This is persisted across saved state. To do this
    // for shared memory, no rlimit is required. If the memory for the
    // allocator is not shared, user needs to ensure there are appropriate
    // rlimits setup to lock the memory.
    bool lockMemory{false};

    // When true, slab memory is ASAN-poisoned on free and unpoisoned on
    // allocation so ASAN can detect use-after-free bugs.
    bool enableAsanPoisoning{false};

    // Page size backing slab memory; default => normal pages. Used when the
    // allocator mmaps its memory and when excluding slab memory from core
    // dumps.
    PageSize hugePageSize{0};
  };

  // Creates a memory allocator out of the caller allocated memory region. The
  // memory is owned by the caller and destroying the memory allocator does
  // not free the memory region it was initialized with. The MemoryAllocator
  // only frees up the memory it allocated internally for its operation
  // through malloc.
  //
  // @param  config     The config for the allocator.
  // @param memoryStart The start address of the memory aligned to slab size.
  //                    Cachelib assume that by default the memory is already
  //                    zeroed by the user. Not doing so will result in
  //                    undefined behavior when calling `allocateZeroedSlab`.
  // @param memSize     The size of memory in bytes.
  // @throw std::invalid_argument if the config is invalid or the memory is
  //        passed in is too small to instantiate a slab allocator or if
  //        memoryStart is not aligned to Slab size.
  MemoryAllocator(Config config, void* memoryStart, size_t memSize);

  // same as the above, but creates a mmaped region of the size and tries to
  // unmap the memory on destruction. this instantiation can not be saved and
  // restored.
  //
  // @param config     The config for the allocator.
  // @param memSize     The size of memory in bytes.
  // @throw std::invalid_argument if the config is invalid or the size
  //        passed in is too small to instantiate a slab allocator.
  MemoryAllocator(Config config, size_t memSize);

  // creates a memory allocator by restoring it from a serialized buffer.
  // @param object          Object that contains the data to restore
  //                        MemoryAllocator
  // @param memoryStart     the start of the memory region that was originally
  //                        used to create this memory allocator.
  // @param memSize         the size of the memory region that was originally
  //                        used to create this memory allocator
  // @param disableCoredump exclude mapped region from core dumps
  // @param enableAsanPoisoning When true, slab memory is ASAN-poisoned
  // @param hugePageSize    Page size backing the restored memory region
  MemoryAllocator(const serialization::MemoryAllocatorObject& object,
                  void* memoryStart,
                  size_t memSize,
                  bool disableCoredump,
                  bool enableAsanPoisoning,
                  PageSize hugePageSize = PageSize());

  MemoryAllocator(const MemoryAllocator&) = delete;
  MemoryAllocator& operator=(const MemoryAllocator&) = delete;

  // returns true if the memory allocator is restorable. false otherwise.
  bool isRestorable() const noexcept { return slabAllocator_.isRestorable(); }

  // allocate memory of corresponding size.
  //
  // @param id    the pool id to be used for this allocation.
  // @param size  the size for the allocation.
  // @return pointer to the memory corresponding to the allocation. nullptr if
  // memory is not available.
  //
  // @throw std::invalid_argument if the poolId is invalid or the size is
  //        invalid.
  void* allocate(PoolId id, uint32_t size);

  // Allocate a zeroed Slab
  //
  // This guarantees the content of the allocated slab is zero because when
  // we release a slab back to free slabs in a memory pool or slab allocator,
  // we zero out the content of the slab
  //
  // @param id         the pool id to be used for this allocation.
  //
  // @throw std::logic_error if config_.enableZeroedSlabAllocs == false
  // @throw std::invalid_argument if the poolId is invalid
  void* allocateZeroedSlab(PoolId id);

  // free the memory back to the allocator.
  //
  // @throw std::invalid_argument if the memory does not belong to any active
  //        allocation handed out by this allocator.
  void free(void* memory);

  // Memory pool interface. The memory pools must be established before the
  // first allocation happens. Currently we dont support adding / removing
  // pools dynamically.
  //
  // @param name      the name of the pool
  // @param size      the size of the pool
  // @param allocSize the set of allocation sizes for this memory pool,
  //                  if empty, a default one will be used
  // @param ensureProvisionable   ensures that the size of the pool is enough
  //                              to provision one slab to each allocation class
  //
  // @return a valid pool id that the caller can use on successful return.
  //
  // @throws std::invalid_argument if the name or size is inappropriate or
  //         if there is not enough space left for this pool.
  //         std::logic_error if we have run out the allowed number of pools.
  PoolId addPool(folly::StringPiece name,
                 size_t size,
                 const std::set<uint32_t>& allocSizes = {},
                 bool ensureProvisionable = false);

  // This should only be called on cache startup on a new memory pool. Provision
  // a pool by filling up each allocation class with prescribed number of slabs.
  // This is useful for users that know their workload distribution in
  // allocation sizes.
  //
  // @param poolId              id of the pool to provision
  // @param slabsDistribution   number of slabs in each AC
  // @return true if we have enough memory and filled each AC successfully
  //         false otherwise. On false, we also revert all provisioned ACs.
  bool provisionPool(PoolId pid,
                     const std::vector<uint32_t>& slabsDistribution) {
    return memoryPoolManager_.provisionPool(pid, slabsDistribution);
  }

  // Limit how many unallocated slabs each allocation class of the pool may
  // take while it fills. See MemoryPool::setSlabCaps.
  void setPoolSlabCaps(PoolId pid,
                       std::vector<uint32_t> caps,
                       std::chrono::seconds maxDuration) {
    memoryPoolManager_.getPoolById(pid).setSlabCaps(std::move(caps),
                                                    maxDuration);
  }

  // See MemoryPool::updateSlabCaps.
  void updatePoolSlabCaps(PoolId pid, std::vector<uint32_t> caps) {
    memoryPoolManager_.getPoolById(pid).updateSlabCaps(std::move(caps));
  }

  bool poolHasSlabCaps(PoolId pid) const {
    return memoryPoolManager_.getPoolById(pid).hasSlabCaps();
  }

  // shrink the existing pool by _bytes_ .
  // @param id     the id for the pool
  // @param bytes  the number of bytes to be taken away from the pool
  // @return  true if the operation succeeded. false if the size of the pool is
  //          smaller than _bytes_
  // @throw   std::invalid_argument if the poolId is invalid.
  bool shrinkPool(PoolId pid, size_t bytes) {
    return memoryPoolManager_.shrinkPool(pid, bytes);
  }

  // grow an existing pool by _bytes_. This will fail if there is no
  // available memory across all the pools to provide for this pool
  // @param id     the pool id to be grown.
  // @param bytes  the number of bytes to be added to the pool.
  // @return    true if the pool was grown. false if the necessary number of
  //            bytes were not available.
  // @throw     std::invalid_argument if the poolId is invalid.
  bool growPool(PoolId pid, size_t bytes) {
    return memoryPoolManager_.growPool(pid, bytes);
  }

  // move bytes from one pool to another. The source pool should be at least
  // _bytes_ in size.
  //
  // @param src     the pool to be sized down and giving the memory.
  // @param dest    the pool receiving the memory.
  // @param bytes   the number of bytes to move from src to dest.
  // @param   true if the resize succeeded. false if src does does not have
  //          correct size to do the transfer.
  // @throw   std::invalid_argument if src or dest is invalid pool
  bool resizePools(PoolId src, PoolId dest, size_t bytes) {
    return memoryPoolManager_.resizePools(src, dest, bytes);
  }

  // Start the process of releasing a slab from this allocation class id and
  // pool id. The release could be for a pool resizing or allocation class
  // rebalancing. If a valid context is returned, the caller needs to free the
  // active allocations in the valid context and call completeSlabRelease. A
  // null context indicates that a slab was successfully released. throws on
  // any other error.
  //
  // @param pid       the pool id
  // @param victim    the allocation class id in the pool. if invalid, we try
  //                  to pick any free slab that is available from the pool.
  // @param receiver  the allocation class that will get a slab
  // @param mode  the mode for slab release (rebalance/resize)
  // @param hint  hint referring to the slab. this can be an allocation that
  //              the user knows to exist in the slab. If this is nullptr, a
  //              random slab is selected from the pool and allocation class.
  // @param  shouldAbortFn invoked in the code to see if this release slab
  //         process should be aborted
  //
  // @return  a valid context. If the slab is already released, then the
  //          caller needs to do nothing. If it is not released, then the caller
  //          needs to free the allocations and call completeSlabRelease with
  //          the same context.
  //
  // @throw std::invalid_argument if the hint is invalid or if the pid or cid
  //        is invalid. Or if the mode is set to kResize but the receiver is
  //        also specified. Receiver class id can only be specified if the mode
  //        is set to kRebalance.
  // @throw exception::SlabReleaseAborted if slab release is aborted due to
  //        shouldAbortFn returning true.
  SlabReleaseContext startSlabRelease(
      PoolId pid,
      ClassId victim,
      ClassId receiver,
      SlabReleaseMode mode,
      const void* hint = nullptr,
      SlabReleaseAbortFn shouldAbortFn = []() { return false; });

  // Check if an alloc is free (during slab release)
  //
  // @param ctx     SlabReleaseContext to enforce that this is only called
  //                during slab release.
  // @param memory  alloc being checked.
  //
  // @return true  if the alloc is free.
  //
  // @throws std::invalid_argument  if the memory does not belong to a slab of
  //         this slab class, or if the slab is not actively being released, or
  //         if the context belongs to a different slab.
  bool isAllocFreed(const SlabReleaseContext& ctx, void* memory) const;

  // Check if the slab has all its active allocations freed.
  //
  // @param ctx context returned by startSlabRelease.
  // @return    true if all allocs have been freed back to the allcoator
  //            false otherwise
  //
  // @throw std::invalid_argument if the pool id or allocation class id
  //        associated with the context is invalid.
  //
  //        std::runtime_error if the slab associatec with the context
  //        does not have the allocStateMap entry.
  bool allAllocsFreed(const SlabReleaseContext& ctx) const;

  // See AllocationClass::processAllocForRelease
  void processAllocForRelease(const SlabReleaseContext& ctx,
                              void* memory,
                              const std::function<void(void*)>& callback) const;

  // Aborts the slab release process when there were active allocations in
  // the slab. This should be called with the same non-null context that was
  // created using startSlabRelease and after the user FAILS to free all the
  // active allocations in the context. The state of the allocation class may
  // not exactly same as pre-startSlabRelease state because freed allocations
  // while trying to release the slab are not restored.
  //
  // @param context  the context returned by startSlabRelease
  //
  // @throw std::invalid_argument if the context is invalid or
  //        context is already released or all allocs in the context are
  //        free
  void abortSlabRelease(const SlabReleaseContext& context);

  // completes the slab release process when there were active allocations in
  // the slab. This should be called with the same non-null context that was
  // created using startSlabRelease and after the user frees all the active
  // allocations in the context. After this, the slab is released appropriately.
  // Calling this with a context that has the slab already released is a no-op.
  // This will block until all the active allocations are completely returned
  // to the allocator.
  //
  // @param context  a valid context
  // @throw std::invalid_argument if the context is invalid.
  //        Or if the mode is set to kResize but the receiver is
  //        also specified. Receiver class id can only be specified if the mode
  //        is set to kRebalance.
  void completeSlabRelease(const SlabReleaseContext& context);

  // The startSlabRelease/completeSlabRelease methods are used with
  // SlabReleaseContext::kAdvise to advise away slabs, one at a time,
  // under memory pressure. Typically, pools are asked to advise away the
  // number of slabs that is proportional to their current size to avoid
  // disproportionately affecting some pools over others. When there is plenty
  // of free memory, pools are asked to reclaim slabs using
  // reclaimSlabsAndGrow() method below to reclaim slabs in proportion
  // to their current size.

  // Advising away slabs reduces the total memory size of the cache reported by
  // slab allocator as well as the individual pool's max and used sizes,
  // reflecting the fact cache size and pool sizes have reduced. The
  // numSlabsReclaimable() method provides the count of advised away slabs
  // and therefore the reduced memory size.

  // Reclaim the given number of advised away slabs from the slab allocator
  // for the given pool. If the numSlabs exceeds the number of advised away
  // slabs (numSlabsReclaimable()), then number of slabs reclaimed is
  // equal to numSlabsReclaimable().
  //
  // @return the number of slabs reclaimed
  size_t reclaimSlabsAndGrow(PoolId id, size_t numSlabs) {
    auto& pool = memoryPoolManager_.getPoolById(id);
    return pool.reclaimSlabsAndGrow(numSlabs);
  }

  // Number of slabs that are advised away and can be reclaimed.
  size_t numSlabsReclaimable() const noexcept {
    return slabAllocator_.numSlabsReclaimable();
  }

  // get the PoolId corresponding to the pool name.
  //
  // @param name  the name of the pool
  // @return  poold id corresponding to the name if it exists or
  //          kInvalidPoolId if name is not a recognized pool.
  PoolId getPoolId(std::string_view name) const noexcept;

  // get the pool name corresponding to its PoolId
  //
  // @param id  the id of the pool
  // @return    pool name of this pool
  // @throw std::logic_error if the pool id is invalid.
  std::string getPoolName(PoolId id) const {
    return memoryPoolManager_.getPoolNameById(id);
  }

  // return the usable size in bytes for this allocator.
  size_t getMemorySize() const noexcept {
    return slabAllocator_.getNumUsableSlabs() * Slab::kSize;
  }

  // return the usable size including the advised away size in bytes
  // for this allocator.
  size_t getMemorySizeInclAdvised() const noexcept {
    return slabAllocator_.getNumUsableAndAdvisedSlabs() * Slab::kSize;
  }

  size_t getUnreservedMemorySize() const noexcept {
    return memoryPoolManager_.getBytesUnReserved();
  }

  // return the usable size in bytes for this allocator given the memory size
  // and assuming no advised away slabs
  static size_t getMemorySize(size_t memorySize) noexcept {
    return SlabAllocator::getNumUsableSlabs(memorySize) * Slab::kSize;
  }

  // return the total memory advised away
  size_t getAdvisedMemorySize() const noexcept {
    return memoryPoolManager_.getAdvisedMemorySize();
  }

  // return the list of pool ids for this allocator.
  std::set<PoolId> getPoolIds() const {
    return memoryPoolManager_.getPoolIds();
  }

  // fetches the memory pool for the id if one exists. This is purely to get
  // information out of the pool.
  //
  // @return const reference to memory pool for the id if one exists.
  // @throw std::invalid_argument if the pool id is invalid.
  const MemoryPool& getPool(PoolId id) const {
    return memoryPoolManager_.getPoolById(id);
  }

  // obtain list of pools that are currently occupying more memory than their
  // current limit.
  std::set<PoolId> getPoolsOverLimit() const {
    return memoryPoolManager_.getPoolsOverLimit();
  }

  // return true if all the memory for the allocator is allocated to some
  // pool.
  // this is leveraged by pool rebalancers to determine if the rebalancing has
  // to start.
  bool allSlabsAllocated() const noexcept {
    return slabAllocator_.allSlabsAllocated();
  }

  // returns true if all the slab memory for the pool is accounted for in some
  // allocation class belonging to the pool.
  //
  // @throw std::invalid_argument if the pool id does not belong to a valid
  // pool.
  bool allSlabsAllocated(PoolId pid) const {
    return getPool(pid).allSlabsAllocated();
  }

  // fetch the pool and allocation class information for the memory
  // corresponding to a memory allocation from the allocator. Caller is
  // expected to supply a memory that is valid and allocated from this
  // allocator.
  //
  // @param memory  the memory belonging to the slab allocator
  // @return        pair of poolId and classId of the memory
  // @throw std::invalid_argument if the memory doesn't belong to allocator
  FOLLY_ALWAYS_INLINE AllocInfo getAllocInfo(const void* memory) const {
    const auto* header = slabAllocator_.getSlabHeader(memory);
    if (!header) {
      throw std::invalid_argument(
          fmt::format("invalid header for slab memory addr: {}", memory));
    }
    return AllocInfo{header->getPoolId(), header->getClassId(),
                     header->getAllocSize()};
  }

  // Check whether a pointer falls within a valid, allocated slab managed by
  // this allocator.
  //
  // @param memory  the memory address to check
  // @return        true if the address belongs to a slab with a valid header
  bool isValidAllocMemory(const void* memory) const noexcept {
    auto* slab = slabAllocator_.getSlabForMemory(memory);
    return (slab && slabAllocator_.isValidSlab(slab));
  }

  // Get slab memory base address and size
  std::pair<const void*, size_t> getSlabMemoryInfo() const noexcept {
    return slabAllocator_.getSlabMemoryInfo();
  }

  // fetch the allocation size for the pool id and class id.
  //
  // @param pid  the pool id
  // @param cid  the allocation class id
  //
  // @return the allocation size corresponding to this pair.
  // @throw std::invalid_argument if the ids are invalid.
  uint32_t getAllocSize(PoolId pid, ClassId cid) const {
    const auto& pool = getPool(pid);
    const auto& allocClass = pool.getAllocationClass(cid);
    return allocClass.getAllocSize();
  }

  // return the default allocation sizes for this allocator.
  const std::set<uint32_t>& getAllocSizes() const noexcept {
    return config_.allocSizes;
  }

  // fetch a random allocation in memory.
  // this does not guarantee the allocation is in any valid state.
  //
  // @return a tuple containing the alloc size and the start address of the
  //         allocation (nullptr if the allocation is in invalid state
  //         according to allocator).
  std::tuple<uint32_t, const void*> getRandomAlloc() const noexcept {
    return slabAllocator_.getRandomAlloc();
  }

  // fetch the allocation class info corresponding to a given size in a pool.
  //
  // @param poolId  the pool to be allocated from
  // @param nBytes  the allocation size
  // @return        a valid class id on success
  // @throw   std::invalid_argument if the poolId is invalid or the size is
  //          outside of the allocation sizes for the memory pool.
  ClassId getAllocationClassId(PoolId poolId, uint32_t nBytes) const;

  // for saving the state of the memory allocator
  //
  // precondition:  The object must have been instantiated with a restorable
  // slab allocator that does not own the memory. serialization must happen
  // without any reader or writer present. Any modification of this object
  // afterwards will result in an invalid, inconsistent state for the
  // serialized data.
  //
  // @throw std::logic_error if the object state can not be serialized
  serialization::MemoryAllocatorObject saveState();

  template <typename PtrType, typename CompressedPtrType>
  using PtrCompressorType = facebook::cachelib::
      PtrCompressor<PtrType, SlabAllocator, CompressedPtrType>;

  template <typename PtrType, typename CompressedPtrType>
  PtrCompressorType<PtrType, CompressedPtrType> createPtrCompressor() {
    return slabAllocator_.createPtrCompressor<PtrType, CompressedPtrType>();
  }

  // compress a given pointer to a valid allocation made out of this allocator
  // through an allocate() or nullptr. Calling this otherwise with invalid
  // pointers leads to undefined behavior. It is guranteed to not throw if the
  // pointer is valid.
  //
  // @param  ptr    valid pointer to allocated memory.
  // @return        A compressed pointer that corresponds to the same
  //                allocation.  This can be stored and decompressed as long
  //                as the original pointer is valid.
  //
  // @throw  std::invalid_argument if the ptr is invalid.
  template <typename CompressedPtrType>
  CompressedPtrType CACHELIB_INLINE compress(const void* ptr,
                                             bool isMultiTiered) const {
    return slabAllocator_.compress<CompressedPtrType>(ptr, isMultiTiered);
  }

  // retrieve the raw pointer corresponding to the compressed pointer. This is
  // guaranteed to succeed as long as the pointer corresponding to this was
  // never freed back to the allocator.
  //
  // @param cPtr    the compressed pointer
  // @return        the raw pointer corresponding to this compressed pointer.
  //
  // @throw   std::invalid_argument if the compressed pointer is invalid.
  template <typename CompressedPtrType>
  void* CACHELIB_INLINE unCompress(const CompressedPtrType& cPtr,
                                   bool isMultiTiered) const {
    return slabAllocator_.unCompress<CompressedPtrType>(cPtr, isMultiTiered);
  }

  // Prefetches the memory of a possibly stale compressed pointer, see
  // SlabAllocator::prefetch().
  template <typename CompressedPtrType>
  void prefetch(const CompressedPtrType& cPtr,
                bool isMultiTiered) const noexcept {
    slabAllocator_.prefetch<CompressedPtrType>(cPtr, isMultiTiered);
  }

  // a special implementation of pointer compression for benchmarking purposes.
  CompressedPtr4B CACHELIB_INLINE compressAlt(const void* ptr) const {
    return slabAllocator_.compressAlt(ptr);
  }

  void* CACHELIB_INLINE unCompressAlt(const CompressedPtr4B cPtr) const {
    return slabAllocator_.unCompressAlt(cPtr);
  }

  // Traverse each slab and call user defined callback on each allocation
  // within the slab. Callback will be invoked if the slab is not advised,
  // marked for release or currently being moved. Callbacks will be invoked
  // irrespective of whether the slab is allocated or free.
  //
  // @param callback   Callback to be executed on each allocation
  // @return           The number of slabs skipped
  //                   Slab can be skipped because it is being released or
  //                   already released but not yet assigned to another pool or
  //                   allocation class.
  template <typename AllocTraversalFn>
  uint64_t forEachAllocation(AllocTraversalFn&& callback) {
    uint64_t slabSkipped = 0;
    for (unsigned int idx = 0; idx < slabAllocator_.getNumUsableSlabs();
         ++idx) {
      Slab* slab = slabAllocator_.getSlabForIdx(idx);
      const auto slabHdr = slabAllocator_.getSlabHeader(slab);
      if (!slabHdr) {
        continue;
      }
      auto classId = slabHdr->getClassId();
      auto poolId = slabHdr->getPoolId();
      if (poolId == Slab::kInvalidPoolId || classId == Slab::kInvalidClassId ||
          slabHdr->isAdvised() || slabHdr->isMarkedForRelease()) {
        ++slabSkipped;
        continue;
      }
      auto& pool = memoryPoolManager_.getPoolById(poolId);
      auto slabIterationStatus = pool.forEachAllocation(
          classId, slab, std::forward<AllocTraversalFn>(callback));
      if (slabIterationStatus ==
          SlabIterationStatus::kSkippedCurrentSlabAndContinue) {
        ++slabSkipped;
      } else if (slabIterationStatus == SlabIterationStatus::kAbortIteration) {
        return slabSkipped;
      }
    }
    return slabSkipped;
  }

  // returns a default set of allocation sizes with given size range and factor.
  //
  // @param factor      the factor by which the alloc sizes grow.
  // @param maxSize     the maximum allowed allocation size
  // @param minSize     the minimum allowed allocation size
  // @param reduceFragmentation if true chunk sizes will be increased to the
  //                            maximum size that maintains the number of chunks
  //                            per slab as determined using factor.
  //
  // @return    std::set of allocation sizes that all fit within maxSize.
  //
  // @throw std::invalid_argument if the maxSize is more than the slab size.
  // @throw std::invalid_argument if the factor is <= 1.0
  // @throw std::invalid_argument if the factor is not incrementing large
  //                              enough when reduceFragmentation is enabled

  static std::set<uint32_t> generateAllocSizes(
      double factor = 1.25,
      uint32_t maxSize = Slab::kSize,
      uint32_t minSize = 72,
      bool reduceFragmentation = false);

  // Generates an optimal set of allocation sizes for a given range of item
  // sizes. This function creates allocation classes that minimize slab
  // fragmentation.
  //
  // For example: if minItemSize=33904 and maxItemSize=103496 and slab
  // size=4MB.
  // 4MB / 33904 ~ 123.71
  // 4MB / 103496 ~ 40.52
  // This function will create 83 allocation classes: [aligned(4MB / 40),
  // aligned(4MB / 41), ..., aligned(4MB / 123)].
  //
  //
  // @param minItemSize  The minimum expected item size in bytes
  // @param maxItemSize  The maximum expected item size in bytes
  //
  // @return  std::set of allocation sizes that optimally cover the item size
  //          range. Each allocation size is aligned to kAlignment (8 bytes).
  //
  // @throw std::invalid_argument if minItemSize > maxItemSize
  // @throw std::invalid_argument if maxItemSize > Slab::kSize
  // @throw std::invalid_argument if the optimal number of allocation classes
  //                              exceeds kMaxClasses
  static std::set<uint32_t> generateOptimalAllocSizesForItemRange(
      uint32_t minItemSize, uint32_t maxItemSize);

  // Generates a set of allocation sizes with evenly distributed sizes.
  // This function creates allocation classes by dividing the space between
  // minItemSize and maxItemSize evenly. The maxItemSize is always included,
  // and intermediate sizes are evenly distributed based on numClassesToAdd.
  //
  // For example: if minItemSize=1000, maxItemSize=5000, and numClassesToAdd=2,
  // it will create 2 allocation classes: [3000, 5000].
  // If numClassesToAdd=3, it will create: [2333, 3666, 5000].
  //
  // @param minItemSize      The minimum expected item size in bytes
  // @param maxItemSize      The maximum expected item size in bytes
  // @param numClassesToAdd  The total number of allocation classes to create.
  //                         Must be at least 1 (which adds only maxItemSize).
  //
  // @return  std::set of allocation sizes that evenly cover the item size
  //          range. Each allocation size is aligned to kAlignment (8 bytes)
  //          and maximized using maximizeAllocSize.
  //
  // @throw std::invalid_argument if minItemSize > maxItemSize
  // @throw std::invalid_argument if maxItemSize > Slab::kSize
  // @throw std::invalid_argument if numClassesToAdd < 1
  // @throw std::invalid_argument if numClassesToAdd exceeds kMaxClasses
  static std::set<uint32_t> generateEvenlyDistributedAllocSizes(
      uint32_t minItemSize, uint32_t maxItemSize, uint32_t numClassesToAdd);

  // Checks if a given allocation size is valid for cachelib. A valid
  // allocation size must satisfy the following requirements:
  // 1. >= Slab::kMinAllocSize (64 bytes)
  // 2. <= Slab::kSize (4MB)
  // 3. Properly aligned to kAlignment (8 bytes)
  //
  // @param size  the allocation size to validate
  //
  // @return true if the size is valid, false otherwise
  static bool isValidAllocSize(uint32_t size) {
    if (size < Slab::kMinAllocSize) {
      return false;
    } else if (size > Slab::kSize) {
      return false;
    } else if (size % kAlignment != 0) {
      return false;
    }
    return true;
  }

  // Maximizes the allocation size while maintaining the number of allocations
  // per slab and alignment. Given a size, this function calculates the maximum
  // allocation size that maintains the same number of allocations per slab.
  //
  // For example: If slabs are 4MB and the input size is 900KB, this allows
  // 4096KB / 900KB = 4 allocations per slab. This leaves 496KB of space
  // unusable in each slab. This function returns the maximum allocation size
  // that still allows 4 allocations per slab but reduces external
  // fragmentation. For this example, it would return 1MB.
  //
  // @param size      the initial allocation size
  // @param slabSize  the size of a slab
  // @param alignment the alignment requirement for allocation sizes
  //
  // @return the maximum aligned allocation size that maintains the same number
  // of allocations per slab
  static uint32_t maximizeAllocSize(uint32_t size,
                                    uint32_t slabSize,
                                    uint32_t alignment) {
    const uint32_t perSlab = slabSize / size;
    XDCHECK_GT(perSlab, 0ULL);
    const uint32_t maxAllocSize = slabSize / perSlab;
    // Align down to maintain same number of allocations per slab
    const uint32_t alignedSize = maxAllocSize - maxAllocSize % alignment;
    XDCHECK_EQ(alignedSize % alignment, 0ULL);
    XDCHECK_EQ(slabSize / alignedSize, perSlab);
    return alignedSize;
  }

  // get the total number of slabs advised away across all pools
  size_t getNumSlabsAdvised() const noexcept {
    return memoryPoolManager_.getNumSlabsAdvised();
  }

  // get the total number of slabs advised away across the given pools
  //
  // @throw std::invalid_argument if a pool id is not a valid pool
  size_t getNumSlabsAdvised(const std::set<PoolId>& poolIds) const {
    return memoryPoolManager_.getNumSlabsAdvised(poolIds);
  }

  // calculate the number of slabs to be advised/reclaimed in each pool
  //
  // @param numSlabsToAdvise  total number of slabs to advise across all pools
  // @param poolIds           list of pools to process
  //
  // @return   PoolAdviseReclaimData containing poolId,
  //           the number of slabs to advise or number of slabs to reclaim
  //           and flag indicating if the number is for advising-away or
  //           reclaiming
  PoolAdviseReclaimData calcNumSlabsToAdviseReclaim(
      size_t numSlabsToAdvise, const std::set<PoolId>& poolIds) {
    return memoryPoolManager_.calcNumSlabsToAdviseReclaim(numSlabsToAdvise,
                                                          poolIds);
  }

  // return the minimum allocation size
  uint32_t getMinAllocSize() const noexcept {
    return slabAllocator_.getMinAllocSize();
  }

 private:
  // @param memory    pointer to the memory.
  // @return          the MemoryPool corresponding to the memory.
  // @throw std::invalid_argument if the memory does not belong to any active
  //        allocation handed out by this allocator.
  MemoryPool& getMemoryPool(const void* memory) const;

  // the config for the allocator.
  const Config config_;

  // the instance of slab allocator we will use to allocate slabs.
  SlabAllocator slabAllocator_;

  // the instance used for book keeping information about the memory pools
  // configuration.
  MemoryPoolManager memoryPoolManager_;

  // Allow access to private members by unit tests
  friend class facebook::cachelib::tests::AllocTestBase;
};
} // namespace cachelib
} // namespace facebook
