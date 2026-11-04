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

#include <folly/Random.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include "cachelib/allocator/memory/AllocationClass.h"
#include "cachelib/allocator/memory/MemoryPool.h"
#include "cachelib/allocator/memory/Slab.h"
#include "cachelib/allocator/memory/SlabAllocator.h"
#include "cachelib/allocator/memory/tests/TestBase.h"
#include "cachelib/common/Serialization.h"

using namespace facebook::cachelib::tests;
using namespace facebook::cachelib;

using MemoryPoolTest = SlabAllocatorTestBase;

constexpr size_t SerializationBufferSize = 100 * 1024;

TEST_F(MemoryPoolTest, Create) {
  auto slabAlloc = createSlabAllocator(10);
  auto usable = slabAlloc->getNumUsableSlabs();
  size_t poolSize = usable * Slab::kSize;

  // random alloc sizes.
  auto allocSizes = getRandomAllocSizes(10);
  PoolId id = 5;

  ASSERT_NO_THROW(MemoryPool(id, poolSize, *slabAlloc, allocSizes));

  // empty allocSizes must throw
  std::set<uint32_t> invalidAllocSizes;
  ASSERT_THROW(MemoryPool(id, poolSize, *slabAlloc, invalidAllocSizes),
               std::invalid_argument);

  // allocSize with any size more than Slab::kSize must throw as well.
  invalidAllocSizes = allocSizes;
  invalidAllocSizes.insert(Slab::kSize + getRandomAllocSize());

  ASSERT_THROW(MemoryPool(id, poolSize, *slabAlloc, invalidAllocSizes),
               std::invalid_argument);
}

// allocate memory across the entire size ranges and ensure that the
// allocations succeed.
TEST_F(MemoryPoolTest, Alloc) {
  auto slabAlloc = createSlabAllocator(100);
  auto usable = slabAlloc->getNumUsableSlabs();
  size_t poolSize = usable * Slab::kSize;

  PoolId id = 5;
  auto allocSizes = getRandomAllocSizes(10);

  MemoryPool mp(id, poolSize, *slabAlloc, allocSizes);
  ASSERT_EQ(id, mp.getId());
  ASSERT_EQ(mp.getCurrentAllocSize(), 0);

  uint32_t prevLow = 1;
  const int ntries = 5;
  for (const auto classSize : allocSizes) {
    // make couple of allocations from each class
    const auto range = prevLow == classSize ? 1 : classSize - prevLow;
    for (int i = 0; i < ntries; i++) {
      // pick a random size in this slab class.
      const auto size = prevLow + (folly::Random::rand32() % range);
      auto before = mp.getCurrentAllocSize();
      void* memory = mp.allocate(size);
      auto after = mp.getCurrentAllocSize();
      ASSERT_NE(memory, nullptr);
      ASSERT_LE(size, classSize);
      ASSERT_EQ(before + classSize, after);
      auto header = slabAlloc->getSlabHeader(memory);
      ASSERT_EQ(header->getAllocSize(), classSize);
    }
    prevLow = classSize + 1;
  }
}

TEST_F(MemoryPoolTest, AllocClasss) {
  auto slabAlloc = createSlabAllocator(100);
  auto usable = slabAlloc->getNumUsableSlabs();
  unsigned int remaining = 10;
  ASSERT_GT(usable, remaining);
  ASSERT_GT(usable - remaining, 0);
  size_t poolSize = (usable - 10) * Slab::kSize;

  PoolId id = 5;
  auto allocSizes = getRandomAllocSizes(30);

  MemoryPool mp(id, poolSize, *slabAlloc, allocSizes);
  ASSERT_GT(mp.getNumClassId(), 0);
  ASSERT_EQ(mp.getNumClassId(), allocSizes.size());

  uint32_t prevLow = 1;
  for (const auto allocSize : allocSizes) {
    const auto range = prevLow == allocSize ? 1 : allocSize - prevLow;
    const auto size = prevLow + (folly::Random::rand32() % range);
    auto classId = mp.getAllocationClassId(size);
    ASSERT_GE(classId, 0);
    ASSERT_LT(classId, mp.getNumClassId());
    void* memory = mp.allocate(size);
    ASSERT_NE(memory, nullptr);
    ASSERT_EQ(mp.getAllocationClassId(memory), classId);
    prevLow = allocSize + 1;
  }

  // alloc size more than the maximum configured should not be allowed.
  ASSERT_THROW(mp.getAllocationClassId(*allocSizes.rbegin() + 1),
               std::invalid_argument);
  uint32_t size = 0;
  ASSERT_THROW(mp.getAllocationClassId(size), std::invalid_argument);
  ASSERT_THROW(mp.getAllocationClassId(allocate(100)), std::invalid_argument);

  auto allocSizes2 = getRandomAllocSizes(10);
  MemoryPool mp2(3, remaining * Slab::kSize, *slabAlloc, allocSizes2);
  void* memory = mp2.allocate(*allocSizes2.begin());

  // trying to get the allocation class id of the first pool using the second
  // should throw.
  ASSERT_THROW(mp.getAllocationClassId(memory), std::invalid_argument);
}

using Allocs = std::vector<void*>;
using AllocInfoT = std::unordered_map<uint32_t, Allocs>;

// given two memory pools that are full, try to free some allocations from one
// pool and still ensure that other is full across all alloc classes.
void checkFreeing(MemoryPool& mp1,
                  MemoryPool& mp2,
                  AllocInfoT& allocs1,
                  const std::set<uint32_t>& allocSizes) {
  // both mp1 and mp2 are full. free some allocations from mp1 and try to
  // allocate in mp2.
  const unsigned long maxToFree = 20;
  std::unordered_map<uint32_t, int> nFreed;
  for (auto size : allocSizes) {
    auto toFree = std::min(maxToFree, allocs1[size].size());
    for (size_t i = 0; i < toFree; i++) {
      auto memory = allocs1[size].back();
      allocs1[size].pop_back();
      ASSERT_NE(memory, nullptr);
      ASSERT_NO_THROW(mp1.free(memory));
      nFreed[size]++;
    }
  }

  // should not be able to allocate from mp2
  for (auto size : allocSizes) {
    ASSERT_EQ(mp2.allocate(size), nullptr);
  }

  // should be able to allocate from mp1.
  for (auto size : allocSizes) {
    while (nFreed[size]-- > 0) {
      void* memory = mp1.allocate(size);
      ASSERT_NE(memory, nullptr);
      allocs1[size].push_back(memory);
    }
  }

  // doing any more allocations from mp1 should fail.
  for (auto size : allocSizes) {
    ASSERT_EQ(mp1.allocate(size), nullptr);
  }
}

// fills up the memory pool and returns the allocations in the outparam
void fillUpMemoryPool(MemoryPool& mp,
                      const std::set<uint32_t>& allocSizes,
                      AllocInfoT& poolAllocs) {
  for (const auto size : allocSizes) {
    // ensure it is power of two.
    ASSERT_EQ(size & (size - 1), 0);
  }

  const auto maxSize = mp.getPoolSize();
  using ClassFullT = std::unordered_map<uint32_t, bool>;
  ClassFullT isClassFull;

  // pick a random class and allocate from that until the memory is full. If
  // an allocation class fails allocation, it should not succeed any later.
  while (mp.getCurrentAllocSize() < maxSize) {
    const unsigned int randomClass =
        folly::Random::rand32() % allocSizes.size();
    const auto classSize = *(std::next(allocSizes.begin(), randomClass));
    void* memory = mp.allocate(classSize);
    if (memory != nullptr) {
      poolAllocs[classSize].push_back(memory);
      const auto it = isClassFull.find(classSize);
      if (it != isClassFull.end()) {
        ASSERT_FALSE(isClassFull[classSize]);
      }
    } else {
      const auto it = isClassFull.find(classSize);
      if (it == isClassFull.end()) {
        isClassFull[classSize] = true;
      } else {
        ASSERT_TRUE(isClassFull[classSize]);
      }
    }
  }
}

TEST_F(MemoryPoolTest, Free) {
  auto slabAlloc = createSlabAllocator(35);
  auto usable = slabAlloc->getNumUsableSlabs();

  PoolId id1 = 5;
  PoolId id2 = 6;
  // use powers of two to ensure that we can fill up memory pool
  // deterministically.
  auto allocSizes = getRandomAllocSizes(10);
  size_t poolSize1 = usable / 2 * Slab::kSize;
  size_t poolSize2 = (usable - usable / 2) * Slab::kSize;

  MemoryPool mp1(id1, poolSize1, *slabAlloc, allocSizes);
  MemoryPool mp2(id2, poolSize2, *slabAlloc, allocSizes);

  // make some allocations to memory pools and try to ensure that the
  // getAllocSize respects freeing.
  const int nallocs = 10;
  Allocs allocs1;
  auto before = mp1.getCurrentAllocSize();
  size_t sumSize = 0;
  for (int i = 0; i < nallocs; i++) {
    const uint32_t randomSize =
        folly::Random::rand32() % *allocSizes.rbegin() + 1;
    void* memory = mp1.allocate(randomSize);
    auto header = slabAlloc->getSlabHeader(memory);
    sumSize += header->getAllocSize();
    ASSERT_NE(memory, nullptr);
    ASSERT_NE(mp2.allocate(randomSize), nullptr);
    allocs1.push_back(memory);
  }

  ASSERT_EQ(allocs1.size(), nallocs);

  auto after = mp1.getCurrentAllocSize();
  ASSERT_EQ(sumSize, after - before);

  // try to free some of the allocations from pool1 into pool2 and this should
  // throw.
  for (int i = 0; i < nallocs; i++) {
    ASSERT_THROW(mp2.free(allocs1[i]), std::invalid_argument);
  }

  // trying to free some random memory not belonging to any of the pools
  // should also throw.
  void* m = allocate(100);
  ASSERT_THROW(mp1.free(m), std::invalid_argument);
  ASSERT_THROW(mp2.free(m), std::invalid_argument);

  // freeing back all the allocations to the memory pool should restore its
  // alloc size.
  ASSERT_GT(after, before);
  for (int i = 0; i < nallocs; i++) {
    ASSERT_NO_THROW(mp1.free(allocs1[i]));
  }
  ASSERT_EQ(before, mp1.getCurrentAllocSize());
}

TEST_F(MemoryPoolTest, Resize) {
  auto slabAlloc = createSlabAllocator(50);
  auto usable = slabAlloc->getNumUsableSlabs();

  PoolId id1 = 5;
  // use powers of two to ensure that we can fill up memory pool
  // deterministically.
  auto allocSizes = getRandomPow2AllocSizes(10);
  size_t poolSize1 = usable * Slab::kSize;

  MemoryPool mp1(id1, poolSize1, *slabAlloc, allocSizes);

  // allocations belonging to pool1 grouped by their allocation class.
  AllocInfoT poolAllocs1;
  fillUpMemoryPool(mp1, allocSizes, poolAllocs1);

  ASSERT_EQ(usable, mp1.getStats().allocatedSlabs());
  ASSERT_EQ(0, mp1.getStats().slabsUnAllocated);

  const auto prevCurrentAllocSize = mp1.getCurrentAllocSize();
  const auto prevSlabAllocSize = getCurrentSlabAllocSize(mp1);
  // size down the pool.
  const auto newSize = usable / 2 * Slab::kSize;
  mp1.resize(newSize);
  ASSERT_EQ(newSize, mp1.getPoolSize());
  ASSERT_EQ(prevCurrentAllocSize, mp1.getCurrentAllocSize());
  ASSERT_EQ(prevSlabAllocSize, getCurrentSlabAllocSize(mp1));
  // the stats should still show the actual number of allocated slabs
  ASSERT_EQ(usable, mp1.getStats().allocatedSlabs());
  // but we should not be able to allocate any more slabs since we are above
  // limit.
  ASSERT_EQ(0, mp1.getStats().slabsUnAllocated);

  auto releaseMemory = [&]() {
    // try to release one slab from every allocation class belonging to the
    // memory pool.
    unsigned int numReleased = 0;
    for (ClassId id = 0; static_cast<size_t>(id) < mp1.getNumClassId(); id++) {
      if (getCurrentSlabAllocSize(mp1) <= mp1.getPoolSize()) {
        break;
      }
      try {
        auto context = mp1.startSlabRelease(id, Slab::kInvalidClassId,
                                            SlabReleaseMode::kResize, nullptr,
                                            false /* enableZeroedSlabAllocs */);
        for (auto alloc : context.getActiveAllocations()) {
          mp1.free(alloc);
        }
        mp1.completeSlabRelease(std::move(context));
        ++numReleased;
      } catch (const std::invalid_argument&) {
        // this class id can not free up anymore.
        continue;
      }
    }
    return numReleased;
  };

  // actually release the memory from the pool to the slab allocator.
  unsigned int numReleased = 0;
  do {
    const auto prev = mp1.getStats();
    numReleased = releaseMemory();
    const auto after = mp1.getStats();
    ASSERT_EQ(prev.allocatedSlabs() - numReleased, after.allocatedSlabs());
  } while (numReleased > 0);

  ASSERT_LE(mp1.getCurrentAllocSize(), mp1.getPoolSize());

  mp1.resize(usable * Slab::kSize);
  ASSERT_EQ(usable * Slab::kSize, mp1.getPoolSize());
  ASSERT_LT(getCurrentSlabAllocSize(mp1), mp1.getPoolSize());
  ASSERT_EQ(getCurrentSlabAllocSize(mp1), newSize);
  ASSERT_EQ(usable - usable / 2, mp1.getStats().slabsUnAllocated);
}

TEST_F(MemoryPoolTest, MultiplePools) {
  auto slabAlloc = createSlabAllocator(50);
  auto usable = slabAlloc->getNumUsableSlabs();

  PoolId id1 = 5;
  PoolId id2 = 6;
  // use powers of two to ensure that we can fill up memory pool
  // deterministically.
  auto allocSizes = getRandomPow2AllocSizes(10);
  size_t poolSize1 = usable / 2 * Slab::kSize;
  size_t poolSize2 = (usable - usable / 2) * Slab::kSize;

  MemoryPool mp1(id1, poolSize1, *slabAlloc, allocSizes);
  MemoryPool mp2(id2, poolSize2, *slabAlloc, allocSizes);

  // allocations belonging to pool1 grouped by their allocation class.
  AllocInfoT poolAllocs1;
  fillUpMemoryPool(mp1, allocSizes, poolAllocs1);

  // now that we have filled up memory pool 1, we should still be able to make
  // allocations to memory pool2 until it become full.
  auto before = mp2.getCurrentAllocSize();
  AllocInfoT poolAllocs2;
  fillUpMemoryPool(mp2, allocSizes, poolAllocs2);
  auto after = mp2.getCurrentAllocSize();
  ASSERT_GT(after, before);

  // free from mp1 and try to allocate from mp2. that should fail. however,
  // allocating from mp1 should succeed. This function returns mp1 and mp2 to
  // be full.
  auto tmpPool1 = poolAllocs1;
  checkFreeing(mp1, mp2, poolAllocs1, allocSizes);
  ASSERT_EQ(tmpPool1, poolAllocs1);
  auto tmpPool2 = poolAllocs2;
  checkFreeing(mp2, mp1, poolAllocs2, allocSizes);
  ASSERT_EQ(tmpPool2, poolAllocs2);
}

// ensure that once the pool has reached its size, no more memory allocations
// are possible until memory is freed.
TEST_F(MemoryPoolTest, MemoryFull) {
  auto slabAlloc = createSlabAllocator(20);
  auto usable = slabAlloc->getNumUsableSlabs();
  // these are slabs outside the pool.
  unsigned int nFreeSlabs = 5;
  unsigned int nPoolSlabs = usable - nFreeSlabs;
  size_t poolSize = nPoolSlabs * Slab::kSize;

  PoolId id = 5;

  // random alloc sizes with powers of two to make testing more exhaustive
  auto allocSizes = getRandomPow2AllocSizes(10);
  MemoryPool mp(id, poolSize, *slabAlloc, allocSizes);

  // ensure that all the memory is allocated. since allocSizes are powers of
  // two, this should terminate.
  while (mp.getCurrentAllocSize() < poolSize) {
    auto randomSize = (folly::Random::rand32() % *allocSizes.rbegin()) + 1;
    // some of these might fail depending on the availability of slabs
    mp.allocate(randomSize);
  }

  // once the pool size is reached, no allocations should be possible for any
  // size.
  ASSERT_EQ(mp.getCurrentAllocSize(), poolSize);
  for (auto size = *allocSizes.begin(); size <= *allocSizes.rbegin(); size++) {
    ASSERT_EQ(mp.allocate(size), nullptr);
  }

  // the pool must not be able to allocate any more slabs.
  ASSERT_EQ(0, mp.getStats().slabsUnAllocated);
  ASSERT_EQ(nPoolSlabs, mp.getStats().allocatedSlabs());

  // we should be able to use the remaining nFreeSlabs.
  PoolId newId = 9;
  while (nFreeSlabs-- > 0) {
    ASSERT_NE(slabAlloc->makeNewSlab(newId), nullptr);
  }
}

TEST_F(MemoryPoolTest, Serialization) {
  auto slabAlloc = createSlabAllocator(20);
  auto usable = slabAlloc->getNumUsableSlabs();
  size_t poolSize = usable * Slab::kSize;

  PoolId id = 5;

  // random alloc sizes with powers of two to make testing more exhaustive
  auto allocSizes = getRandomPow2AllocSizes(10);
  MemoryPool mp(id, poolSize, *slabAlloc, allocSizes);

  // ensure that all the memory is allocated. since allocSizes are powers of
  // two, this should terminate.
  std::vector<void*> allocatedMem;
  while (mp.getCurrentAllocSize() < poolSize) {
    uint32_t min = 1;
    for (auto allocSize : allocSizes) {
      const auto diff = allocSize - min;
      const auto randomSize =
          diff > 0 ? folly::Random::rand32() % diff + min : allocSize;
      // some of these might fail depending on the availability of slabs
      void* addr = mp.allocate(randomSize);
      if (addr) {
        allocatedMem.push_back(addr);
      }
      min = allocSize + 1;
    }
  }
  ASSERT_NE(allocatedMem.size(), 0);

  // once the pool size is reached, no allocations should be possible for any
  // size.
  ASSERT_EQ(mp.getCurrentAllocSize(), poolSize);
  for (auto size = *allocSizes.begin(); size <= *allocSizes.rbegin(); size++) {
    ASSERT_EQ(mp.allocate(size), nullptr);
  }

  // Construct an identical memory pool object
  uint8_t buffer[SerializationBufferSize];
  uint8_t* begin = buffer;
  uint8_t* end = buffer + SerializationBufferSize;
  Serializer serializer(begin, end);
  serializer.serialize(mp.saveState());
  Deserializer deserializer(begin, end);
  MemoryPool mp2(deserializer.deserialize<serialization::MemoryPoolObject>(),
                 *slabAlloc);
  ASSERT_TRUE(isSameMemoryPool(mp, mp2));

  // Free all allocations
  for (auto addr : allocatedMem) {
    mp2.free(addr);
  }
  allocatedMem.clear();

  // Allocate more memory again
  ASSERT_EQ(allocatedMem.size(), 0);
  ASSERT_EQ(mp2.getCurrentAllocSize(), 0);
  while (mp2.getCurrentAllocSize() < poolSize) {
    size_t min = 1;
    for (auto allocSize : allocSizes) {
      const auto diff = allocSize - min;
      const auto randomSize =
          diff > 0 ? folly::Random::rand32() % diff + min : allocSize;
      // some of these might fail depending on the availability of slabs
      void* addr = mp2.allocate(randomSize);
      if (addr) {
        allocatedMem.push_back(addr);
      }
      min = allocSize + 1;
    }
  }
  ASSERT_NE(allocatedMem.size(), 0);

  // once the pool size is reached, no allocations should be possible for any
  // size.
  ASSERT_EQ(mp2.getCurrentAllocSize(), poolSize);
  for (auto size = *allocSizes.begin(); size <= *allocSizes.rbegin(); size++) {
    ASSERT_EQ(mp2.allocate(size), nullptr);
  }
}

TEST_F(MemoryPoolTest, InvalidDeSerialization) {
  auto slabAlloc = createSlabAllocator(20);
  auto usable = slabAlloc->getNumUsableSlabs();
  size_t poolSize = usable * Slab::kSize;

  PoolId id = 5;

  // random alloc sizes with powers of two to make testing more exhaustive
  auto allocSizes = getRandomPow2AllocSizes(10);
  MemoryPool mp(id, poolSize, *slabAlloc, allocSizes);

  // ensure that all the memory is allocated. since allocSizes are powers of
  // two, this should terminate.
  std::vector<void*> allocatedMem;
  while (mp.getCurrentAllocSize() < poolSize) {
    uint32_t min = 1;
    for (auto allocSize : allocSizes) {
      const auto diff = allocSize - min;
      const auto randomSize =
          diff > 0 ? folly::Random::rand32() % diff + min : allocSize;
      // some of these might fail depending on the availability of slabs
      void* addr = mp.allocate(randomSize);
      if (addr) {
        allocatedMem.push_back(addr);
      }
      min = allocSize + 1;
    }
  }
  ASSERT_NE(allocatedMem.size(), 0);

  // Construct an identical memory pool object
  uint8_t buffer[SerializationBufferSize];
  uint8_t* begin = buffer;
  uint8_t* end = buffer + SerializationBufferSize;
  Serializer serializer(begin, end);
  serializer.serialize(mp.saveState());

  Deserializer deserializer(begin, end);
  const auto correctState =
      deserializer.deserialize<serialization::MemoryPoolObject>();
  MemoryPool mp2(correctState, *slabAlloc);
  ASSERT_TRUE(isSameMemoryPool(mp, mp2));

  // check with invalid id
  auto incorrectIdState = correctState;
  *incorrectIdState.id() = -5;

  ASSERT_THROW(MemoryPool(incorrectIdState, *slabAlloc), std::invalid_argument);

  // check with invalid currSlabAllocSize and invalid currAllocSize
  auto incorrectAllocSizeState = correctState;
  *incorrectAllocSizeState.currAllocSize() =
      *correctState.currSlabAllocSize() +
      folly::Random::rand32() % Slab::kSize + 1;

  ASSERT_THROW(MemoryPool(incorrectAllocSizeState, *slabAlloc),
               std::invalid_argument);

  auto incorrectSlabAllocSizeState = correctState;
  *incorrectSlabAllocSizeState.currSlabAllocSize() =
      *correctState.currAllocSize() - (folly::Random::rand32() % Slab::kSize) -
      1;

  ASSERT_GT(*incorrectSlabAllocSizeState.currAllocSize(),
            *incorrectSlabAllocSizeState.currSlabAllocSize());
  ASSERT_THROW(MemoryPool(incorrectSlabAllocSizeState, *slabAlloc),
               std::invalid_argument);

  // mismatching number of allocation sizes and serialized alloc objects.
  auto incorrectSizesState = correctState;
  incorrectSizesState.acSizes()->push_back(folly::Random::rand32());

  ASSERT_THROW(MemoryPool(incorrectSizesState, *slabAlloc),
               std::invalid_argument);

  incorrectSizesState = correctState;
  std::vector<uint32_t> sizes;
  for (auto size : *incorrectSizesState.acSizes()) {
    sizes.push_back(size);
  }

  std::mt19937 g(folly::Random::rand32());
  std::shuffle(sizes.begin(), sizes.end(), g);

  for (size_t i = 0; i < sizes.size(); i++) {
    incorrectSizesState.acSizes()[i] = sizes[i];
  }

  ASSERT_THROW(MemoryPool(incorrectSizesState, *slabAlloc),
               std::invalid_argument);

  incorrectSizesState = correctState;
  incorrectSizesState.acSizes()->push_back(
      incorrectSizesState.acSizes()->back());

  ASSERT_THROW(MemoryPool(incorrectSizesState, *slabAlloc),
               std::invalid_argument);

  auto invalidAcObjectState = correctState;
  const auto idx = folly::Random::rand32() % invalidAcObjectState.ac()->size();
  auto& obj = invalidAcObjectState.ac()[idx];
  *obj.allocationSize() = folly::Random::rand32() % Slab::kSize + 1;

  ASSERT_THROW(MemoryPool(invalidAcObjectState, *slabAlloc),
               std::invalid_argument);
}

TEST_F(MemoryPoolTest, SlabRelease) {
  auto slabAlloc = createSlabAllocator(20);
  auto usable = slabAlloc->getNumUsableSlabs();
  size_t poolSize = usable * Slab::kSize;

  PoolId poolId = 5;

  // random alloc sizes with powers of two to make testing more exhaustive
  auto allocSizes = getRandomPow2AllocSizes(10);
  MemoryPool mp(poolId, poolSize, *slabAlloc, allocSizes);

  // ensure that all the memory is allocated. since allocSizes are powers of
  // two, this should terminate.
  std::unordered_map<ClassId, std::set<void*>> allocsByClass;
  auto fillUpPoolWithAllocs = [&]() {
    while (mp.getCurrentAllocSize() < poolSize) {
      uint32_t min = 1;
      for (auto allocSize : allocSizes) {
        const auto diff = allocSize - min;
        const auto randomSize =
            diff > 0 ? folly::Random::rand32() % diff + min : allocSize;
        // some of these might fail depending on the availability of slabs
        void* addr = mp.allocate(randomSize);
        if (addr) {
          auto classId = mp.getAllocationClassId(addr);
          allocsByClass[classId].insert(addr);
        }
        min = allocSize + 1;
      }
    }
  };

  auto testSlabReleaseInMode = [&](SlabReleaseMode mode) {
    // with all the memory allocated, release a slab and we should be able to
    // make more allocations.
    auto numUsableSlabs = slabAlloc->getNumUsableSlabs();
    for (ClassId id = 0; static_cast<size_t>(id) < mp.getNumClassId(); id++) {
      auto prevStat = mp.getStats();
      auto context =
          mp.startSlabRelease(id, Slab::kInvalidClassId, mode, nullptr,
                              false /* enableZeroedSlabAllocs */);
      ASSERT_FALSE(context.isReleased());
      ASSERT_LE(context.getActiveAllocations().size(),
                allocsByClass[id].size());

      // the allocs returned should match the allocs from the class.
      for (auto alloc : context.getActiveAllocations()) {
        ASSERT_NE(allocsByClass[id].find(alloc), allocsByClass[id].end());
        // remove
        mp.free(alloc);
        allocsByClass[id].erase(alloc);
      }
      const Slab* slab = context.getSlab();
      auto header = slabAlloc->getSlabHeader(slab);

      // ensure that all allocs are from the same slab.
      for (auto alloc : context.getActiveAllocations()) {
        ASSERT_EQ(slabAlloc->getSlabForMemory(alloc), slab);
      }

      ASSERT_EQ(id, header->getClassId());
      ASSERT_EQ(id, context.getClassId());
      ASSERT_EQ(mp.getId(), context.getPoolId());

      auto slabAllocSize = getCurrentSlabAllocSize(mp);

      // complete the slab release. depending on the mode, it will either
      // get released back to the slab allocator or stay within the pool
      mp.completeSlabRelease(std::move(context));

      // slab header must reflect the fact that the slab does not belong to
      // the allocation class , but belongs to the pool.
      ASSERT_EQ(Slab::kInvalidClassId, header->getClassId());
      ASSERT_EQ(0, header->getAllocSize());
      auto stat = mp.getStats();
      switch (mode) {
      case SlabReleaseMode::kResize:
        ASSERT_EQ(Slab::kInvalidPoolId, header->getPoolId());
        // resize should not have added the slab within the pool.
        ASSERT_EQ(prevStat.freeSlabs, stat.freeSlabs);
        ASSERT_EQ(prevStat.allocatedSlabs() - 1, stat.allocatedSlabs());
        // When resizing since we gave away one slab, we should be able to
        // count that as unallocated slab.
        ASSERT_EQ(prevStat.slabsUnAllocated + 1, stat.slabsUnAllocated);
        ASSERT_EQ(prevStat.numSlabResize + 1, stat.numSlabResize);
        break;
      case SlabReleaseMode::kAdvise:
        ASSERT_EQ(Slab::kInvalidPoolId, header->getPoolId());
        // advise should not have added the slab within the pool.
        ASSERT_EQ(prevStat.freeSlabs, stat.freeSlabs);
        ASSERT_EQ(prevStat.allocatedSlabs() - 1, stat.allocatedSlabs());
        // since we advised away one slab, the number of unallocated slabs
        // do not increase but the number of advised slabs does.
        ASSERT_EQ(prevStat.slabsUnAllocated, stat.slabsUnAllocated);
        ASSERT_EQ(prevStat.numSlabAdvise + 1, stat.numSlabAdvise);
        break;
      case SlabReleaseMode::kRebalance:
        ASSERT_EQ(mp.getId(), header->getPoolId());
        ASSERT_EQ(prevStat.freeSlabs + 1, stat.freeSlabs);
        ASSERT_EQ(prevStat.allocatedSlabs(), stat.allocatedSlabs());
        ASSERT_EQ(prevStat.slabsUnAllocated, stat.slabsUnAllocated);
        ASSERT_EQ(prevStat.numSlabRebalance + 1, stat.numSlabRebalance);
        break;
      }
      ASSERT_EQ(slabAllocSize - Slab::kSize, getCurrentSlabAllocSize(mp));
    }

    // if this was for a resize scenario, the released slab will go back to
    // the allocator, if not, it will go back to the pool. check that.
    switch (mode) {
    case SlabReleaseMode::kResize:
      ASSERT_FALSE(slabAlloc->allSlabsAllocated());
      break;
    case SlabReleaseMode::kRebalance:
      ASSERT_TRUE(slabAlloc->allSlabsAllocated());
      break;
    case SlabReleaseMode::kAdvise:
      auto stat = mp.getStats();
      ASSERT_TRUE(slabAlloc->allSlabsAllocated());
      // Verify that total cache memory size is reduced by the advised away size
      ASSERT_EQ(numUsableSlabs - stat.numSlabAdvise,
                slabAlloc->getNumUsableSlabs());
      // The number of advised away slabs equals number of reclaimable ones.
      ASSERT_EQ(stat.numSlabAdvise, slabAlloc->numSlabsReclaimable());
      break;
    }
  };

  fillUpPoolWithAllocs();
  ASSERT_TRUE(mp.allSlabsAllocated());
  // do resize. this should release the slabs to the slab allocator.
  testSlabReleaseInMode(SlabReleaseMode::kResize);
  ASSERT_FALSE(mp.allSlabsAllocated());

  fillUpPoolWithAllocs();
  ASSERT_TRUE(mp.allSlabsAllocated());

  // advising away should release slabs to slab allocator which promptly get
  // advised away leaving no free slabs for allocation.
  auto prePoolSize = mp.getPoolSize();
  auto preUsedPoolSize = getCurrentSlabAllocSize(mp);
  ASSERT_EQ(mp.getPoolSize(), mp.getPoolUsableSize());
  testSlabReleaseInMode(SlabReleaseMode::kAdvise);
  ASSERT_TRUE(mp.allSlabsAllocated());
  auto advisedAwaySize = mp.getStats().numSlabAdvise * Slab::kSize;
  // The pool size reduces by number of advised away slabs
  ASSERT_EQ(prePoolSize - advisedAwaySize, mp.getPoolUsableSize());
  ASSERT_EQ(advisedAwaySize, mp.getPoolAdvisedSize());
  // The used pool size also reduces by number of advised away slabs
  ASSERT_EQ(preUsedPoolSize - advisedAwaySize, getCurrentSlabAllocSize(mp));

  // Reclaim all the memory
  {
    auto stat = mp.getStats();
    // Reclaim all the slabs that were advised away.
    mp.reclaimSlabsAndGrow(stat.numSlabAdvise);
    // Since we advised away slabs and not freed them, we don't have any
    // unallocated slabs.
    ASSERT_FALSE(mp.allSlabsAllocated());
    // The pool size is back to before advising away
    ASSERT_EQ(prePoolSize, mp.getPoolSize());
    ASSERT_EQ(prePoolSize, mp.getPoolUsableSize());
    ASSERT_EQ(0, mp.getPoolAdvisedSize());
    // Since the reclaimed slabs go to free pool, the used size doesn't change.
    ASSERT_EQ(preUsedPoolSize - advisedAwaySize, getCurrentSlabAllocSize(mp));
  }

  fillUpPoolWithAllocs();
  ASSERT_TRUE(mp.allSlabsAllocated());
  // The reclaimed slabs have been used up.
  ASSERT_EQ(preUsedPoolSize, getCurrentSlabAllocSize(mp));

  // do rebalance. this should not release the slabs to the slab allocator.
  testSlabReleaseInMode(SlabReleaseMode::kRebalance);
  ASSERT_FALSE(mp.allSlabsAllocated());

  auto stat = mp.getStats();
  ASSERT_GT(stat.numSlabResize, 0);
  ASSERT_GT(stat.numSlabRebalance, 0);

  uint8_t buffer[SerializationBufferSize];
  uint8_t* begin = buffer;
  uint8_t* end = buffer + SerializationBufferSize;
  Serializer serializer(begin, end);
  serializer.serialize(mp.saveState());

  Deserializer deserializer(begin, end);
  const auto state =
      deserializer.deserialize<serialization::MemoryPoolObject>();
  MemoryPool newMp(state, *slabAlloc);
  auto newStats = newMp.getStats();
  ASSERT_EQ(newStats.numSlabResize, stat.numSlabResize);
  ASSERT_EQ(newStats.numSlabRebalance, stat.numSlabRebalance);
}

// when using victim classId  as kInvalidClassId under resize mode, we should
// be able to move from the pool's free slab list.
TEST_F(MemoryPoolTest, ReleaseSlabFromFreeSlabs) {
  auto slabAlloc = createSlabAllocator(20);
  auto usable = slabAlloc->getNumUsableSlabs();
  size_t poolSize = usable * Slab::kSize;

  PoolId poolId = 5;

  // just one allocation size
  uint32_t allocSize = 1024;
  std::set<uint32_t> allocSizes = {allocSize};
  MemoryPool mp(poolId, poolSize, *slabAlloc, allocSizes);

  std::vector<void*> allocs;
  for (int i = 0; i < 10; i++) {
    allocs.push_back(mp.allocate(allocSize));
  }

  ASSERT_EQ(mp.getStats().freeSlabs, 0);
  ASSERT_EQ(mp.getStats().allocatedSlabs(), 1);
  // trying to release from the pool without victim should fail since the free
  // slabs are empty
  ASSERT_THROW(mp.startSlabRelease(Slab::kInvalidClassId,
                                   Slab::kInvalidClassId,
                                   SlabReleaseMode::kResize,
                                   nullptr,
                                   false),
               std::invalid_argument);

  // use rebalance mode with no receiver to move the slab to the free slab
  // with the pool
  {
    auto classId = mp.getAllocationClassId(allocs.back());
    auto context =
        mp.startSlabRelease(classId, Slab::kInvalidClassId,
                            SlabReleaseMode::kRebalance, allocs.back(), false);

    for (auto alloc : allocs) {
      mp.free(alloc);
    }
    mp.completeSlabRelease(context);
  }

  // since the receiver in the above release was kInvalidClassId, the slabs
  // would be in the pool's free list. trying to release them with resize mode
  // should send it back to the slab allocator.
  ASSERT_EQ(mp.getStats().freeSlabs, 1);

  // try releaseing under the resize mode with invalid class id and it should
  // work.
  auto context = mp.startSlabRelease(Slab::kInvalidClassId,
                                     Slab::kInvalidClassId,
                                     SlabReleaseMode::kResize,
                                     nullptr,
                                     false);
  ASSERT_TRUE(context.isReleased());
  ASSERT_EQ(mp.getStats().freeSlabs, 0);
  ASSERT_EQ(mp.getStats().allocatedSlabs(), 0);
}

// since we have an atomic that is used to track pool size limiting, we can
// end up with scenarios where we overshoot the accounting when we release
// slabs and try to acquire allocs at the same time. See comment on
// getSlab(). This test tries to simulate that by having threads race with
// allocating memory and releasing slabs and ensures that the bug fixed in
// D4248865 does not show up anymore.
TEST_F(MemoryPoolTest, AllocReleaseMT) {
  auto slabAlloc = createSlabAllocator(100);
  auto usable = slabAlloc->getNumUsableSlabs();
  // only use half of the slabs for this. This ensures that when the race for
  // currSlabAllocSize_ is observed and we over-shoot, the slab allocator has
  // potential to let us overshoot.
  size_t poolSize = (usable / 2) * Slab::kSize;

  PoolId id = 5;
  // we dont really care about the number of alloc classes
  const uint32_t allocSize = Slab::kSize;

  MemoryPool mp(id, poolSize, *slabAlloc, {allocSize});
  ASSERT_EQ(id, mp.getId());
  ASSERT_EQ(mp.getCurrentAllocSize(), 0);

  const unsigned int numAllocsToTry = 100000;
  auto allocFn = [&]() {
    unsigned int tries = 0;
    std::set<void*> allocs;
    while (++tries < numAllocsToTry || !mp.allSlabsAllocated()) {
      void* alloc = mp.allocate(allocSize);
      if (alloc) {
        allocs.insert(alloc);
      }

      if (!allocs.empty() && tries % 5 == 0) {
        // only one alloc class.
        try {
          auto ctxt = mp.startSlabRelease(0,
                                          Slab::kInvalidClassId,
                                          SlabReleaseMode::kRebalance,
                                          nullptr,
                                          false);
          if (!ctxt.isReleased()) {
            mp.free(ctxt.getActiveAllocations().front());
            mp.completeSlabRelease(ctxt);
          }
        } catch (std::invalid_argument&) {
          // do nothing. we probably lost race on slab release;
        }
      }
    }
  };

  unsigned int nThreads = 5;
  std::vector<std::thread> threads;
  for (unsigned int i = 0; i < nThreads; i++) {
    threads.emplace_back(allocFn);
  }

  for (auto& thread : threads) {
    thread.join();
  }

  ASSERT_TRUE(mp.allSlabsAllocated());
  ASSERT_FALSE(mp.overLimit());
}

namespace {
constexpr uint32_t kSmallAllocSize = Slab::kSize / 8;
constexpr uint32_t kLargeAllocSize = Slab::kSize / 2;
constexpr size_t kMaxAllocAttempts = 10000;

// allocates `size` until the pool refuses, returning how many succeeded.
size_t allocateUntilRefused(MemoryPool& mp, uint32_t size) {
  size_t n = 0;
  while (n < kMaxAllocAttempts && mp.allocate(size) != nullptr) {
    ++n;
  }
  return n;
}
} // namespace

TEST_F(MemoryPoolTest, SlabCapStopsGreedyClassDuringFill) {
  auto slabAlloc = createSlabAllocator(20);
  MemoryPool mp(1, 10 * Slab::kSize, *slabAlloc,
                {kSmallAllocSize, kLargeAllocSize});
  const uint32_t largeCap = 2;
  mp.setSlabCaps({8, largeCap}, std::chrono::hours{1});

  const size_t largeAllocs = allocateUntilRefused(mp, kLargeAllocSize);

  EXPECT_EQ(largeCap * (Slab::kSize / kLargeAllocSize), largeAllocs);
  EXPECT_FALSE(mp.allSlabsAllocated());
  EXPECT_GT(mp.getSlabCapDenials(), 0);
  EXPECT_NE(nullptr, mp.allocate(kSmallAllocSize));
}

TEST_F(MemoryPoolTest, SlabCapsLiftAfterMaxDuration) {
  auto slabAlloc = createSlabAllocator(20);
  MemoryPool mp(1, 10 * Slab::kSize, *slabAlloc,
                {kSmallAllocSize, kLargeAllocSize});
  mp.setSlabCaps({1, 1}, std::chrono::seconds{0});

  const size_t threeSlabsOfLarge = 3 * (Slab::kSize / kLargeAllocSize);
  for (size_t i = 0; i < threeSlabsOfLarge; ++i) {
    ASSERT_NE(nullptr, mp.allocate(kLargeAllocSize));
  }

  EXPECT_FALSE(mp.hasSlabCaps());
}

TEST_F(MemoryPoolTest, UpdatedSlabCapsTakeEffectDuringFill) {
  auto slabAlloc = createSlabAllocator(20);
  MemoryPool mp(1, 10 * Slab::kSize, *slabAlloc,
                {kSmallAllocSize, kLargeAllocSize});
  mp.setSlabCaps({8, 1}, std::chrono::hours{1});
  const size_t perSlab = Slab::kSize / kLargeAllocSize;
  ASSERT_EQ(perSlab, allocateUntilRefused(mp, kLargeAllocSize));

  mp.updateSlabCaps({8, 3});

  EXPECT_TRUE(mp.hasSlabCaps());
  EXPECT_EQ(2 * perSlab, allocateUntilRefused(mp, kLargeAllocSize));
}

TEST_F(MemoryPoolTest, RefusedClassRetriesOnceCapsAreSetAgain) {
  auto slabAlloc = createSlabAllocator(20);
  MemoryPool mp(1, 10 * Slab::kSize, *slabAlloc,
                {kSmallAllocSize, kLargeAllocSize});
  mp.setSlabCaps({8, 1}, std::chrono::hours{1});
  const size_t perSlab = Slab::kSize / kLargeAllocSize;
  ASSERT_EQ(perSlab, allocateUntilRefused(mp, kLargeAllocSize));
  const auto denials = mp.getSlabCapDenials();

  EXPECT_EQ(nullptr, mp.allocate(kLargeAllocSize));
  EXPECT_EQ(denials + 1, mp.getSlabCapDenials());

  mp.setSlabCaps({8, 2}, std::chrono::hours{1});

  EXPECT_EQ(perSlab, allocateUntilRefused(mp, kLargeAllocSize));
}

TEST_F(MemoryPoolTest, UpdateDoesNotRearmLiftedSlabCaps) {
  auto slabAlloc = createSlabAllocator(20);
  MemoryPool mp(1, 4 * Slab::kSize, *slabAlloc,
                {kSmallAllocSize, kLargeAllocSize});
  mp.setSlabCaps({2, 2}, std::chrono::hours{1});
  allocateUntilRefused(mp, kSmallAllocSize);
  allocateUntilRefused(mp, kLargeAllocSize);
  ASSERT_FALSE(mp.hasSlabCaps());

  mp.updateSlabCaps({1, 1});

  EXPECT_FALSE(mp.hasSlabCaps());
}

TEST_F(MemoryPoolTest, SlabCapsLiftOnceThePoolFills) {
  auto slabAlloc = createSlabAllocator(20);
  MemoryPool mp(1, 4 * Slab::kSize, *slabAlloc,
                {kSmallAllocSize, kLargeAllocSize});
  mp.setSlabCaps({2, 2}, std::chrono::hours{1});

  allocateUntilRefused(mp, kSmallAllocSize);
  allocateUntilRefused(mp, kLargeAllocSize);

  EXPECT_TRUE(mp.allSlabsAllocated());
  EXPECT_FALSE(mp.hasSlabCaps());
}
