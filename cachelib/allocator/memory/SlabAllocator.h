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

#include <sanitizer/asan_interface.h>
#include <sys/mman.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <fmt/core.h>

#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"
#pragma GCC diagnostic pop
#include <folly/logging/xlog.h>
#include <folly/synchronization/SanitizeThread.h>

#include "cachelib/allocator/memory/CompressedPtr.h"
#include "cachelib/allocator/memory/Slab.h"
#include "cachelib/shm/ShmCommon.h"

namespace facebook {
namespace cachelib {

// forward declarations
namespace tests {
class AllocTestBase;
}

// Given a contiguous piece of memory, divides it up into slabs. The slab
// allocator is also responsible for providing memory for the slab headers for
// each slab.
class SlabAllocator {
 public:
  struct Config {
    Config() {}
    Config(bool _excludeFromCoreDump,
           bool _lockMemory,
           bool _enableAsanPoisoning,
           PageSize _hugePageSize = PageSize())
        : excludeFromCoredump(_excludeFromCoreDump),
          lockMemory(_lockMemory),
          enableAsanPoisoning(_enableAsanPoisoning),
          hugePageSize(_hugePageSize) {}

    // exclude the memory region from core dumps
    bool excludeFromCoredump{false};

    // lock the pages in memory, forcing to allocate them and retaining them in
    // memory even when untouched.
    bool lockMemory{false};

    // When true, slab memory is ASAN-poisoned on free and unpoisoned on
    // allocation so ASAN can detect use-after-free bugs.
    bool enableAsanPoisoning{false};

    // Page size backing the slab memory; default => normal pages. Used to
    // allocate mmap-owned memory.
    PageSize hugePageSize{};
  };

  // initialize the slab allocator for the range of memory starting from
  // memoryStart, up to memorySize bytes. The available memory is divided into
  // space for slab headers and slabs. When created this way, the slab
  // allocator does not own the memory. It just manages allocations on top of
  // it. Destroying the object does not free up the memory.
  //
  // @param memoryStart   the start of the memory aligned to slab size.
  // @param memorySize    the size of the memory.
  // @param config        the config for this allocator
  //
  // @throw std::invalid_argument if the memoryStart is not aligned to Slab
  // size or if memorySize is incorrect.
  SlabAllocator(void* memoryStart, size_t memorySize, const Config& config);

  // same as the above, but allocates the memory using mmap and munmaps it
  // on destruction. Instantiating through this means you cannot save the
  // state and restore the slab allocator since the memory is destroyed once
  // the object is destroyed.
  SlabAllocator(size_t memorySize, const Config& config);

  // free up and unmap the mmaped memory if the allocator was created with
  // one.
  ~SlabAllocator();

  SlabAllocator(const SlabAllocator&) = delete;
  SlabAllocator& operator=(const SlabAllocator&) = delete;

  // restore the state of the slab allocator from a previous instance. The
  // caller needs to ensure that the same memory region of the appropriate
  // size is already mapped into the process.
  //
  // @param object        Object that contains the data to restore SlabAllocator
  // @param memoryStart   the start of the memory that was originally used
  // @param memorySize    the size of the memory that was originally used
  // @param config        the new config for this allocator
  //
  // @throw std::invalid_argument if the memoryStart/size does not match the
  //        previous instance of the same allocator
  SlabAllocator(const serialization::SlabAllocatorObject& object,
                void* memoryStart,
                size_t memorySize,
                const Config& config);

  // returns true if the slab allocator can be restored through serialized
  // state. This is a precondition to calling saveState.
  bool isRestorable() const noexcept { return !ownsMemory_; }

  using LockHolder = std::unique_lock<std::mutex>;

  // return true if any more slabs can be allocated from the slab allocator at
  // this point of time.
  bool allSlabsAllocated() const {
    LockHolder l(lock_);
    return allMemorySlabbed() && freeSlabs_.empty();
  }

  // fetch a random allocation in memory.
  // this does not guarantee the allocation is in a valid state.
  //
  // @return a tuple containing the alloc size and the start address of the
  //         allocation (nullptr if the allocation is in invalid state
  //         according to allocator).
  std::tuple<uint32_t, const void*> getRandomAlloc() const noexcept;

  // grab an empty slab from the slab allocator if one is available.
  //
  // @param id  the pool id.
  // @return  pointer to a new slab of memory.
  Slab* makeNewSlab(PoolId id);

  // frees a used slab back to the slab allocator.
  //
  // @throw throws std::runtime_error if the slab is invalid
  void freeSlab(Slab* slab);

  // Frees a used slab and advises it away to free its memory.
  // The slab goes into an advised slabs pool to enable
  // reclaiming its memory later.
  //
  // @throw throws std::runtime_error if the slab is invalid
  // @return true if madvise succeeded
  bool adviseSlab(Slab* slab);

  // Page in the pages for the slab into memory that were previously
  // madvised away for the given pool.
  Slab* reclaimSlab(PoolId id);

  // Number of advised away slabs that can be reclaimed by calling reclaimSlab()
  size_t numSlabsReclaimable() const noexcept {
    LockHolder l(lock_);
    return advisedSlabs_.size();
  }

  // Assumes there are no slabs that have been advised away from given memory.
  // returns the number of usable slabs for the given amount of memory in
  // bytes aligned to Slab's size.
  static unsigned int getNumUsableSlabs(size_t memorySize) noexcept;

  // returns the number of slabs that the cache can hold excluding
  // advised away slabs, which are not usable.
  unsigned int getNumUsableSlabs() const noexcept;

  // return the number of slabs that the cache can hold
  unsigned int getNumUsableAndAdvisedSlabs() const noexcept;

  // returns the SlabHeader for the memory address or nullptr if the memory
  // is invalid. Hotly accessed for getting alloc info
  FOLLY_ALWAYS_INLINE SlabHeader* getSlabHeader(
      const void* memory) const noexcept {
    const auto* slab = getSlabForMemory(memory);
    if (LIKELY(isValidSlab(slab))) {
      const auto slabIndex = static_cast<SlabIdx>(slab - slabMemoryStart_);
      return getSlabHeader(slabIndex);
    }
    return nullptr;
  }

  // return the SlabHeader for the given slab or nullptr if the slab is
  // invalid
  SlabHeader* getSlabHeader(const Slab* const slab) const noexcept;

  // for saving the state of the slab allocator state and reattaching.
  //
  // precondition:  The object must have been instantiated with the
  // constructor that does not own the memory. serialization must happen
  // without any reader or writer present. Any modification of this object
  // afterwards will result in an invalid, inconsistent state for the
  // serialized data.
  //
  // @throw std::logic_error if the object state can not be serialized
  serialization::SlabAllocatorObject saveState();

  // returns ture if ptr points to memory in the slab and the slab is a valid
  // slab, false otherwise.
  bool isMemoryInSlab(const void* ptr, const Slab* slab) const noexcept;

  // true if the slab is a valid allocated slab in the memory belonging to this
  // allocator.
  FOLLY_ALWAYS_INLINE bool isValidSlab(const Slab* slab) const noexcept {
    // suppress TSAN race error, this is harmless because nextSlabAllocation_
    // cannot go backwards and slab can't become invalid once it is valid
    folly::annotate_ignore_thread_sanitizer_guard g(__FILE__, __LINE__);
    return slab >= slabMemoryStart_ && slab < nextSlabAllocation_ &&
           getSlabForMemory(static_cast<const void*>(slab)) == slab;
  }

  // returns the slab in which the memory resides, irrespective of the
  // validity of the memory. The caller can use isValidSlab to check if the
  // returned slab is valid.
  FOLLY_ALWAYS_INLINE const Slab* getSlabForMemory(
      const void* memory) const noexcept {
    // returns the closest slab boundary for the memory address.
    auto offset = reinterpret_cast<uintptr_t>(memory) & ~kAddressMask;
    return reinterpret_cast<const Slab*>(
        reinterpret_cast<const uint8_t*>(memory) - offset);
  }

  using SlabIdx = uint32_t;

  // compress and uncompress a pointer in the slab memory to/from a smaller
  // representation. The pointer must belong to a valid allocation made out of
  // the corresponding memory allocator. trying to inline this just increases
  // the code size and does not move the needle on the benchmarks much.
  // Calling this with invalid input in optimized build is undefined behavior.
  template <typename CompressedPtrType>
  CompressedPtrType CACHELIB_INLINE compress(const void* ptr,
                                             bool isMultiTiered) const {
    if (ptr == nullptr) {
      return CompressedPtrType{};
    }

    const Slab* slab = getSlabForMemory(ptr);
    if (!isValidSlab(slab)) {
      throw std::invalid_argument(fmt::format("Invalid pointer ptr {}", ptr));
    }

    const auto slabIndex = static_cast<SlabIdx>(slab - slabMemoryStart_);
    const SlabHeader* header = getSlabHeader(slabIndex);

    const uint32_t allocSize = header->getAllocSize();
    XDCHECK(allocSize >= getMinAllocSize());

    const auto allocIdx =
        static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(ptr) -
                              reinterpret_cast<const uint8_t*>(slab)) /
        allocSize;
    return CompressedPtrType{slabIndex, allocIdx, isMultiTiered};
  }

  // uncompress the point and return the raw ptr.  This function never throws
  // in optimized build and assumes that the caller is responsible for calling
  // it with a valid compressed pointer.
  template <typename CompressedPtrType>
  void* CACHELIB_INLINE unCompress(const CompressedPtrType& ptr,
                                   bool isMultiTiered) const {
    if (ptr.isNull()) {
      return nullptr;
    }

    /* TODO: isMultiTiered set to false by default.
       Multi-tiering flag will have no impact till
       rest of the multi-tiering changes are merged.
     */
    const SlabIdx slabIndex = ptr.getSlabIdx(isMultiTiered);
    const uint32_t allocIdx = ptr.getAllocIdx();
    const Slab* slab = &slabMemoryStart_[slabIndex];

#ifndef NDEBUG
    if (UNLIKELY(!isValidSlab(slab))) {
      throw std::invalid_argument(
          fmt::format("Invalid slab index {}", slabIndex));
    }
#endif

    const auto* header = getSlabHeader(slabIndex);
    const uint32_t allocSize = header->getAllocSize();

    XDCHECK_GE(allocSize, getMinAllocSize());
    const auto offset = allocSize * allocIdx;

#ifndef NDEBUG
    if (offset >= Slab::kSize) {
      throw std::invalid_argument(
          fmt::format("Invalid slab offset. allocSize = {}, allocIdx = {}",
                      allocSize,
                      allocIdx));
    }
#endif

    return slab->memoryAtOffset(offset);
  }

  // Prefetches the memory of a compressed pointer that may be stale: its slab
  // may have been released or given to another allocation class since. Unlike
  // unCompress(), never checks the pointer; out-of-slab offsets are skipped.
  template <typename CompressedPtrType>
  void prefetch(const CompressedPtrType& ptr,
                bool isMultiTiered) const noexcept {
    if (ptr.isNull()) {
      return;
    }
    const SlabIdx slabIndex = ptr.getSlabIdx(isMultiTiered);
    uint32_t allocSize;
    {
      // allocSize may be 0 or belong to another class; the offset check below
      // keeps the address inside the slab.
      folly::annotate_ignore_thread_sanitizer_guard g(__FILE__, __LINE__);
      allocSize = getSlabHeader(slabIndex)->getAllocSize();
    }
    const uint64_t offset = uint64_t{allocSize} * ptr.getAllocIdx();
    if (offset < Slab::kSize) {
      __builtin_prefetch(slabMemoryStart_[slabIndex].memoryAtOffset(offset));
    }
  }

  // a special implementation of pointer compression for benchmarking purposes.
  CompressedPtr4B compressAlt(const void* ptr) const;
  void* unCompressAlt(const CompressedPtr4B ptr) const;

  // returns the index of the slab from the start of the slab memory
  SlabIdx slabIdx(const Slab* const slab) const noexcept {
    if (slab == nullptr) {
      return kNullSlabIdx;
    }
    // We should never be querying for a slab that is not valid or beyond
    // nextSlabAllocation_.
    XDCHECK(slab == nextSlabAllocation_ || isValidSlab(slab));
    return static_cast<SlabIdx>(slab - slabMemoryStart_);
  }

  // returns the slab corresponding to the idx, irrespective of the validity of
  // the memory. The caller can use isValidSlab to check if the returned slab is
  // valid.
  Slab* getSlabForIdx(const SlabIdx idx) const noexcept {
    if (idx == kNullSlabIdx) {
      return nullptr;
    }
    return &slabMemoryStart_[idx];
  }

  template <typename PtrType, typename CompressedPtrType>
  PtrCompressor<PtrType, SlabAllocator, CompressedPtrType> createPtrCompressor()
      const {
    return PtrCompressor<PtrType, SlabAllocator, CompressedPtrType>(*this);
  }

  static constexpr uint32_t getMinAllocSize() noexcept {
    return static_cast<uint32_t>(1) << (Slab::kMinAllocPower);
  }

  bool isAsanPoisoningEnabled() const noexcept {
#if FOLLY_SANITIZE_ADDRESS
    return asanPoisoningEnabled_;
#else
    return false;
#endif
  }

  void asanPoisonMemoryRegion([[maybe_unused]] const volatile void* addr,
                              [[maybe_unused]] size_t size) const {
#if FOLLY_SANITIZE_ADDRESS
    if (asanPoisoningEnabled_) {
      ASAN_POISON_MEMORY_REGION(addr, size);
    }
#endif
  }

  void asanUnpoisonMemoryRegion([[maybe_unused]] const volatile void* addr,
                                [[maybe_unused]] size_t size) const {
#if FOLLY_SANITIZE_ADDRESS
    if (asanPoisoningEnabled_) {
      ASAN_UNPOISON_MEMORY_REGION(addr, size);
    }
#endif
  }

  // Returns the user-visible slab data region, excluding internal slab headers
  // at the start of the memory arena.
  std::pair<const void*, size_t> getSlabMemoryInfo() const noexcept {
    auto slabStart = reinterpret_cast<uintptr_t>(slabMemoryStart_);
    auto memStart = reinterpret_cast<uintptr_t>(memoryStart_);
    return {slabMemoryStart_, memorySize_ - (slabStart - memStart)};
  }

 private:
  // null Slab* presenttation. With 4M Slab size, a valid slab index would never
  // reach 2^16 - 1;
  static constexpr SlabIdx kNullSlabIdx = std::numeric_limits<SlabIdx>::max();

  // used for delegation from the first two types of constructors.
  SlabAllocator(void* memoryStart,
                size_t memorySize,
                bool ownsMemory,
                const Config& config);

  // intended for the constructor to ensure we are in a valid state after
  // constructing from a deserialized object.
  //
  // @throw std::invalid_argument if the state is invalid.
  void checkState() const;

  // Logs whether slab ASAN poisoning is active. No-op in non-ASAN builds.
  void logAsanPoisoningStatus() const;

  // returns first byte after the end of memory region we own.
  const Slab* getSlabMemoryEnd() const noexcept {
    return reinterpret_cast<Slab*>(reinterpret_cast<uint8_t*>(memoryStart_) +
                                   memorySize_);
  }

  // returns true if we have slabbed all the memory that is available to us.
  // false otherwise.
  bool allMemorySlabbed() const noexcept {
    return nextSlabAllocation_ == getSlabMemoryEnd();
  }

  // this is for pointer compression.
  FOLLY_ALWAYS_INLINE SlabHeader* getSlabHeader(
      unsigned int slabIndex) const noexcept {
    return reinterpret_cast<SlabHeader*>(memoryStart_) + slabIndex;
  }

  // implementation of makeNewSlab that takes care of locking, free list and
  // carving out new slabs.
  // @return  pointer to slab or nullptr if no more slabs can be allocated.
  Slab* makeNewSlabImpl();

  // Initialize the header for the given slab and pool
  void initializeHeader(Slab* slab, PoolId id);

  // allocates space from the memory we own to store the SlabHeaders for all
  // the slabs.
  static Slab* computeSlabMemoryStart(void* memoryStart, size_t memorySize);

  // exclude associated slab memory from core dump
  //
  // @throw std::system_error on any failure to advise
  void excludeMemoryFromCoredump(const PageSize& pageSize) const;

  // used by the memory locker to get pages allocated and locked into the
  // binary. With a cache size of 256GB, this will have about 60 million page
  // faults to reoslve and we want to spread that out evenly and do it
  // asynchronously.
  void lockMemoryAsync() noexcept;

  // shutsdown the memory locker if it is still running.
  void stopMemoryLocker();

  // lock serializing access to nextSlabAllocation_, freeSlabs_.
  mutable std::mutex lock_;

  // the current sizes of different memory pools from the slab allocator's
  // perspective. This is bumped up during makeNewSlab based on the poolId and
  // bumped down when the slab is released through freeSlab.
  std::array<std::atomic<size_t>, std::numeric_limits<PoolId>::max()>
      memoryPoolSize_{{}};

  // list of allocated slabs that are not in use.
  std::vector<Slab*> freeSlabs_;

  // list of allocated slabs for which memory has been madvised away
  std::vector<Slab*> advisedSlabs_;

  // start of the slab memory region aligned to slab size
  void* const memoryStart_{nullptr};

  // size of memory aligned to slab size
  const size_t memorySize_;

  // beginning of the slab memory that we actually give out to the user. This
  // is used to ensure that we dont treat slabs before this, that are used for
  // headers as valid slab.
  Slab* const slabMemoryStart_{nullptr};

  // the memory address up to which we have converted into slabs.
  Slab* nextSlabAllocation_{nullptr};

  // boolean atomic that represents whether the allocator can allocate any
  // more slabs without holding any locks.
  std::atomic<bool> canAllocate_{true};

  // whether the memory this slab allocator manages is mmaped by the caller.
  const bool ownsMemory_{true};

  // when ownsMemory_, the length of the mmap backing memoryStart_; used to
  // munmap the full region. Exceeds memorySize_ for huge-page mappings, which
  // round up to the huge-page size. Zero when the memory is caller-provided.
  size_t mmapLength_{0};

#if FOLLY_SANITIZE_ADDRESS
  // whether to poison free slab memory to detect use-after-free bugs
  const bool asanPoisoningEnabled_{false};
#endif

  // thread that does back-ground job of paging in and locking the memory if
  // enabled.
  std::thread memoryLocker_;

  // signals the locker thread to stop if we need to shutdown this instance.
  std::atomic<bool> stopLocking_{false};

  // Used by tests to avoid having to created shared memory for madvise
  // to be successful.
  bool pretendMadvise_{false};

  // amount of time to sleep in between each step to spread out the page
  // faults over a period of time.
  static constexpr unsigned int kLockSleepMS = 100;

  // number of pages to touch in eash step.
  static constexpr size_t kPagesPerStep = 10000;

  static constexpr size_t minAllocSize = static_cast<uint32_t>(1)
                                         << (Slab::kMinAllocPower);

  static_assert((Slab::kSize & (Slab::kSize - 1)) == 0,
                "Slab size is not power of two");

  // mask for all addresses belonging to slab aligned to Slab::kSize;
  static constexpr uint64_t kAddressMask =
      std::numeric_limits<uint64_t>::max() -
      (static_cast<uint64_t>(1) << Slab::kNumSlabBits) + 1;

  // Allow access to private members by unit tests
  friend class facebook::cachelib::tests::AllocTestBase;
};
} // namespace cachelib
} // namespace facebook
