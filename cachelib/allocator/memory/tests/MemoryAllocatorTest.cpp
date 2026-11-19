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

#include <unordered_map>
#include <vector>

#include "cachelib/allocator/memory/MemoryAllocator.h"
#include "cachelib/allocator/memory/tests/TestBase.h"
#include "cachelib/common/Serialization.h"

using namespace facebook::cachelib;
using namespace facebook::cachelib::tests;

using MemoryAllocatorTest = SlabAllocatorTestBase;

constexpr size_t SerializationBufferSize = 100 * 1024;
using Config = typename MemoryAllocator::Config;

namespace {
unsigned int forEachAllocationCount;
bool test_callback(void* /* unused */, AllocInfo /* unused */) {
  forEachAllocationCount++;
  return true;
}
} // namespace

namespace facebook::cachelib {

TEST_F(MemoryAllocatorTest, Create) {
  size_t size = 100 * Slab::kSize;
  void* memory = allocate(size);
  auto allocSizes = getRandomAllocSizes(5);
  MemoryAllocator m(getDefaultMemoryAllocatorConfig(allocSizes), memory, size);

  ASSERT_EQ(m.getPoolIds().size(), 0);
  ASSERT_LE(m.getMemorySize(), size);
  ASSERT_EQ(m.getMemorySize(), MemoryAllocator::getMemorySize(size));
  // minimum of one slab worth of memory.
  ASSERT_GT(m.getMemorySize(), Slab::kSize);
}

TEST_F(MemoryAllocatorTest, AddPool) {
  const unsigned int numClasses = 5;
  const unsigned int numPools = 4;
  // create enough memory for 4 pools with 5 allocation classes each and 5 slabs
  // each for each allocation class.
  const size_t poolSize = numClasses * 5 * Slab::kSize;
  // allocate enough memory for all the pools plus slab headers.
  const size_t size = numPools * poolSize + 2 * Slab::kSize;
  void* memory = allocate(size);

  // adding a pool with  too many alloc classes should throw.
  ASSERT_THROW(
      {
        auto allocSizes = getRandomAllocSizes(MemoryAllocator::kMaxClasses + 1);
        MemoryAllocator m(getDefaultMemoryAllocatorConfig(allocSizes), memory,
                          size);
      },
      std::invalid_argument);

  auto allocSizes = getRandomAllocSizes(numClasses);
  MemoryAllocator m(getDefaultMemoryAllocatorConfig(allocSizes), memory, size);

  ASSERT_GE(m.getMemorySize(), numPools * poolSize);

  auto poolIds = m.getPoolIds();
  ASSERT_EQ(poolIds.size(), 0);

  for (unsigned int i = 0; i < numPools - 1; i++) {
    const std::string poolName = getRandomStr();
    PoolId pid = Slab::kInvalidPoolId;
    ASSERT_NO_THROW(pid = m.addPool(poolName, poolSize));
    ASSERT_NE(pid, Slab::kInvalidPoolId);
    ASSERT_EQ(m.getPoolId(poolName), pid);
  }

  // adding pool with overriding the alloc sizes should work as well.
  auto poolName = getRandomStr();

  // override with too many classes should fail as well.
  ASSERT_THROW(m.addPool(poolName,
                         poolSize,
                         getRandomAllocSizes(MemoryAllocator::kMaxClasses + 1)),
               std::invalid_argument);

  PoolId pid = m.addPool(poolName, poolSize, getRandomAllocSizes(3));
  ASSERT_NE(pid, Slab::kInvalidPoolId);
  ASSERT_EQ(m.getPoolId(poolName), pid);
}

// makes allocations across the sizes for the pool and returns it.
void testAllocForPool(MemoryAllocator& m,
                      PoolId pid,
                      const std::set<uint32_t>& sizes,
                      std::vector<void*>& allocs) {
  const auto& pool = m.getPool(pid);
  auto totalSize = pool.getPoolSize();
  auto allocSize = pool.getCurrentAllocSize();

  // this is the available space.
  auto remaining = totalSize - allocSize;

  // ensure that the remaining size has at least one slab per allocation size
  // roughly.
  ASSERT_GT(remaining, sizes.size() * Slab::kSize);

  // allocating with 0 or more than Slab::kSize or sizes.rbegin() should throw
  ASSERT_THROW(m.allocate(pid, 0), std::invalid_argument);
  ASSERT_THROW(
      m.allocate(pid, Slab::kSize + folly::Random::rand32(1, UINT32_MAX)),
      std::invalid_argument);
  ASSERT_THROW(m.allocate(pid, *sizes.rbegin() + 1), std::invalid_argument);

  uint32_t prev = 1;
  for (const auto size : sizes) {
    const auto range = prev == size ? 1 : size - prev;
    uint32_t randomSize = folly::Random::rand32() % range + prev;
    void* memory = m.allocate(pid, randomSize);
    ASSERT_NE(memory, nullptr);
    allocs.push_back(memory);
    prev = size + 1;
  }

  // allocating from invalid pools should fail.
  ASSERT_THROW(m.allocate(-1, 100), std::invalid_argument);
  ASSERT_THROW(m.allocate(MemoryPoolManager::kMaxPools, 100),
               std::invalid_argument);
}

// testing the alloc and free interfaces.
TEST_F(MemoryAllocatorTest, AllocFree) {
  const unsigned int numClasses = 10;
  const unsigned int numPools = 4;
  // create enough memory for 4 pools with 5 allocation classes each and 5 slabs
  // each for each allocation class.
  const size_t poolSize = numClasses * 5 * Slab::kSize;
  // allocate enough memory for all the pools plus slab headers.
  const size_t totalSize = numPools * poolSize + 2 * Slab::kSize;
  void* memory = allocate(totalSize);
  auto allocSizes = getRandomAllocSizes(numClasses);
  MemoryAllocator m(getDefaultMemoryAllocatorConfig(allocSizes), memory,
                    totalSize);

  std::unordered_map<PoolId, const std::set<uint32_t>> pools;
  std::unordered_map<PoolId, size_t> beforeSizes;
  for (unsigned int i = 0; i < numPools; i++) {
    auto nClasses = folly::Random::rand32() % numClasses + 1;
    auto sizes = getRandomAllocSizes(nClasses);
    auto pid = m.addPool(getRandomStr(), poolSize, sizes);
    ASSERT_NE(pid, Slab::kInvalidPoolId);
    pools.insert({pid, sizes});
    beforeSizes.insert({pid, m.getPool(pid).getCurrentAllocSize()});
  }
  ASSERT_EQ(pools.size(), numPools);

  using PoolAllocs = std::unordered_map<PoolId, std::vector<void*>>;
  PoolAllocs poolAllocs;
  for (const auto& kv : pools) {
    const auto pid = kv.first;
    const auto& sizes = kv.second;
    std::vector<void*> allocs;
    testAllocForPool(m, pid, sizes, allocs);
    // must have one allocation per size.
    ASSERT_EQ(allocs.size(), sizes.size());
    size_t sum = 0;
    for (auto size : sizes) {
      sum += size;
    }
    ASSERT_GT(m.getPool(pid).getCurrentAllocSize(), beforeSizes[pid]);
    // we must have drained at least corresponding to allocSizes.
    ASSERT_GE(m.getPool(pid).getCurrentAllocSize() - beforeSizes[pid], sum);
    poolAllocs.insert({pid, allocs});
  }

  ASSERT_EQ(poolAllocs.size(), numPools);

  for (const auto& kv : poolAllocs) {
    const auto pid = kv.first;
    const auto& allocs = kv.second;
    for (auto alloc : allocs) {
      ASSERT_NO_THROW(m.free(alloc));
    }
    // we must have restored the pool size.
    ASSERT_EQ(m.getPool(pid).getCurrentAllocSize(), beforeSizes[pid]);
  }

  const unsigned int ntries = 10;
  // freeing random memory not belonging to the memory allocator should throw
  // as well.
  for (const auto& kv : pools) {
    const auto pid = kv.first;
    const auto& sizes = kv.second;
    size_t prev = 1;
    for (const auto size : sizes) {
      const auto range = prev == size ? 1 : size - prev;
      for (unsigned int i = 0; i < ntries; i++) {
        uint32_t randomSize = folly::Random::rand32() % range + prev;
        void* mem = allocate(randomSize);
        ASSERT_NE(mem, nullptr);
        ASSERT_THROW(m.free(mem), std::invalid_argument);
      }
      prev = size + 1;
    }
    // all the above frees should not have changed the pool allocated size.
    ASSERT_EQ(m.getPool(pid).getCurrentAllocSize(), beforeSizes[pid]);
  }
}

TEST_F(MemoryAllocatorTest, GetAllocInfo) {
  const unsigned int numClasses = 10;
  const unsigned int numPools = 3;
  const size_t poolSize = numClasses * 5 * Slab::kSize;
  // allocate enough memory for all the pools plus slab headers.
  const size_t size = numPools * poolSize + 2 * Slab::kSize;

  MemoryAllocator m(
      getDefaultMemoryAllocatorConfig(getRandomAllocSizes(numClasses)),
      allocate(size), size);

  ASSERT_EQ(m.getPoolIds().size(), 0);

  for (unsigned int i = 0; i < numPools; i++) {
    const std::string poolName = getRandomStr();
    PoolId pid = Slab::kInvalidPoolId;
    auto allocSizes = getRandomAllocSizes(numClasses);
    ASSERT_NO_THROW(pid = m.addPool(poolName, poolSize, allocSizes));
    ASSERT_NE(pid, Slab::kInvalidPoolId);
    // try to allocate for the allocation classes and check the getAllocInfo
    // and getAllocationClassId
    size_t prev = 1;
    for (const auto s : allocSizes) {
      const auto range = prev == s ? 1 : s - prev;
      uint32_t randomSize = (folly::Random::rand32() % range) + prev;
      void* memory = m.allocate(pid, randomSize);
      ASSERT_NE(memory, nullptr);
      const auto classId = m.getAllocationClassId(pid, randomSize);
      ASSERT_NE(classId, Slab::kInvalidClassId);
      const auto allocInfo = m.getAllocInfo(memory);
      ASSERT_EQ(pid, allocInfo.poolId);
      ASSERT_EQ(classId, allocInfo.classId);
      ASSERT_LE(randomSize, allocInfo.allocSize);
      prev = s + 1;
    }
  }

  PoolId invalidPoolId = *m.getPoolIds().rbegin() + 1;
  ASSERT_THROW(m.getPool(invalidPoolId), std::invalid_argument);
  ASSERT_THROW(m.getAllocationClassId(invalidPoolId, getRandomAllocSize()),
               std::invalid_argument);
}

TEST_F(MemoryAllocatorTest, Serialization) {
  const unsigned int numClasses = 10;
  const unsigned int numPools = 3;
  const size_t poolSize = numClasses * 5 * Slab::kSize;
  // allocate enough memory for all the pools plus slab headers.
  const size_t size = numPools * poolSize + 2 * Slab::kSize;

  void* memory = allocate(size);
  MemoryAllocator m(
      getDefaultMemoryAllocatorConfig(getRandomAllocSizes(numClasses)), memory,
      size);

  ASSERT_EQ(m.getPoolIds().size(), 0);

  std::vector<std::pair<PoolId, std::set<uint32_t>>> allocatedPools;
  for (unsigned int i = 0; i < numPools; i++) {
    const std::string poolName = getRandomStr();
    PoolId pid = Slab::kInvalidPoolId;
    auto allocSizes = getRandomAllocSizes(numClasses);
    ASSERT_NO_THROW(pid = m.addPool(poolName, poolSize, allocSizes));
    ASSERT_NE(pid, Slab::kInvalidPoolId);
    // try to allocate for the allocation classes and check the getAllocInfo
    // and getAllocationClassId
    size_t prev = 1;
    for (const auto s : allocSizes) {
      const auto range = prev == s ? 1 : s - prev;
      size_t randomSize = (folly::Random::rand32() % range) + prev;
      void* mem = m.allocate(pid, randomSize);
      ASSERT_NE(mem, nullptr);
      const auto classId = m.getAllocationClassId(pid, randomSize);
      ASSERT_NE(classId, Slab::kInvalidClassId);
      const auto allocInfo = m.getAllocInfo(mem);
      ASSERT_EQ(allocInfo.classId, classId);
      ASSERT_EQ(allocInfo.poolId, pid);
      prev = s + 1;
    }
    allocatedPools.emplace_back(pid, allocSizes);
  }

  uint8_t buffer[SerializationBufferSize];
  uint8_t* begin = buffer;
  uint8_t* end = buffer + SerializationBufferSize;
  Serializer serializer(begin, end);
  serializer.serialize(m.saveState());

  // Attach to a different address
  void* memory2 = allocate(size);
  ASSERT_NE(memory, memory2);
  ASAN_UNPOISON_MEMORY_REGION(memory, size);
  memcpy(memory2, memory, size);

  Deserializer deserializer(begin, end);
  MemoryAllocator m2(
      deserializer.deserialize<serialization::MemoryAllocatorObject>(),
      memory2,
      size,
      true /* disableCoredump*/,
      true /* enableAsanPoisoning */);
  ASSERT_TRUE(isSameMemoryAllocator(m, m2));

  for (const auto& itr : allocatedPools) {
    const auto pid = itr.first;
    const auto& allocSizes = itr.second;
    size_t prev = 1;
    for (const auto s : allocSizes) {
      const auto range = prev == s ? 1 : s - prev;
      size_t randomSize = (folly::Random::rand32() % range) + prev;
      void* mem = m2.allocate(pid, randomSize);
      ASSERT_NE(mem, nullptr);
      const auto classId = m2.getAllocationClassId(pid, randomSize);
      ASSERT_NE(classId, Slab::kInvalidClassId);
      const auto allocInfo = m2.getAllocInfo(mem);
      ASSERT_EQ(pid, allocInfo.poolId);
      ASSERT_EQ(classId, allocInfo.classId);
      prev = s + 1;
    }
  }
}

TEST_F(MemoryAllocatorTest, Restorable) {
  const unsigned int numClasses = 10;
  const unsigned int numPools = 3;
  const size_t poolSize = numClasses * 5 * Slab::kSize;
  // allocate enough memory for all the pools plus slab headers.
  const size_t size = numPools * poolSize + 2 * Slab::kSize;

  auto c = getDefaultMemoryAllocatorConfig(getRandomAllocSizes(numClasses));
  {
    void* memory = allocate(size);
    MemoryAllocator m(c, memory, size);
    ASSERT_TRUE(m.isRestorable());
    uint8_t buffer[SerializationBufferSize];
    uint8_t* begin = buffer;
    uint8_t* end = buffer + SerializationBufferSize;
    Serializer serializer(begin, end);
    serializer.serialize(m.saveState());

    Deserializer deserializer(begin, end);
    MemoryAllocator m2(
        deserializer.deserialize<serialization::MemoryAllocatorObject>(),
        memory,
        size,
        true /* disableCoredump*/,
        true /* enableAsanPoisoning */);
    ASSERT_TRUE(isSameMemoryAllocator(m, m2));
    ASSERT_TRUE(m2.isRestorable());

    memset(buffer, 0, sizeof(buffer));
    Serializer serializer2(begin, end);
    serializer2.serialize(m2.saveState());

    Deserializer deserializer2(begin, end);
    auto correctState =
        deserializer2.deserialize<serialization::MemoryAllocatorObject>();
    MemoryAllocator m3(correctState, memory, size, true /* disableCoredump*/,
                       true /* enableAsanPoisoning */);
    ASSERT_TRUE(m2.isRestorable());

    size_t randomSize =
        size + ((folly::Random::rand32() % 5) + 1) * Slab::kSize;

    ASSERT_THROW(MemoryAllocator m4(correctState, memory, randomSize,
                                    true /* disableCoredump*/,
                                    true /* enableAsanPoisoning */),
                 std::invalid_argument);

    randomSize = size - ((folly::Random::rand32() % 5) + 1) * Slab::kSize;

    ASSERT_THROW(MemoryAllocator m4(correctState, memory, randomSize,
                                    true /* disableCoredump*/,
                                    true /* enableAsanPoisoning */),
                 std::invalid_argument);
  }

  {
    MemoryAllocator m(c, size);
    ASSERT_FALSE(m.isRestorable());
    ASSERT_THROW(m.saveState(), std::logic_error);
  }
}

TEST_F(MemoryAllocatorTest, ResizePool) {
  const unsigned int numClasses = 10;
  const unsigned int numPools = 2;
  const size_t poolSize = numClasses * 5 * Slab::kSize;

  // allocate enough memory for all the pools plus slab headers.
  const size_t size = numPools * poolSize + 2 * Slab::kSize;

  std::set<uint32_t> allocSizes;
  allocSizes.insert(Slab::kSize);

  void* memory = allocate(size);
  MemoryAllocator m(getDefaultMemoryAllocatorConfig(allocSizes), memory, size);

  auto poolName1 = getRandomStr();
  auto p1 = m.addPool(poolName1, poolSize);

  auto poolName2 = getRandomStr();
  auto p2 = m.addPool(poolName2, poolSize);

  ASSERT_EQ(p1, m.getPoolId(poolName1));
  ASSERT_EQ(p2, m.getPoolId(poolName2));

  const PoolId invalidPoolId = p2 + 1;
  ASSERT_THROW(m.getPool(invalidPoolId), std::invalid_argument);

  ASSERT_TRUE(m.resizePools(p1, p2, Slab::kSize));

  ASSERT_THROW(
      { m.resizePools(invalidPoolId, p2, Slab::kSize); },
      std::invalid_argument);

  ASSERT_THROW(
      { m.resizePools(p1, invalidPoolId, Slab::kSize); },
      std::invalid_argument);
}

TEST_F(MemoryAllocatorTest, GrowShrinkPool) {
  const unsigned int numClasses = 10;
  const unsigned int numPools = 3;
  const size_t poolSize = numClasses * 5 * Slab::kSize;

  // allocate enough memory for all the pools plus slab headers.
  const size_t size = numPools * poolSize + 1 * Slab::kSize;

  void* memory = allocate(size);
  std::set<uint32_t> allocSizes;
  allocSizes.insert(Slab::kSize);
  MemoryAllocator m(getDefaultMemoryAllocatorConfig(allocSizes), memory, size);

  auto poolName1 = getRandomStr();
  auto p1 = m.addPool(poolName1, poolSize);

  auto poolName2 = getRandomStr();
  auto p2 = m.addPool(poolName2, poolSize);

  ASSERT_EQ(p1, m.getPoolId(poolName1));
  ASSERT_EQ(p2, m.getPoolId(poolName2));

  const PoolId invalidPoolId = p2 + 1;
  ASSERT_THROW(m.getPool(invalidPoolId), std::invalid_argument);
  // we shouldn't be able to resize our pools beyond
  // the memory in slab allocator has. Currently we have two pools taking
  // 2/3rd of the available memory. So you should not be able to grow beyond
  // the total size.
  ASSERT_FALSE(m.growPool(p1, size));
  ASSERT_THROW({ m.growPool(invalidPoolId, size); }, std::invalid_argument);

  // should not be able to shrink more than what we have as well.
  ASSERT_FALSE(m.shrinkPool(p1, size));
  ASSERT_THROW({ m.shrinkPool(invalidPoolId, size); }, std::invalid_argument);

  ASSERT_EQ(m.getPool(p1).getPoolSize(), poolSize);

  ASSERT_TRUE(m.growPool(p1, Slab::kSize));
  ASSERT_EQ(m.getPool(p1).getPoolSize(), poolSize + Slab::kSize);

  ASSERT_TRUE(m.shrinkPool(p1, Slab::kSize));
  ASSERT_EQ(m.getPool(p1).getPoolSize(), poolSize);

  // grow so that there is no more memory available.
  ASSERT_TRUE(m.growPool(p1, poolSize));
  // should not be able to grow anymore
  ASSERT_FALSE(m.growPool(p1, Slab::kSize));

  ASSERT_TRUE(m.shrinkPool(p2, Slab::kSize));
  ASSERT_TRUE(m.growPool(p1, Slab::kSize));
}

// prefetch() takes pointers that may be stale: their slab may since have been
// released or given to an allocation class of another size. Unlike
// unCompress(), it must not check them; in debug builds unCompress() and
// memoryAtOffset() reject these pointers.
TEST_F(MemoryAllocatorTest, PrefetchStale) {
  const size_t totalSize = 5 * Slab::kSize;
  void* memory = allocate(totalSize);
  const std::set<uint32_t> sizes{1024};
  MemoryAllocator m(getDefaultMemoryAllocatorConfig(sizes), memory, totalSize);
  const auto pid = m.addPool("pool", 2 * Slab::kSize, sizes);
  void* alloc = m.allocate(pid, 1024);
  ASSERT_NE(nullptr, alloc);

  // One slab holds 1024-byte allocations and the other is not carved yet.
  const uint32_t pastSlabEnd = Slab::kSize / 1024;
  for (const uint32_t slabIdx : {0u, 1u}) {
    for (const uint32_t allocIdx : {1u, pastSlabEnd}) {
      const CompressedPtr4B ptr{static_cast<int64_t>(
          compress(CompressedPtr4B{}, slabIdx, allocIdx, false))};
      m.prefetch(ptr, false /* isMultiTiered */);
    }
  }
  m.free(alloc);
}

TEST_F(MemoryAllocatorTest, isAllocFreed) {
  const size_t numSlabs = 100;
  const size_t size = numSlabs * Slab::kSize;
  const size_t allocatorSize = size + 10 * Slab::kSize;
  const size_t smallSize = Slab::kSize / 10;
  auto config = getDefaultMemoryAllocatorConfig(
      std::set<uint32_t>{smallSize, Slab::kSize});

  void* memory = allocate(allocatorSize);
  MemoryAllocator m(config, memory, allocatorSize);
  auto pid = m.addPool(getRandomStr(), size);

  // allocate until no more space
  std::vector<void*> allocations;
  while (void* alloc = m.allocate(pid, smallSize)) {
    memset(alloc, 'a', smallSize);
    allocations.push_back(alloc);
  }

  for (unsigned int i = 0; i < numSlabs; ++i) {
    // must call startSlabRelease() before isAllocFreed(), because
    // isAllocFreed() requires the slab header's isMarkedForRelease field to
    // be true
    auto releaseContext = m.startSlabRelease(pid, 0, Slab::kInvalidClassId,
                                             SlabReleaseMode::kResize);
    ASSERT_FALSE(releaseContext.isReleased());
    auto activeAllocs = releaseContext.getActiveAllocations();
    for (void* slabAlloc : activeAllocs) {
      ASSERT_FALSE(m.isAllocFreed(releaseContext, slabAlloc));
      m.free(slabAlloc);
      ASSERT_TRUE(m.isAllocFreed(releaseContext, slabAlloc));
    }

    m.completeSlabRelease(releaseContext);
    ASSERT_TRUE(!activeAllocs.empty());
    for (void* slabAlloc : activeAllocs) {
      // slab release already completed
      ASSERT_THROW(m.isAllocFreed(releaseContext, slabAlloc),
                   std::invalid_argument);
    }
  }
}

TEST_F(MemoryAllocatorTest, ReleaseSlabToReceiver) {
  const size_t numSlabs = 100;
  const size_t usableSize = numSlabs * Slab::kSize;
  const size_t allocatorSize = usableSize + 10 * Slab::kSize;

  const size_t size1 = Slab::kSize / 100;
  const size_t size2 = Slab::kSize / 10;

  void* memory = allocate(allocatorSize);
  MemoryAllocator m(
      getDefaultMemoryAllocatorConfig(std::set<uint32_t>{size1, size2}), memory,
      allocatorSize);
  auto pid = m.addPool(getRandomStr(), usableSize);

  // allocate until no more space
  std::vector<void*> allocations1;
  std::vector<void*> allocations2;
  for (int i = 0;; ++i) {
    void* alloc;
    if (i % 2 == 0) {
      alloc = m.allocate(pid, size1);
      if (alloc) {
        allocations1.push_back(alloc);
      }
    } else {
      alloc = m.allocate(pid, size2);
      if (alloc) {
        allocations2.push_back(alloc);
      } else {
        // if we cannot allocate size1, that means there is no
        // space left in the memory allocator, so we stop
        break;
      }
    }
  }
  ASSERT_FALSE(allocations1.empty());
  ASSERT_FALSE(allocations2.empty());

  const auto allocInfo1 = m.getAllocInfo(allocations1[0]);
  const auto allocInfo2 = m.getAllocInfo(allocations2[0]);

  // Release one slab belonging to allocations2 and move to
  // the AC that stores allocations1 should give us more space
  // to allocate more for allocations2
  auto ctx = m.startSlabRelease(pid, allocInfo1.classId, allocInfo2.classId,
                                SlabReleaseMode::kRebalance);

  // clear all allocations1 so we can be sure this slab can be freed
  for (auto alloc : allocations1) {
    m.free(alloc);
  }

  m.completeSlabRelease(ctx);
  ASSERT_NE(nullptr, m.allocate(pid, size2));

  // allocate until it's filled up again
  while (true) {
    void* alloc = m.allocate(pid, size2);
    if (alloc) {
      allocations2.push_back(alloc);
    } else {
      // if we cannot allocate size1, that means there is no
      // space left in the memory allocator, so we stop
      break;
    }
  }

  // since allocation1 was all freed, this slab release should
  // not require us to call `completeSlabRelease`
  auto ctx2 = m.startSlabRelease(pid, allocInfo1.classId, allocInfo2.classId,
                                 SlabReleaseMode::kRebalance);
  ASSERT_TRUE(ctx2.isReleased());
  ASSERT_NE(nullptr, m.allocate(pid, size2));

  // Verify we throw if we specify a receiver when the mode is kResize
  ASSERT_THROW(m.startSlabRelease(pid, allocInfo1.classId, allocInfo2.classId,
                                  SlabReleaseMode::kResize),
               std::invalid_argument);

  auto ctx3 = m.startSlabRelease(pid, allocInfo2.classId, Slab::kInvalidClassId,
                                 SlabReleaseMode::kResize);
  ctx3.setReceiver(allocInfo1.classId);
  ASSERT_THROW(m.completeSlabRelease(ctx3), std::invalid_argument);
}

TEST_F(MemoryAllocatorTest, ZeroedSlabAllocs) {
  const size_t numSlabs = 100;
  const size_t size = numSlabs * Slab::kSize;
  const size_t allocatorSize = size + 10 * Slab::kSize;
  const size_t smallSize = Slab::kSize / 10;
  MemoryAllocator::Config config{
      std::set<uint32_t>{smallSize,    // 2 AllocationClass,
                         Slab::kSize}, // one for small allocation
                                       // the other for slab allocation
      true /* enableZeroedSlabAllocs */, true /* disableFullCoredump*/,
      false /* lockMemory */, true /* enableAsanPoisoning */};

  void* memory = allocate(allocatorSize);
  MemoryAllocator m(config, memory, allocatorSize);
  auto pid = m.addPool(getRandomStr(), size);

  // allocate until no more space
  std::vector<void*> allocations;
  while (void* alloc = m.allocate(pid, smallSize)) {
    memset(alloc, 'a', smallSize);
    allocations.push_back(alloc);
  }

  // free all allocations
  for (auto alloc : allocations) {
    m.free(alloc);
  }
  allocations.clear();

  // release slabs from the first allocation class, since we just allocated all
  // memory using smallSize. this should set all slabs to zero
  for (unsigned int i = 0; i < numSlabs; ++i) {
    auto releaseContext =
        m.startSlabRelease(pid, 0 /* first allocation class */,
                           Slab::kInvalidClassId, SlabReleaseMode::kResize);
    ASSERT_EQ(0, releaseContext.getActiveAllocations().size());
    ASSERT_TRUE(releaseContext.isReleased());
  }

  // allocate slabs and each one's content should be zero
  std::vector<void*> slabAllocations;
  while (void* slabAlloc = m.allocateZeroedSlab(pid)) {
    for (unsigned int i = 0; i < Slab::kSize; ++i) {
      ASSERT_EQ(0, reinterpret_cast<char*>(slabAlloc)[i]);
    }
    slabAllocations.push_back(slabAlloc);
  }

  // save and restore
  auto serializedData = m.saveState();
  MemoryAllocator m2(serializedData, memory, allocatorSize,
                     true /* disableCoredump*/, true /* enableAsanPoisoning */);

  // write to all the slabs and then free them using restored memory allocator
  for (void* slabAlloc : slabAllocations) {
    memset(slabAlloc, 'b', Slab::kSize);
  }

  // release slabs from the second allocation class, since we just allocated all
  // memory using Slab::kSize. this should set all slabs to zero
  for (unsigned int i = 0; i < numSlabs; ++i) {
    auto releaseContext =
        m2.startSlabRelease(pid, 1 /* second allocation class */,
                            Slab::kInvalidClassId, SlabReleaseMode::kResize);
    ASSERT_FALSE(releaseContext.isReleased());
    auto activeAllocs = releaseContext.getActiveAllocations();
    for (void* slabAlloc : activeAllocs) {
      ASSERT_FALSE(m2.isAllocFreed(releaseContext, slabAlloc));
      m2.free(slabAlloc);
      ASSERT_TRUE(m2.isAllocFreed(releaseContext, slabAlloc));
    }
    m2.completeSlabRelease(releaseContext);
  }

  // try allocate slabs again, they should be zero
  while (void* slabAlloc = m2.allocateZeroedSlab(pid)) {
    for (unsigned int i = 0; i < Slab::kSize; ++i) {
      ASSERT_EQ(0, reinterpret_cast<char*>(slabAlloc)[i]);
    }
  }
}
TEST_F(MemoryAllocatorTest, forEachAllocation) {
  const size_t numSlabs = 1;
  const size_t size = numSlabs * Slab::kSize;
  const size_t allocatorSize = size + 10 * Slab::kSize;
  const size_t smallSize = Slab::kSize / 10;
  auto config = getDefaultMemoryAllocatorConfig(
      std::set<uint32_t>{smallSize, Slab::kSize});

  void* memory = allocate(allocatorSize);
  MemoryAllocator m(config, memory, allocatorSize);
  auto pid = m.addPool(getRandomStr(), size);

  std::vector<void*> allocations;
  while (void* alloc = m.allocate(pid, smallSize)) {
    allocations.push_back(alloc);
  }

  forEachAllocationCount = 0;
  m.forEachAllocation(test_callback);
  ASSERT_EQ(forEachAllocationCount, allocations.size());

  // must call startSlabRelease() before isAllocFreed(), because
  // isAllocFreed() requires the slab header's isMarkedForRelease field to
  // be true
  auto releaseContext = m.startSlabRelease(pid, 0, Slab::kInvalidClassId,
                                           SlabReleaseMode::kResize);
  ASSERT_FALSE(releaseContext.isReleased());

  // Check that we dont iterate over slab under slab release
  forEachAllocationCount = 0;
  m.forEachAllocation(test_callback);
  ASSERT_EQ(forEachAllocationCount, 0);

  auto activeAllocs = releaseContext.getActiveAllocations();
  for (void* slabAlloc : activeAllocs) {
    ASSERT_FALSE(m.isAllocFreed(releaseContext, slabAlloc));
    m.free(slabAlloc);
    ASSERT_TRUE(m.isAllocFreed(releaseContext, slabAlloc));
  }

  m.completeSlabRelease(releaseContext);
  ASSERT_TRUE(!activeAllocs.empty());
  for (void* slabAlloc : activeAllocs) {
    // slab release already completed
    ASSERT_THROW(m.isAllocFreed(releaseContext, slabAlloc),
                 std::invalid_argument);
  }

  // Check that we dont iterate over slab which has been released.
  forEachAllocationCount = 0;
  m.forEachAllocation(test_callback);
  ASSERT_EQ(forEachAllocationCount, 0);
}

TEST_F(MemoryAllocatorTest, isValidAllocSize) {
  // Test zero size - invalid
  EXPECT_FALSE(MemoryAllocator::isValidAllocSize(0));

  // Test sizes below minimum - invalid
  EXPECT_FALSE(MemoryAllocator::isValidAllocSize(Slab::kMinAllocSize - 1));
  EXPECT_FALSE(MemoryAllocator::isValidAllocSize(32));

  // Test sizes above maximum - invalid
  EXPECT_FALSE(MemoryAllocator::isValidAllocSize(Slab::kSize + 1));
  EXPECT_FALSE(MemoryAllocator::isValidAllocSize(Slab::kSize + 8));

  // Test misaligned sizes - invalid
  EXPECT_FALSE(MemoryAllocator::isValidAllocSize(65));
  EXPECT_FALSE(MemoryAllocator::isValidAllocSize(127));
  EXPECT_FALSE(MemoryAllocator::isValidAllocSize(1023));

  // Test valid sizes
  EXPECT_TRUE(MemoryAllocator::isValidAllocSize(Slab::kMinAllocSize));
  EXPECT_TRUE(MemoryAllocator::isValidAllocSize(Slab::kSize));
  EXPECT_TRUE(MemoryAllocator::isValidAllocSize(128));
  EXPECT_TRUE(MemoryAllocator::isValidAllocSize(256));
  EXPECT_TRUE(MemoryAllocator::isValidAllocSize(264));
  EXPECT_TRUE(MemoryAllocator::isValidAllocSize(1024));
  EXPECT_TRUE(MemoryAllocator::isValidAllocSize(4096));
}

#if FOLLY_SANITIZE_ADDRESS
TEST_F(MemoryAllocatorTest, AsanPoisoningDisabled) {
  const size_t poolSize = 10 * Slab::kSize;
  const size_t allocatorSize = poolSize + 2 * Slab::kSize;
  const uint32_t allocSize = 1024;

  MemoryAllocator::Config config{
      std::set<uint32_t>{allocSize}, false /* enableZeroedSlabAllocs */,
      true /* disableFullCoredump */, false /* lockMemory */,
      false /* enableAsanPoisoning */};

  void* memory = allocate(allocatorSize);
  MemoryAllocator m(config, memory, allocatorSize);
  auto pid = m.addPool(getRandomStr(), poolSize);

  void* alloc = m.allocate(pid, allocSize);
  ASSERT_NE(alloc, nullptr);
  memset(alloc, 0xAB, allocSize);

  m.free(alloc);

  // With poisoning disabled, accessing freed memory should not trigger ASAN
  uint8_t byte = reinterpret_cast<uint8_t*>(alloc)[0];
  ASSERT_EQ(byte, 0xAB);
}

TEST_F(MemoryAllocatorTest, AsanPoisoningEnabled) {
  const size_t poolSize = 10 * Slab::kSize;
  const size_t allocatorSize = poolSize + 2 * Slab::kSize;
  const uint32_t allocSize = 1024;

  MemoryAllocator::Config config{
      std::set<uint32_t>{allocSize}, false /* enableZeroedSlabAllocs */,
      true /* disableFullCoredump */, false /* lockMemory */,
      true /* enableAsanPoisoning */};

  void* memory = allocate(allocatorSize);
  MemoryAllocator m(config, memory, allocatorSize);
  auto pid = m.addPool(getRandomStr(), poolSize);

  void* alloc = m.allocate(pid, allocSize);
  ASSERT_NE(alloc, nullptr);
  memset(alloc, 0xAB, allocSize);

  m.free(alloc);

  // With poisoning enabled, reading the freed allocation must trip ASAN.
  EXPECT_DEATH(
      {
        volatile uint8_t byte = reinterpret_cast<uint8_t*>(alloc)[0];
        (void)byte;
      },
      "AddressSanitizer");
}
#endif

} // namespace facebook::cachelib
