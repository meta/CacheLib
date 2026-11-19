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
#include <string>
#include <utility>

#include "cachelib/allocator/memory/MemoryAllocator.h"
#include "cachelib/allocator/memory/MemoryPool.h"
#include "cachelib/allocator/memory/Slab.h"
#include "cachelib/allocator/memory/SlabAllocator.h"
#include "cachelib/common/TestUtils.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <folly/Random.h>
#pragma GCC diagnostic pop

#include <gtest/gtest.h>

namespace facebook {
namespace cachelib {

class AllocationClass;
class MemoryPool;
class MemoryPoolManager;
class MemoryAllocator;

namespace tests {

// base class for all the tests that deal with memory to be allocated.
class AllocTestBase : public testing::Test {
 public:
  // returns memory of size, aligned to Slab::kSize
  void* allocate(size_t size) {
    void* memory = nullptr;
    auto ret = posix_memalign(&memory, Slab::kSize, size);
    if (memory == nullptr || ret != 0) {
      throw std::bad_alloc();
    }
    memset(memory, 0, size);
    toFree_.push_back(memory);
    return memory;
  }

  static SlabAllocator createOwnedSlabAllocator(
      size_t size, const SlabAllocator::Config& config, void*& backing) {
    auto memory = SlabAllocator::allocateOwnedMemory(size, config.hugePageSize);
    backing = memory.get();
    return SlabAllocator(std::move(memory), size, config);
  }

  static constexpr uint32_t getMinAllocSize() noexcept {
    return static_cast<uint32_t>(1) << (Slab::kMinAllocPower);
  }

  ~AllocTestBase() override {
    for (auto m : toFree_) {
      free(m);
    }
  }

  template <typename CompressedPtrType>
  unsigned int getCompressedPtrNumAllocIdxBits() {
    return CompressedPtrType::kNumAllocIdxBits;
  }

  template <typename CompressedPtrType>
  unsigned int getCompressedPtrNumSlabIdxBits(CompressedPtrType ptr,
                                              bool isMultiTiered) {
    return ptr.numSlabIdxBits(isMultiTiered);
  }

  template <typename CompressedPtrType>
  typename CompressedPtrType::PtrType compress(CompressedPtrType ptr,
                                               uint32_t slabIdx,
                                               uint32_t allocIdx,
                                               bool isMultiTiered) {
    return ptr.compress(slabIdx, allocIdx, isMultiTiered,
                        isMultiTiered ? 1 : 0);
  }

  bool compareCompressedPtr(const CompressedPtr4B& ptr1,
                            const CompressedPtr5B& ptr2) {
    return ((ptr1.getSlabIdx(false) == ptr2.getSlabIdx(false)) &&
            (ptr1.getAllocIdx() == ptr2.getAllocIdx()));
  }

  static bool isSameSlabList(const std::vector<Slab*>& slabs1,
                             const SlabAllocator& a1,
                             const std::vector<Slab*>& slabs2,
                             const SlabAllocator& a2);

  static bool isSameSlabAllocator(const SlabAllocator& a1,
                                  const SlabAllocator& a2);

  static bool isSameAllocationClass(const AllocationClass& ac1,
                                    const AllocationClass& ac2);

  static bool isSameMemoryPool(const MemoryPool& mp1, const MemoryPool& mp2);

  static bool isSameMemoryPoolManager(const MemoryPoolManager& m1,
                                      const MemoryPoolManager& m2);

  static bool isSameMemoryAllocator(const MemoryAllocator& m1,
                                    const MemoryAllocator& m2);

  static unsigned int getSlabAllocatorLockSleepMs() noexcept {
    return SlabAllocator::kLockSleepMS;
  }

  static void waitForMemoryLocker(SlabAllocator& s) {
    if (s.memoryLocker_.joinable()) {
      s.memoryLocker_.join();
    }
  }

  void pretendMadvise(SlabAllocator* alloc) { alloc->pretendMadvise_ = true; }

  size_t getCurrentSlabAllocSize(const MemoryPool& pool) const {
    return pool.currSlabAllocSize_;
  }

  // Test helper to override a slab header's classId (preserving its allocSize)
  // through the guarded mutation API.
  static void setSlabHeaderClassId(SlabHeader* header, ClassId classId) {
    header->assignToClass(classId, header->getAllocSize());
  }

 private:
  std::vector<void*> toFree_{};
};

// Returns a random allocation size in the range [kReservedSize, Slab::kSize]
uint32_t getRandomAllocSize();

// generate n random allocation sizes that are powers of two.
std::set<uint32_t> getRandomPow2AllocSizes(unsigned int n);

// generate n random allocation sizes.
std::set<uint32_t> getRandomAllocSizes(unsigned int n,
                                       size_t minSize = Slab::kMinAllocSize);

// base class for all tests that require a slab allocator
class SlabAllocatorTestBase : public AllocTestBase {
 public:
  std::unique_ptr<SlabAllocator> createSlabAllocator(unsigned int numSlabs) {
    // 2 slabs for the headers.
    const auto size = (numSlabs + 2) * Slab::kSize;
    auto memory = allocate(size);
    auto allocator = std::unique_ptr<SlabAllocator>(new SlabAllocator(
        memory, size,
        {false /* excludeFromCoredump */, false /* lockMemory */,
         true /* enableAsanPoisoning */}));
    if (allocator == nullptr) {
      throw std::bad_alloc();
    }
    pretendMadvise(allocator.get());
    return allocator;
  }

  MemoryAllocator::Config getDefaultMemoryAllocatorConfig(
      std::set<uint32_t> allocSizes) {
    return {
        std::move(allocSizes), false /* enabledZerodSlabAllocs */,
        true /* disableFullCoreDump */, false /* lockMemory */,
        true /* enableAsanPoisoning */
    };
  }

  std::unique_ptr<MemoryAllocator> createFilledMemoryAllocator() {
    const unsigned int numClasses = 10;
    const unsigned int numPools = 4;
    // create enough memory for 4 pools with 5 allocation classes each and 5
    // slabs each for each allocation class.
    const size_t poolSize = numClasses * 5 * Slab::kSize;
    // allocate enough memory for all the pools plus slab headers.
    const size_t totalSize = numPools * poolSize + 2 * Slab::kSize;
    void* memory = allocate(totalSize);
    auto m = std::unique_ptr<MemoryAllocator>(
        new MemoryAllocator(getDefaultMemoryAllocatorConfig(getRandomAllocSizes(
                                numClasses, getMinAllocSize())),
                            memory,
                            totalSize));

    std::unordered_map<PoolId, const std::set<uint32_t>> pools;
    for (unsigned int i = 0; i < numPools; i++) {
      auto nClasses = folly::Random::rand32() % numClasses + 1;
      auto sizes = getRandomAllocSizes(nClasses, getMinAllocSize());
      auto pid = m->addPool(getRandomStr(), poolSize, sizes);
      pools.insert({pid, sizes});
    }

    using PoolAllocs = std::unordered_map<PoolId, std::vector<void*>>;
    PoolAllocs poolAllocs;
    auto makeAllocsOutOfPool = [&m, &poolAllocs, &pools](PoolId pid) {
      const auto& sizes = pools[pid];
      std::vector<void*> allocs;
      unsigned int numAllocations = 0;
      do {
        uint32_t prev = getMinAllocSize();
        numAllocations = 0;
        for (const auto size : sizes) {
          const auto range = prev == size ? 1 : size - prev;
          uint32_t randomSize = folly::Random::rand32() % range + prev;
          void* alloc = m->allocate(pid, randomSize);
          if (alloc != nullptr) {
            allocs.push_back(alloc);
            numAllocations++;
          }
          prev = size + 1;
        }
      } while (numAllocations > 0);
      poolAllocs[pid] = allocs;
    };
    for (const auto& pool : pools) {
      makeAllocsOutOfPool(pool.first);
    }
    return m;
  }

  static std::string getRandomStr() {
    unsigned int len = folly::Random::rand32() % 40 + 10;
    return facebook::cachelib::test_util::getRandomAsciiStr(len);
  }
};

} // namespace tests
} // namespace cachelib
} // namespace facebook
