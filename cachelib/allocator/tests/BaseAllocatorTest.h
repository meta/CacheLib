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

#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/Singleton.h>
#include <folly/synchronization/Baton.h>
#include <folly/synchronization/Latch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "cachelib/allocator/BackgroundMoverStrategy.h"
#include "cachelib/allocator/CCacheAllocator.h"
#include "cachelib/allocator/FreeMemStrategy.h"
#include "cachelib/allocator/LruTailAgeStrategy.h"
#include "cachelib/allocator/ManualStrategy.h"
#include "cachelib/allocator/MarginalHitsOptimizeStrategy.h"
#include "cachelib/allocator/MarginalHitsStrategy.h"
#include "cachelib/allocator/PoolRebalancer.h"
#include "cachelib/allocator/Util.h"
#include "cachelib/allocator/tests/TestBase.h"
#include "cachelib/common/TestUtils.h"
#include "cachelib/compact_cache/CCacheCreator.h"

namespace facebook {
namespace cachelib {
namespace detail {
template <typename HandleT>
void objcacheUnmarkNascent(const HandleT& hdl) {
  hdl.unmarkNascent();
}

template <typename HandleT, typename AllocT>
HandleT createHandleWithWaitContextForTest(AllocT& alloc) {
  return HandleT{alloc};
}

template <typename HandleT>
std::shared_ptr<typename HandleT::ItemWaitContext> getWaitContextForTest(
    HandleT& hdl) {
  return hdl.getItemWaitContext();
}
} // namespace detail

namespace tests {
template <typename AllocatorT>
class BaseAllocatorTest : public AllocatorTest<AllocatorT> {
 private:
  const size_t kShmInfoSize = 10 * 1024 * 1024; // 10 MB
  using AllocatorTest<AllocatorT>::testShmIsNotRemoved;
  using AllocatorTest<AllocatorT>::testShmIsRemoved;
  using AllocatorTest<AllocatorT>::testInfoShmIsRemoved;

 public:
  void testStats(bool is2q) {
    const int numSlabs = 2;
    const size_t kItemSize = 100;
    const size_t nItems = 10;

    auto checkStats = [&](auto& allocator, auto& mmConfig2) {
      const size_t numBytes = allocator->getCacheMemoryStats().ramCacheSize;
      auto poolId = allocator->addPool("default", numBytes, {} /* allocSizes */,
                                       mmConfig2);
      for (unsigned int i = 0; i < nItems; ++i) {
        auto handle = util::allocateAccessible(
            *allocator, poolId, folly::to<std::string>(i), kItemSize);
        ASSERT_NE(nullptr, handle);
      }

      auto stats = allocator->getPoolStats(poolId);
      ASSERT_EQ(nItems, stats.numItems());
      ASSERT_EQ(0, stats.numEvictions());

      // thread local stats
      ASSERT_EQ(nItems, stats.numAllocAttempts());

      if (is2q) {
        // Access items in hot cache
        for (unsigned int i = 0; i < 3; ++i) {
          auto handle = allocator->find(folly::to<std::string>(nItems - i - 1));
          ASSERT_NE(nullptr, handle);
        }
        // Move items to warm cache from cold cache
        for (unsigned int i = 0; i < nItems / 3; ++i) {
          auto handle = allocator->find(folly::to<std::string>(i));
          ASSERT_NE(nullptr, handle);
        }
        // Access items in warm cache
        for (unsigned int i = 0; i < nItems / 3; ++i) {
          auto handle = allocator->find(folly::to<std::string>(i));
          ASSERT_NE(nullptr, handle);
        }
        stats = allocator->getPoolStats(poolId);
        int classId =
            allocator->getPool(poolId).getAllocationClassId(kItemSize);
        ASSERT_EQ(nItems / 3,
                  stats.cacheStats[static_cast<ClassId>(classId + 1)]
                      .containerStat.numHotAccesses);
        ASSERT_EQ(nItems / 3,
                  stats.cacheStats[static_cast<ClassId>(classId + 1)]
                      .containerStat.numColdAccesses);
        ASSERT_EQ(nItems / 3,
                  stats.cacheStats[static_cast<ClassId>(classId + 1)]
                      .containerStat.numWarmAccesses);
      }

      auto before = allocator->getGlobalCacheStats();

      for (unsigned int i = 0; i < 2 * nItems; i++) {
        auto handle = allocator->find(folly::to<std::string>(i));
      }

      auto after = allocator->getGlobalCacheStats();
      ASSERT_GT(after.numCacheGets, 0);

      // thread local stats
      ASSERT_EQ(2 * nItems, after.numCacheGets - before.numCacheGets);
    };

    typename AllocatorT::MMConfig mmConfig;
    mmConfig.lruRefreshTime = 0;

    typename AllocatorT::Config configA;
    configA.setCacheSize(numSlabs * Slab::kSize);
    // Disable slab rebalancing
    configA.enablePoolRebalancing(nullptr, std::chrono::seconds{0});
    auto a = std::make_unique<AllocatorT>(configA);

    typename AllocatorT::Config configB;
    configB.setCacheSize(numSlabs * Slab::kSize);
    // Disable slab rebalancing
    configB.enablePoolRebalancing(nullptr, std::chrono::seconds{0});
    auto b = std::make_unique<AllocatorT>(configB);

    // two different allocator types, so stats aren't conflated
    checkStats(a, mmConfig);
    checkStats(b, mmConfig);

    // destroy and recreate should start with all stats at 0
    a.reset(new AllocatorT(configA));
    checkStats(a, mmConfig);
  }

  // test all the error scenarios with respect to allocating a new key.
  void testAllocateAccessible() {
    // the eviction call back to keep track of the evicted keys.
    std::set<std::string> evictedKeys;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto k = data.item.getKey();
            evictedKeys.insert({k.data(), k.size()});
          }
        };
    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);

    // create an allocator with 100 slabs.
    config.setCacheSize(100 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize1 = numBytes / 3;
    const auto poolSize2 = numBytes - poolSize1;
    auto poolId1 = alloc.addPool("one", poolSize1);
    auto poolId2 = alloc.addPool("two", poolSize2);

    // try to allocate as much as possible and ensure that even when we run more
    // than twice the size of the cache, we are able to allocate by recycling
    // the memory. To do this, we first ensure we have a minimum number of
    // allocations across a set of sizes.
    const unsigned int nSizes = 10;
    const unsigned int keyLen = 100;
    auto sizes1 = this->getValidAllocSizes(alloc, poolId1, nSizes, keyLen);

    auto sizes2 = this->getValidAllocSizes(alloc, poolId2, nSizes, keyLen);

    // create a key and ensure that creating the same key again will not succeed
    // and original allocation is untouched.
    const auto key = this->getRandomNewKey(alloc, keyLen);
    {
      auto handle = util::allocateAccessible(alloc, poolId1, key, sizes1[3]);
      ASSERT_EQ(util::allocateAccessible(alloc, poolId1, key, sizes1[3]),
                nullptr);
    }
    ASSERT_EQ(util::allocateAccessible(alloc, poolId1, key, sizes1[3]),
              nullptr);
    // same pool, different size
    ASSERT_EQ(util::allocateAccessible(alloc, poolId1, key, sizes1[0]),
              nullptr);
    // different pool should also fail.
    ASSERT_EQ(util::allocateAccessible(alloc, poolId2, key, sizes2[3]),
              nullptr);
    ASSERT_EQ(util::allocateAccessible(alloc, poolId2, key, sizes2[0]),
              nullptr);

    ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, alloc.remove(key));

    // should be able to allocate now
    ASSERT_NE(util::allocateAccessible(alloc, poolId2, key, sizes2[0]),
              nullptr);
    ASSERT_EQ(util::allocateAccessible(alloc, poolId2, key, sizes2[0]),
              nullptr);

    // allocating with invalid key should fail.
    ASSERT_THROW(util::allocateAccessible(alloc, poolId1, "", sizes1[0]),
                 std::invalid_argument);
    ASSERT_THROW(util::allocateAccessible(alloc, poolId1,
                                          test_util::getRandomAsciiStr(256),
                                          sizes1[0]),
                 std::invalid_argument);
    // Note: we don't test for a null stringpiece with positive size as the key
    //       because folly::StringPiece now throws an exception for it

    // allocate until we evict the key.
    this->fillUpPoolUntilEvictions(alloc, poolId2, sizes2, keyLen);
    while (evictedKeys.find(key) == evictedKeys.end()) {
      this->ensureAllocsOnlyFromEvictions(alloc, poolId2, sizes2, keyLen,
                                          poolSize2);
    }

    ASSERT_EQ(alloc.find(key), nullptr);

    // should be able to allocate after evicted.
    ASSERT_NE(util::allocateAccessible(alloc, poolId1, key, sizes1[0]),
              nullptr);

    // grab a handle for the current allocation, remove the allocation and you
    // should be able to allocate a new one while still holding a handle to the
    // previously allocated one.
    auto handle = alloc.findToWrite(key);
    ASSERT_NE(handle, nullptr);
    const char magicVal1 = 'f';
    memset(handle->getMemory(), magicVal1, handle->getSize());

    // remove the existing handle.
    ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, alloc.remove(key));

    // create a new allocation with the same key.
    auto newHandle = util::allocateAccessible(alloc, poolId1, key, sizes1[2]);
    ASSERT_NE(newHandle, nullptr);
    ASSERT_NE(newHandle, handle);
    const char magicVal2 = 'g';
    memset(newHandle->getMemory(), magicVal2, newHandle->getSize());

    // handle and newHandle should have nothing in common.
    const char* m = reinterpret_cast<const char*>(handle->getMemory());
    for (unsigned int i = 0; i < handle->getSize(); i++) {
      ASSERT_EQ(*(m + i), magicVal1);
    }
  }

  // Allocate a few items of different value sizes.
  // Make sure the usable size is exactly as we requested
  void testItemSize() {
    typename AllocatorT::Config config;
    // create an allocator worth 100 slabs.
    config.setCacheSize(100 * Slab::kSize);
    AllocatorT alloc(config);

    std::set<uint32_t> allocSizes{100, 1000, 2000, 5000};
    auto pid = alloc.addPool(
        "default", alloc.getCacheMemoryStats().ramCacheSize, allocSizes);

    const uint32_t valSizeStep = 25;
    for (uint32_t i = 1; i <= 100; ++i) {
      const uint32_t valSize = valSizeStep * i;
      const auto key = folly::to<std::string>(i);

      auto it = util::allocateAccessible(alloc, pid, key, valSize);
      ASSERT_NE(nullptr, it);
      ASSERT_EQ(valSize, it->getSize());

      const auto allocSize = alloc.getUsableSize(*it) +
                             sizeof(typename AllocatorT::Item) +
                             it->getKey().size();
      ASSERT_NE(allocSizes.end(), allocSizes.find(allocSize));
    }
  }

  void testCacheCreationTime() {
    typename AllocatorT::Config config;

    std::vector<uint32_t> sizes;
    uint8_t poolId;

    const size_t nSlabs = 20;
    config.setCacheSize(nSlabs * Slab::kSize);
    const unsigned int keyLen = 100;
    auto creationTime = 0;
    config.enableCachePersistence(this->cacheDir_);

    // Test allocations. These allocations should remain after save/restore.
    // Original lru allocator
    std::vector<std::string> keys;
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      poolId = alloc.addPool("foobar", numBytes);
      sizes = this->getValidAllocSizes(alloc, poolId, nSlabs, keyLen);
      this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
      for (const auto& item : alloc) {
        auto key = item.getKey();
        keys.push_back(key.str());
      }

      creationTime = alloc.getCacheCreationTime();

      // save
      alloc.shutDown();
    }

    /* sleep override */ std::this_thread::sleep_for(std::chrono::seconds(3));

    // Restore lru allocator, check creation time.
    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);
      ASSERT_EQ(creationTime, alloc.getCacheCreationTime());
      for (auto& key : keys) {
        auto handle = alloc.find(typename AllocatorT::Key{key});
        ASSERT_NE(nullptr, handle.get());
      }

      alloc.shutDown();
    }

    // Restore lru allocator, check creation time.
    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);

      ASSERT_EQ(creationTime, alloc.getCacheCreationTime());
    }

    // create new and ensure that the creation time is not previous one.
    AllocatorT alloc(AllocatorT::SharedMemNew, config);
    ASSERT_NE(creationTime, alloc.getCacheCreationTime());
  }

  void testCCacheWarmRoll() {
    struct Key {
      int id;
      // need these two functions for key comparison
      bool operator==(const Key& other) const { return id == other.id; }
      bool isEmpty() const { return id == 0; }
      Key(int i) : id(i) {}
    } __attribute__((packed));

    typename AllocatorT::Config config;

    std::set<PoolId> poolIds;
    const int testCacheSize = 12 * 1024 * 1024; // 12 Mb
    config.setCacheSize(1024 * 1024 * 1024);    // 1G

    config.enableCachePersistence(this->cacheDir_);
    config.enableCompactCache();
    using IntValueCCache =
        typename CCacheCreator<CCacheAllocator, Key, int>::type;

    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);

      // we are making the name of compact cache, the key, the value all
      // be the same string.
      std::string prefix("prefix_");
      for (int i = 1; i <= 9; i++) {
        // adding regular pool also
        alloc.addPool(prefix + std::to_string(i), testCacheSize);
        const auto& cc = alloc.template addCompactCache<IntValueCCache>(
            std::to_string(i), testCacheSize);
        ASSERT_EQ(CCacheReturn::NOTFOUND, cc->set(Key(i), &i));
        poolIds.insert(cc->getPoolId());
      }

      alloc.shutDown();
    }

    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);
      ASSERT_EQ(poolIds, alloc.getCCachePoolIds());
      int out;
      for (int i = 1; i <= 9; i++) {
        const auto& cc = alloc.template attachCompactCache<IntValueCCache>(
            std::to_string(i));
        const auto& ci = alloc.getCompactCache(cc->getPoolId());
        ASSERT_EQ(ci.getName(), cc->getName());
        ASSERT_EQ(CCacheReturn::FOUND,
                  cc->get(Key(std::stoi(cc->getName())),
                          std::chrono::milliseconds(0), &out));
        ASSERT_EQ(std::to_string(out), cc->getName());
      }
      alloc.shutDown();
    }
  }

  // test all the error scenarios with respect to allocating a new key where it
  // is not accessible right away.
  void testAllocateInAccessible(typename AllocatorT::MMConfig& mmConfig) {
    // the eviction call back to keep track of the evicted keys.
    std::unordered_map<std::string, unsigned int> evictedKeys;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto k = data.item.getKey();
            std::string strKey = {k.data(), k.size()};
            if (evictedKeys.find(strKey) == evictedKeys.end()) {
              evictedKeys.insert(std::make_pair(strKey, 1));
            } else {
              evictedKeys[strKey]++;
            }
          }
        };
    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(50 * Slab::kSize);

    // create an allocator worth 50 slabs.
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes / 2;
    auto poolId = alloc.addPool("one", poolSize, {} /* allocSizes */, mmConfig);

    // try to allocate as much as possible and ensure that even when we run more
    // than twice the size of the cache, we are able to allocate by recycling
    // the memory. To do this, we first ensure we have a minimum number of
    // allocations across a set of sizes.
    const unsigned int nSizes = 20;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    // create a key and ensure that creating the same key again will not succeed
    // and original allocation is untouched.
    const auto key = this->getRandomNewKey(alloc, keyLen);
    {
      auto handle = alloc.allocate(poolId, key, sizes[3]);
      ASSERT_EQ(alloc.find(key), nullptr);

      // should be able to allocate with the same key again until the first one
      // is made accessible.
      ASSERT_NE(nullptr, alloc.allocate(poolId, key, sizes[3]));

      ASSERT_TRUE(alloc.insert(std::move(handle)));
      ASSERT_NE(nullptr, alloc.find(key));
      ASSERT_EQ(util::allocateAccessible(alloc, poolId, key, sizes[3]),
                nullptr);
    }

    // now that it is made accessible, we should not be able to allocate the
    // same key again.
    ASSERT_EQ(util::allocateAccessible(alloc, poolId, key, sizes[3]), nullptr);
    // same pool, different size
    ASSERT_EQ(util::allocateAccessible(alloc, poolId, key, sizes[0]), nullptr);

    // allocate until we evict the key.
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    for (unsigned int i = 0; i < 5; i++) {
      this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                          poolSize);
    }

    ASSERT_NE(evictedKeys.end(), evictedKeys.find(key));

    ASSERT_EQ(alloc.find(key), nullptr);

    const char magicVal1 = 'f';
    const char magicVal2 = 'e';
    const char magicVal3 = 'h';
    // should be able to allocate after evicted. make the new one inaccessible.
    {
      auto inAccessibleHandle = alloc.allocate(poolId, key, sizes[0]);
      ASSERT_NE(inAccessibleHandle, nullptr);
      memset(inAccessibleHandle->getMemory(), magicVal1,
             inAccessibleHandle->getSize());
    }

    // allocate a new one and make that accessible.
    {
      auto handle = alloc.allocate(poolId, key, sizes[0]);
      ASSERT_NE(handle, nullptr);

      ASSERT_EQ(alloc.find(key), nullptr);
      memset(handle->getMemory(), magicVal2, handle->getSize());
      ASSERT_TRUE(alloc.insert(handle));
    }

    evictedKeys.clear();

    // this is the one that we made accessible. hold a reference to this. it
    // should not be evicted.
    auto handle = alloc.find(key);

    for (unsigned int i = 0; i < 5; i++) {
      this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                          poolSize);
    }

    // should not be evicted since we have the handle.
    ASSERT_EQ(evictedKeys.end(), evictedKeys.find(key));

    // handle and newHandle should have nothing in common.
    const char* m = reinterpret_cast<const char*>(handle->getMemory());
    for (unsigned int i = 0; i < handle->getSize(); i++) {
      ASSERT_EQ(*(m + i), magicVal2);
    }

    // should not be able to allocate, we still hold a valid handle.
    ASSERT_EQ(util::allocateAccessible(alloc, poolId, key, sizes[0]), nullptr);

    // but we should be able to replace this item
    {
      auto newHandle = alloc.allocate(poolId, key, sizes[0]);
      memset(newHandle->getMemory(), magicVal3, newHandle->getSize());
      ASSERT_EQ(handle, alloc.insertOrReplace(newHandle));
    }
    {
      auto newHandle = alloc.find(key);
      const char* data = reinterpret_cast<const char*>(newHandle->getMemory());
      for (unsigned int i = 0; i < newHandle->getSize(); i++) {
        ASSERT_EQ(*(data + i), magicVal3);
      }
    }
  }

  // fill up the memory and test that making further allocations causes
  // evictions from the cache.
  void testEvictions() {
    // create an allocator worth 100 slabs.
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("default", numBytes);

    // try to allocate as much as possible and ensure that even when we run more
    // than twice the size of the cache, we are able to allocate by recycling
    // the memory. To do this, we first ensure we have a minimum number of
    // allocations across a set of sizes.
    const unsigned int nSizes = 10;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    // at this point, the cache should be full. we dont own any references to
    // the item and we should be able to allocate by recycling.
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                        numBytes * 5);
  }

  // fill up the memory and test that making further allocations causes
  // evictions from the cache even in the presence of long outstanding
  // handles.
  void testEvictionsWithActiveHandles() {
    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.configureChainedItems();
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("default", numBytes);

    // try to allocate as much as possible and ensure that even when we run more
    // than twice the size of the cache, we are able to allocate by recycling
    // the memory. To do this, we first ensure we have a minimum number of
    // allocations across a set of sizes.
    const unsigned int nSizes = 2;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    const std::string specialKey = "helloworld";
    // keep this handle around.
    auto handle = alloc.allocate(poolId, specialKey, sizes[0]);
    ASSERT_NE(nullptr, handle);
    alloc.insertOrReplace(handle);
    for (auto s : sizes) {
      auto chained = alloc.allocateChainedItem(handle, s);
      ASSERT_NE(nullptr, chained);
      alloc.addChainedItem(handle, std::move(chained));
    }

    // at this point, the cache should be full. we dont own any references to
    // the item and we should be able to allocate by recycling.
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                        numBytes * 5);
  }

  // test the free callback, which should only be invoked once the item is
  // removed
  // and the last handle is dropped
  void testRemovals() {
    std::vector<std::string> freedKeys;
    auto freeCb = [&freedKeys](const typename AllocatorT::RemoveCbData& data) {
      if (data.context == RemoveContext::kNormal) {
        auto key = data.item.getKey();
        freedKeys.push_back({key.data(), key.size()});
      }
    };
    typename AllocatorT::Config config;
    config.setRemoveCallback(freeCb);
    config.setCacheSize(10 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("default", numBytes);

    const std::string key = "key";
    auto handle = util::allocateAccessible(alloc, poolId, key, 1000 /* size */);

    alloc.remove(handle->getKey());

    // The item should not have been removed since we still have a handle
    ASSERT_TRUE(freedKeys.empty());

    handle.reset();

    // Exactly one item is removed and its key should be "key"
    ASSERT_EQ(1, freedKeys.size());
    ASSERT_EQ(key, freedKeys[0]);
  }

  // fill up one pool and ensure that memory can still be allocated from the
  // other pool without evictions.
  void testPools() {
    // create an allocator worth 100 slabs.
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;

    auto poolSize3 = 2 * Slab::kSize;
    auto poolSize1 = numBytes / 3;
    auto poolSize2 = numBytes - poolSize1 - poolSize3;

    auto poolId1 = alloc.addPool("one", poolSize1);
    auto poolId2 = alloc.addPool("two", poolSize2);

    ASSERT_THROW(alloc.addPool("three",
                               poolSize3,
                               std::set<uint32_t>{200, 500, 1000},
                               typename AllocatorT::MMConfig{},
                               nullptr,
                               nullptr,
                               true /* ensureProvisionable */),
                 std::invalid_argument);

    ASSERT_NO_THROW(alloc.addPool("three",
                                  poolSize3,
                                  std::set<uint32_t>{200, 500},
                                  typename AllocatorT::MMConfig{},
                                  nullptr,
                                  nullptr,
                                  true /* ensureProvisionable */));

    // try to allocate as much as possible and ensure that even when we run more
    // than twice the size of the cache, we are able to allocate by recycling
    // the memory. To do this, we first ensure we have a minimum number of
    // allocations across a set of sizes.
    const unsigned int keyLen = 100;
    const unsigned int nSizes = 10;
    auto sizes1 = this->getValidAllocSizes(alloc, poolId1, nSizes, keyLen);

    this->fillUpPoolUntilEvictions(alloc, poolId1, sizes1, keyLen);
    this->ensureAllocsOnlyFromEvictions(alloc, poolId1, sizes1, keyLen,
                                        3 * poolSize1);

    auto sizes2 = this->getValidAllocSizes(alloc, poolId2, nSizes, keyLen);
    // now try to allocate from pool2 and see that it can allocate without
    // evictions.
    this->testAllocWithoutEviction(alloc, poolId2, sizes2, keyLen);

    // pool1 should still be full and only allocate from evictions.
    this->ensureAllocsOnlyFromEvictions(alloc, poolId1, sizes1, keyLen,
                                        3 * poolSize1);
  }

  bool isConst(const void*) { return true; }
  bool isConst(void*) { return false; }

  using WriteHandle = typename AllocatorT::WriteHandle;
  using ReadHandle = typename AllocatorT::ReadHandle;
  void testReadWriteHandle() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("default", numBytes);

    {
      auto handle = util::allocateAccessible(alloc, poolId, "key", 100);
      ASSERT_NE(handle, nullptr);
      ASSERT_FALSE(isConst(handle->getMemory()));
    }

    {
      auto handle = alloc.find("key");
      ASSERT_NE(handle, nullptr);
      ASSERT_TRUE(isConst(handle->getMemory()));

      // read handle clone
      auto handle2 = handle.clone();
      ASSERT_TRUE(isConst(handle2->getMemory()));

      // upgrade a read handle to a write handle
      auto handle3 = std::move(handle).toWriteHandle();
      ASSERT_FALSE(isConst(handle3->getMemory()));
    }

    {
      auto handle = alloc.findToWrite("key");
      ASSERT_NE(handle, nullptr);
      ASSERT_FALSE(isConst(handle->getMemory()));

      // write handle clone
      auto handle2 = handle.clone();
      ASSERT_FALSE(isConst(handle2->getMemory()));

      // downgrade a write handle to a read handle
      ReadHandle handle3 = handle.clone();
      ASSERT_NE(handle3, nullptr);
      ASSERT_TRUE(isConst(handle3->getMemory()));
    }

    {
      // test upgrade function with waitContext
      ReadHandle handle = detail::createHandleWithWaitContextForTest<
          typename AllocatorT::WriteHandle, AllocatorT>(alloc);
      auto waitContext = detail::getWaitContextForTest(handle);
      // This is like doing a "clone" and setting it into wait context
      waitContext->set(alloc.find("key"));
      auto handle2 = std::move(handle).toWriteHandle();
      ASSERT_FALSE(isConst(handle2->getMemory()));
    }
  }

  void testTryAcquireSuccess() {
    using TryAcquireResult = typename AllocatorT::TryAcquireResult;

    typename AllocatorT::Config config;
    config.setCacheSize(3 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    {
      auto handle = util::allocateAccessible(alloc, poolId, "key", 100);
      ASSERT_NE(nullptr, handle);
    }

    auto itemHandle = alloc.findInternal("key");
    ASSERT_NE(nullptr, itemHandle);

    auto res = alloc.tryAcquire(itemHandle.get());
    ASSERT_EQ(TryAcquireResult::kSuccess, res.second);
    ASSERT_NE(nullptr, res.first);
    ASSERT_EQ(itemHandle.get(), res.first.get());
  }

  void testTryAcquireMoving() {
    using TryAcquireResult = typename AllocatorT::TryAcquireResult;

    typename AllocatorT::Config config;
    config.setCacheSize(3 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    {
      auto handle = util::allocateAccessible(alloc, poolId, "key", 100);
      ASSERT_NE(nullptr, handle);
    }

    auto itemHandle = alloc.findInternal("key");
    ASSERT_NE(nullptr, itemHandle);
    auto* item = itemHandle.get();
    itemHandle.reset();

    ASSERT_TRUE(item->markMoving());

    auto res = alloc.tryAcquire(item);
    ASSERT_EQ(TryAcquireResult::kMoving, res.second);
    ASSERT_EQ(nullptr, res.first);
    item->unmarkMoving();
    ASSERT_FALSE(item->isMoving());
  }

  // make some allocations without evictions and ensure that we are able to
  // fetch them.
  void testFind() {
    // the eviction call back to keep track of the evicted keys.
    std::set<std::string> evictedKeys;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto key = data.item.getKey();
            evictedKeys.insert({key.data(), key.size()});
          }
        };
    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(100 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    const unsigned int nSizes = 10;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    // all the keys that we have attemped to allocate
    std::set<std::string> keysCreated;
    const unsigned int numEvictions = 100;
    do {
      for (const auto size : sizes) {
        const auto key = this->getRandomNewKey(alloc, keyLen);
        auto handle = util::allocateAccessible(alloc, poolId, key, size);
        ASSERT_NE(handle, nullptr);
        ASSERT_EQ(handle->getKey().str(), key);
        keysCreated.insert(key);
      }
    } while (evictedKeys.size() == numEvictions);

    // for all the created keys, those that are not evicted should be available.
    for (auto key : keysCreated) {
      const bool evicted = evictedKeys.find(key) != evictedKeys.end();
      if (evicted) {
        ASSERT_EQ(alloc.find(key), nullptr);
      } else {
        auto handle = alloc.find(key);
        ASSERT_NE(handle, nullptr);
        ASSERT_EQ(handle->getKey().str(), key);
      }
    }
  }

  // make some allocations without evictions, remove them and ensure that they
  // cannot be accessed through find.
  void testRemove() {
    // the eviction call back to keep track of the evicted keys.
    std::set<std::string> evictedKeys;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto key = data.item.getKey();
            evictedKeys.insert({key.data(), key.size()});
          }
        };
    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(100 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    const unsigned int nSizes = 10;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    // all the keys that we have attemped to allocate
    std::set<std::string> keysCreated;
    const unsigned int numEvictions = 100;
    do {
      for (const auto size : sizes) {
        const auto key = this->getRandomNewKey(alloc, keyLen);
        auto handle = util::allocateAccessible(alloc, poolId, key, size);
        ASSERT_NE(handle, nullptr);
        ASSERT_EQ(handle->getKey().str(), key);
        keysCreated.insert(key);
      }
    } while (evictedKeys.size() == numEvictions);

    // for all the created keys, those that are not evicted, remove them and
    // subsequent find should not return anything. Also, the removed keys should
    // not trigger any evictionCB.
    const unsigned int prevNEvictions = evictedKeys.size();
    for (const auto& key : keysCreated) {
      const bool evicted = evictedKeys.find(key) != evictedKeys.end();
      if (evicted) {
        ASSERT_EQ(alloc.find(key), nullptr);
      } else {
        // remove the key and ensure that the eviction call back was not called.
        const auto deleted = alloc.remove(key);
        ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, deleted);
        auto handle = alloc.find(key);
        ASSERT_EQ(handle, nullptr);
        ASSERT_EQ(prevNEvictions, evictedKeys.size());
      }
    }

    const auto& removedKeys = keysCreated;
    // at this point, all the created keys are removed. trying to allocate now
    // should not call the eviction call back on the removed keys.
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                        2 * numBytes);

    // ensure that the keysCreated dont end up ever triggerring eviction cb.
    for (const auto& removedKey : removedKeys) {
      ASSERT_EQ(evictedKeys.find(removedKey), evictedKeys.end());
    }
  }

  // trigger various scenarios for item being destroyed from cache and ensure
  // that remove call back fires.
  void testRemoveCb() {
    std::vector<std::string> evictedKeys;
    std::vector<std::string> removedKeys;
    auto removeCb = [&](const typename AllocatorT::RemoveCbData& data) {
      const auto key = data.item.getKey();
      if (data.context == RemoveContext::kEviction) {
        evictedKeys.push_back({key.data(), key.size()});
      } else {
        removedKeys.push_back({key.data(), key.size()});
      }
    };
    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(100 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    const unsigned int nSizes = 10;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    // remove cb should not be called for items not inserted in cache.
    for (int i = 0; i < 100; i++) {
      auto hdl = alloc.allocate(poolId, std::to_string(i), sizes[i % nSizes]);
    }

    ASSERT_EQ(0, evictedKeys.size());
    ASSERT_EQ(0, removedKeys.size());

    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                        3 * numBytes);
    // must have evicted at least once.
    ASSERT_GE(evictedKeys.size(), sizes.size());
    auto prevNEvicions = evictedKeys.size();
    this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                        3 * numBytes);
    ASSERT_GT(evictedKeys.size(), prevNEvicions);

    for (const auto size : sizes) {
      prevNEvicions = evictedKeys.size();
      const auto key = this->getRandomNewKey(alloc, keyLen);
      auto handle = util::allocateAccessible(alloc, poolId, key, size);
      ASSERT_NE(handle, nullptr);
      ASSERT_EQ(prevNEvicions + 1, evictedKeys.size());
      // ensure that evicted key cannot be found.
      const auto& evictedKey = evictedKeys.back();
      ASSERT_EQ(alloc.find(evictedKey), nullptr);
      ASSERT_NE(alloc.find(key), nullptr);
    }
  }

  void testPreRemoveCb() {
    using Event = std::pair<RemoveContext, std::string>;
    std::vector<Event> events;
    typename AllocatorT::Config config;
    config.setPreRemoveCallback(
        [&](RemoveContext context,
            const typename AllocatorT::Item& item) noexcept {
          EXPECT_TRUE(item.isAccessible());
          events.emplace_back(context, item.getKey().str());
        });
    config.setCacheSize(10 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolId = alloc.addPool("foobar", numBytes);

    {
      auto handle = util::allocateAccessible(alloc, poolId, "normal", 100);
      ASSERT_NE(nullptr, handle);
    }
    ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, alloc.remove("normal"));
    const std::vector<Event> afterNormal{{RemoveContext::kNormal, "normal"}};
    EXPECT_EQ(afterNormal, events);

    auto expired = util::allocateAccessible(alloc, poolId, "expired", 100);
    ASSERT_NE(nullptr, expired);
    ASSERT_TRUE(expired->updateExpiryTime(util::getCurrentTimeSec() - 1));
    ASSERT_TRUE(alloc.removeIfExpired(expired));
    const std::vector<Event> afterExpired{
        {RemoveContext::kNormal, "normal"},
        {RemoveContext::kExpired, "expired"},
    };
    EXPECT_EQ(afterExpired, events);

    {
      auto original =
          util::allocateAccessible(alloc, poolId, "replacement", 100);
      ASSERT_NE(nullptr, original);
    }
    const auto eventsBeforeReplacement = events.size();
    auto replacement = alloc.allocate(poolId, "replacement", 100);
    ASSERT_NE(nullptr, replacement);
    auto replaced = alloc.insertOrReplace(replacement);
    ASSERT_NE(nullptr, replaced);
    EXPECT_EQ(eventsBeforeReplacement, events.size());
    replaced.reset();
    replacement.reset();
    ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, alloc.remove("replacement"));

    const auto eventsBeforeEvictions = events.size();
    constexpr unsigned int kKeyLen = 16;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, 3, kKeyLen);
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, kKeyLen);
    this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, kKeyLen,
                                        numBytes);
    ASSERT_GT(events.size(), eventsBeforeEvictions);
    for (auto i = eventsBeforeEvictions; i < events.size(); ++i) {
      EXPECT_EQ(RemoveContext::kEviction, events[i].first);
    }
  }

  // trigger various scenarios for item being destroyed from cache and ensure
  // that destructor call back fires.
  void testItemDestructor() {
    std::vector<std::string> evictedKeys;
    std::vector<std::string> removedKeys;
    PoolId poolId;
    auto itemDestructor = [&](const typename AllocatorT::DestructorData& data) {
      const auto key = data.item.getKey();
      if (data.context == DestructorContext::kEvictedFromRAM) {
        evictedKeys.push_back({key.data(), key.size()});
      } else {
        // kRemovedFromRAM case, no NVM in this test
        removedKeys.push_back({key.data(), key.size()});
      }
      ASSERT_EQ(poolId, data.pool);
    };
    typename AllocatorT::Config config;
    config.setItemDestructor(itemDestructor);
    config.setCacheSize(100 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize / 2;
    alloc.addPool("fake", numBytes);
    poolId = alloc.addPool("foobar", numBytes);

    const unsigned int nSizes = 10;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    // destructor cb should not be called for items not inserted in cache.
    for (int i = 0; i < 100; i++) {
      auto hdl = alloc.allocate(poolId, std::to_string(i), sizes[i % nSizes]);
    }

    ASSERT_EQ(0, evictedKeys.size());
    ASSERT_EQ(0, removedKeys.size());

    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                        3 * numBytes);
    // must have evicted at least once.
    ASSERT_GE(evictedKeys.size(), sizes.size());
    auto prevNEvicions = evictedKeys.size();
    this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                        3 * numBytes);
    ASSERT_GT(evictedKeys.size(), prevNEvicions);

    for (const auto size : sizes) {
      prevNEvicions = evictedKeys.size();
      const auto key = this->getRandomNewKey(alloc, keyLen);
      auto handle = util::allocateAccessible(alloc, poolId, key, size);
      ASSERT_NE(handle, nullptr);
      ASSERT_EQ(prevNEvicions + 1, evictedKeys.size());
      // ensure that evicted key cannot be found.
      const auto& evictedKey = evictedKeys.back();
      ASSERT_EQ(alloc.find(evictedKey), nullptr);
      ASSERT_NE(alloc.find(key), nullptr);
    }
  }

  // do slab release and make sure that moved items dont get call backs issued
  // and evicted items get remove cb executed.
  void testRemoveCbSlabReleaseMoving() {
    std::set<std::string> evictedKeys;
    std::set<std::string> removedKeys;
    auto removeCb = [&](const typename AllocatorT::RemoveCbData& data) {
      const auto key = data.item.getKey().str();
      // we should never exceute this more than once for the same key
      ASSERT_EQ(evictedKeys.find(key), evictedKeys.end());
      ASSERT_EQ(removedKeys.find(key), removedKeys.end());
      if (data.context == RemoveContext::kEviction) {
        evictedKeys.insert(key);
      } else {
        removedKeys.insert(key);
      }
    };
    typename AllocatorT::Config config;
    using Item = typename AllocatorT::Item;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(3 * Slab::kSize);

    std::set<std::string> movedKeys;
    auto moveCb = [&](const Item& oldItem, Item&, Item* /* parentPtr */) {
      movedKeys.insert(oldItem.getKey().str());
    };

    config.enableMovingOnSlabRelease(moveCb, {});

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    uint32_t size1 = 14 * 1024;
    uint32_t size2 = 16 * 1024;
    std::set<uint32_t> sizes = {size1, size2};
    auto poolId = alloc.addPool("foobar", numBytes, sizes);

    ASSERT_EQ(0, evictedKeys.size());
    ASSERT_EQ(0, removedKeys.size());

    auto& pool = alloc.getPool(poolId);
    int i = 0;
    while (evictedKeys.size() == 0) {
      if (util::allocateAccessible(alloc, poolId, std::to_string(i) + "size1",
                                   size1 - 100)) {
        i++;
      }
    }

    ASSERT_NE(0, i);

    auto hint = alloc.find(std::to_string(0) + "size1").get();
    // release the slab corresponding to one of the items.
    alloc.releaseSlab(poolId, pool.getAllocationClassId(size1),
                      SlabReleaseMode::kRebalance, hint);

    ASSERT_EQ(0, removedKeys.size());
    ASSERT_NE(0, evictedKeys.size());
    ASSERT_NE(0, movedKeys.size());
  }

  void testRemoveCbSlabRelease() {
    std::set<std::string> evictedKeys;
    std::set<std::string> removedKeys;
    auto removeCb = [&](const typename AllocatorT::RemoveCbData& data) {
      const auto key = data.item.getKey().str();
      // we should never exceute this more than once for the same key
      ASSERT_EQ(evictedKeys.find(key), evictedKeys.end());
      ASSERT_EQ(removedKeys.find(key), removedKeys.end());
      if (data.context == RemoveContext::kEviction) {
        evictedKeys.insert(key);
      } else {
        removedKeys.insert(key);
      }
    };
    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(3 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    uint32_t size1 = 14 * 1024;
    uint32_t size2 = 16 * 1024;
    std::set<uint32_t> sizes = {size1, size2};
    auto poolId = alloc.addPool("foobar", numBytes, sizes);

    ASSERT_EQ(0, evictedKeys.size());
    ASSERT_EQ(0, removedKeys.size());

    auto& pool = alloc.getPool(poolId);
    int i = 0;
    while (evictedKeys.size() == 0) {
      if (util::allocateAccessible(alloc, poolId, std::to_string(i) + "size1",
                                   size1 - 100)) {
        i++;
      }
    }

    ASSERT_NE(0, i);

    auto hint = alloc.find(std::to_string(0) + "size1").get();
    // release the slab corresponding to one of the items.
    alloc.releaseSlab(poolId, pool.getAllocationClassId(size1),
                      SlabReleaseMode::kRebalance, hint);

    ASSERT_EQ(0, removedKeys.size());
    ASSERT_NE(0, evictedKeys.size());
  }

  // create some allocation and hold the references to them. These allocations
  // should not be ever evicted. removing the keys while we have handle should
  // not mess up anything. Ensures that evict call backs are called when we hold
  // references and then later delete the items.
  void testRefCountEvictCB(typename AllocatorT::MMConfig& mmConfig) {
    std::set<std::string> evictedKeys;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto key = data.item.getKey();
            evictedKeys.insert({key.data(), key.size()});
          }
        };
    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(100 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId =
        alloc.addPool("foobar", numBytes, {} /* allocSizes */, mmConfig);

    const unsigned int nSizes = 10;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    std::vector<typename AllocatorT::WriteHandle> handles;
    size_t nHandles = 30;
    // make some allocations and hold the references to them.
    while (handles.size() != nHandles) {
      for (const auto size : sizes) {
        const auto key = this->getRandomNewKey(alloc, keyLen);
        auto handle = util::allocateAccessible(alloc, poolId, key, size);
        ASSERT_NE(handle, nullptr);

        // fetch the key into another handle and ensure that they are the same.
        auto otherHandle = alloc.find(key);
        ASSERT_NE(otherHandle, nullptr);
        ASSERT_EQ(otherHandle.get(), handle.get());

        handles.push_back(std::move(handle));
        if (handles.size() == nHandles) {
          break;
        }
      }
    }

    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                        3 * numBytes);

    // ensure that you can fetch all the items that we have a handle on.
    // TODO we might want to revisit this guarantee
    int8_t val = 1;
    for (auto& handle : handles) {
      const auto key = handle->getKey();
      // ensure key was not evicted.
      ASSERT_EQ(evictedKeys.find({key.data(), key.size()}), evictedKeys.end());
      auto newHandle = alloc.find(key);
      ASSERT_NE(newHandle, nullptr);
      ASSERT_EQ(handle.get(), newHandle.get());
      ASSERT_EQ(evictedKeys.find({key.data(), key.size()}), evictedKeys.end());
      // also write a unique value to the handle's memory
      size_t size = handle->getSize();
      memset(handle->getMemory(), val++, size);
    }

    // remove the keys to which we have handle to.
    val = 1;
    std::set<std::string> removedKeys;
    for (const auto& handle : handles) {
      // remove only 10 handles. these will not be evicted, but just returned to
      // the caller.
      if (val >= 10) {
        break;
      }

      const auto key = handle->getKey();
      removedKeys.insert({key.data(), key.size()});
      ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, alloc.remove(key));
      // should not be able to fetch the allocation once we removed it. even
      // though we have a handle to it.
      ASSERT_EQ(alloc.find(key), nullptr);

      // ensure that the handle is still valid memory.
      int8_t* m = (int8_t*)handle->getMemory();
      size_t size = handle->getSize();
      const auto expectedVal = val++;
      for (unsigned int i = 0; i < size; i++) {
        ASSERT_EQ(*(m + i), expectedVal);
      }
    }

    this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                        3 * numBytes);

    val = 1;
    std::set<std::string> handleKeys;
    std::set<std::string> foundEvicted;
    for (auto& handle : handles) {
      // ensure that the handle is still valid memory.
      int8_t* m = (int8_t*)handle->getMemory();
      size_t size = handle->getSize();
      const auto expectedVal = val++;
      for (unsigned int i = 0; i < size; i++) {
        ASSERT_EQ(*(m + i), expectedVal);
      }

      const auto temp = handle->getKey();
      const std::string key(temp.data(), temp.size());
      handleKeys.insert(key);
      // release the handle. this will release the allocations back to the cache
      // and we should be able to notice that the eviction call back gets
      // called.
      handle.reset();
    }

    // release the handles and trigger evictions. eventually all the items we
    // had handle to should be evicted.

    std::set<std::string> expectedKeys;
    std::set_difference(handleKeys.begin(), handleKeys.end(),
                        removedKeys.begin(), removedKeys.end(),
                        std::inserter(expectedKeys, expectedKeys.end()));
    while (foundEvicted.size() < expectedKeys.size()) {
      this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                          3 * numBytes,
                                          /* check = */ false);
      for (const auto& key : expectedKeys) {
        if (evictedKeys.find(key) != evictedKeys.end()) {
          foundEvicted.insert(key);
        }
      }
    }
  }

  // fill up the pool with allocations and ensure that the evictions then cycle
  // through the lru and the lru is fixed in length.
  void testTestLruLength() {
    std::set<std::string> evictedKeys;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto key = data.item.getKey();
            evictedKeys.insert({key.data(), key.size()});
          }
        };
    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(10 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    const unsigned int nSizes = 5;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    this->testLruLength(alloc, poolId, sizes, keyLen, evictedKeys);
  }

  void testReaperShutDown(
      typename AllocatorT::Config::MemoryTierConfigs cfgs = {}) {
    const size_t nSlabs = 20;
    const size_t size = nSlabs * Slab::kSize;

    typename AllocatorT::Config config;
    config.setCacheSize(size);
    // set up a small cache so that we can actually reap faster.
    config.setAccessConfig({8, 8});
    config.enableCachePersistence(this->cacheDir_);
    config.enableItemReaperInBackground(std::chrono::seconds(1), {});
    if (cfgs.size()) {
      config.configureMemoryTiers(cfgs);
    }
    std::vector<typename AllocatorT::Key> keys;
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      auto poolId =
          alloc.addPool("foo", alloc.getCacheMemoryStats().ramCacheSize);
      unsigned int ttlSecs = 1;
      util::allocateAccessible(alloc, poolId, "hello", 1000, ttlSecs);
      while (alloc.getReaperStats().numTraversals < 5) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      // save
      ASSERT_NO_THROW(alloc.shutDown());
    }
  }

  // test cleanup logic when cache is not attached, but directory is present
  void testCacheCleanupDirExists() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.enableCachePersistence(this->cacheDir_);
    config.configureChainedItems();

    // Create a new cache allocator and save it properly
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
    }

    testShmIsNotRemoved(config);
    ASSERT_TRUE(util::getStatIfExists(config.cacheDir, nullptr));
    AllocatorT::cleanupStrayShmSegments(config.cacheDir, config.usePosixShm);
    testShmIsRemoved(config);
    ASSERT_TRUE(util::getStatIfExists(config.cacheDir, nullptr));

    // Restoring should fail
    ASSERT_THROW(AllocatorT(AllocatorT::SharedMemAttach, config),
                 std::invalid_argument);
  }

  // test cleanup logic when cache is attached. cleanup should be no-op.
  void testCacheCleanupAttached() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.enableCachePersistence(this->cacheDir_);
    config.configureChainedItems();
    config.reaperInterval = std::chrono::seconds(0);

    // Disable slab rebalancing
    config.enablePoolRebalancing(nullptr, std::chrono::seconds{0});

    // Destroy singletons before we fork() below
    folly::SingletonVault::singleton()->destroyInstances();

    // Create a new cache allocator and save it properly
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      ASSERT_TRUE(util::getStatIfExists(config.cacheDir, nullptr));
      // at this state, the shm is still attached (we have lock on metadata
      // file). This hackery is needed to ensure that we execute the code that
      // calls flock on the cache dir metadata from a separate process. flocks
      // only work exclusive across process and not within the same process.
      auto pid = fork();
      ASSERT_GE(pid, 0);
      if (pid == 0) {
        ASSERT_FALSE(AllocatorT::cleanupStrayShmSegments(config.cacheDir,
                                                         config.usePosixShm));
        // use a return code to signal we succeeded the assertion
        exit(5);
      }

      int status = 0;
      waitpid(pid, &status, 0);
      ASSERT_EQ(WEXITSTATUS(status), 5);
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
    }

    testShmIsNotRemoved(config);

    // Restoring should succeed.
    ASSERT_NO_THROW({
      auto alloc = AllocatorT(AllocatorT::SharedMemAttach, config);
      alloc.shutDown();
    });

    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);

      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
      auto pid = fork();
      ASSERT_GE(pid, 0);
      if (pid == 0) {
        ASSERT_TRUE(AllocatorT::cleanupStrayShmSegments(config.cacheDir,
                                                        config.usePosixShm));
        // use a return code to signal we succeeded the assertion
        exit(5);
      }

      int status = 0;
      waitpid(pid, &status, 0);
      ASSERT_EQ(WEXITSTATUS(status), 5);
    }

    // Re-enable folly::Singleton creation
    folly::SingletonVault::singleton()->reenableInstances();
  }

  // test cleanup when directory was removed and cache is not attached.
  void testCacheCleanupDirRemoved() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.enableCachePersistence(this->cacheDir_);
    config.configureChainedItems();

    // Create a new cache allocator and save it properly
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      ASSERT_TRUE(util::getStatIfExists(config.cacheDir, nullptr));
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
    }

    testShmIsNotRemoved(config);
    util::removePath(config.cacheDir);
    ASSERT_FALSE(util::getStatIfExists(config.cacheDir, nullptr));
    AllocatorT::cleanupStrayShmSegments(config.cacheDir, config.usePosixShm);
    testShmIsRemoved(config);

    // Restoring should fail
    ASSERT_THROW(AllocatorT(AllocatorT::SharedMemAttach, config),
                 std::exception);
  }

  void testShutDownWithActiveHandles() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.enableCachePersistence(this->cacheDir_);
    config.configureChainedItems();

    // Create a new cache allocator and save it properly
    AllocatorT alloc(AllocatorT::SharedMemNew, config);
    auto pid =
        alloc.addPool("foobar", alloc.getCacheMemoryStats().ramCacheSize);

    auto handle = util::allocateAccessible(alloc, pid, "key", 10);
    ASSERT_NE(nullptr, handle);
    ASSERT_EQ(AllocatorT::ShutDownStatus::kFailed, alloc.shutDown());
    handle.reset();
    ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
  }

  void testDuplicateShutDowns() {
    // This tests that when multiple threads enter shutdown simultaneously, only
    // 1 makes it to the non-reentrant (i.e., saves cache) part. To do this, we
    // create a worker that blocks until released and get two threads into
    // shutDown().  We then release the worker, which releases the two
    // shutDown() callers.

    using ShutDownStatus = typename AllocatorT::ShutDownStatus;
    // Parks a mover thread on first invocation until released
    class BlockingMoverStrategy : public BackgroundMoverStrategy {
     public:
      std::vector<size_t> calculateBatchSizes(
          const CacheBase&, std::vector<MemoryDescriptorType>) override {
        if (!parked_.exchange(true)) {
          entered_.post();
          release_.wait();
        }
        return {};
      }

      folly::Baton<> entered_; // posted once the worker is parked
      folly::Baton<> release_; // post to let the worker (and shutDown) proceed

     private:
      std::atomic<bool> parked_{false};
    };

    // We use CacheAllocator::triggerFastShutdown_ to detect when shutDown()
    // callers have made it into the function, so set enableFastShutdown
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.enableCachePersistence(this->cacheDir_);
    config.enableFastShutdown = true;

    AllocatorT alloc(AllocatorT::SharedMemNew, config);
    alloc.addPool("foobar", alloc.getCacheMemoryStats().ramCacheSize);

    // Start a background that blocks until released; block the test thread
    // until we're sure the worker is where we want it
    auto strategy = std::make_shared<BlockingMoverStrategy>();
    ASSERT_TRUE(alloc.startNewBackgroundEvictor(std::chrono::milliseconds{100},
                                                strategy, 1 /* threads */));
    strategy->entered_.wait();

    // Spin until a caller has entered shutDown(), then clear the flag so the
    // next caller's entry is observable too
    auto waitForEntry = [&] {
      while (!alloc.triggerFastShutdown_.load()) {
        std::this_thread::yield();
      }
      alloc.triggerFastShutdown_.store(false);
    };

    ShutDownStatus results[2] = {ShutDownStatus::kFailed,
                                 ShutDownStatus::kFailed};
    std::thread ta([&] { results[0] = alloc.shutDown(); });
    waitForEntry();
    std::thread tb([&] { results[1] = alloc.shutDown(); });
    waitForEntry();

    // Both callers are in shutDown() - unblock the worker
    strategy->release_.post();

    ta.join();
    tb.join();

    EXPECT_TRUE((results[0] == ShutDownStatus::kSuccess &&
                 results[1] == ShutDownStatus::kSkipped) ||
                (results[0] == ShutDownStatus::kSkipped &&
                 results[1] == ShutDownStatus::kSuccess))
        << "expected exactly one kSuccess and one kSkipped, got "
        << static_cast<int>(results[0]) << " and "
        << static_cast<int>(results[1]);
  }

  void testAttachDetachOnExit() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.enableCachePersistence(this->cacheDir_);
    config.configureChainedItems();

    // Create a new cache allocator and save it properly
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
    }

    testShmIsNotRemoved(config);

    // Restoring a saved cache allocator should succeed. Save it again
    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);
      testInfoShmIsRemoved(config);
      // Restoring multiple cache allocators from the same saved state should
      // fail
      ASSERT_THROW(AllocatorT(AllocatorT::SharedMemAttach, config),
                   std::system_error);
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());

      testShmIsNotRemoved(config);

      // Restoring after shutDown but before destructing previous one should
      // succeed
      AllocatorT anotherAlloc(AllocatorT::SharedMemAttach, config);
      testInfoShmIsRemoved(config);
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, anotherAlloc.shutDown());
    }

    testShmIsNotRemoved(config);

    // Restoring again should still succeed. Exit without saving
    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);
      testInfoShmIsRemoved(config);
    }

    testShmIsRemoved(config);

    // Restoring a unsaved cache allocator should fail
    ASSERT_THROW(AllocatorT(AllocatorT::SharedMemAttach, config),
                 std::invalid_argument);

    testShmIsRemoved(config);

    // Create a new cache allocator and save it properly
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
    }

    testShmIsNotRemoved(config);

    // Cleanup on restart
    AllocatorT::ShmManager::cleanup(this->cacheDir_, config.usePosixShm);

    testShmIsRemoved(config);

    // Restoring after cleanup on restart should fail
    ASSERT_THROW(AllocatorT(AllocatorT::SharedMemAttach, config),
                 std::invalid_argument);

    testShmIsRemoved(config);

    // Create a new cache allocator, save it but cleanup on exit
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
      SCOPE_EXIT {
        AllocatorT::ShmManager::cleanup(this->cacheDir_, config.usePosixShm);
      };
    }

    testShmIsRemoved(config);

    // Restoring after cleanup on exit should fail
    ASSERT_THROW(AllocatorT(AllocatorT::SharedMemAttach, config),
                 std::invalid_argument);

    testShmIsRemoved(config);

    // create one and shut it down
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
    }

    testShmIsNotRemoved(config);

    {
      // create another new one.
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      // old one's info shm must be cleared
      testInfoShmIsRemoved(config);
    }
  }

  void testDropFile() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.enableCachePersistence(this->cacheDir_);
    config.configureChainedItems();

    // Create a new cache allocator and save it properly
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
    }

    const std::string fName = this->cacheDir_ + "/ColdRoll";
    auto touchFile = [&]() { std::ofstream s(fName); };
    testShmIsNotRemoved(config);
    touchFile();

    // now create a drop file to indicate that attach should fail.
    ASSERT_TRUE(util::getStatIfExists(fName, nullptr));

    // Restoring a saved cache allocator should fail
    {
      ASSERT_THROW(AllocatorT alloc(AllocatorT::SharedMemAttach, config),
                   std::exception);
      testShmIsNotRemoved(config);
      ASSERT_FALSE(util::getStatIfExists(fName, nullptr));
      ASSERT_NO_THROW(AllocatorT alloc(AllocatorT::SharedMemNew, config));
    }

    // touch file and trying to create a new one should work fine.
    touchFile();
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
    }
    testShmIsNotRemoved(config);
    {
      ASSERT_NO_THROW(AllocatorT alloc(AllocatorT::SharedMemAttach, config));
    }
  }

  void testSerialization() {
    std::set<std::string> evictedKeys;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto key = data.item.getKey();
            evictedKeys.insert({key.data(), key.size()});
          }
        };

    const size_t nSlabs = 20;
    const size_t size = nSlabs * Slab::kSize;
    const unsigned int nSizes = 5;
    const unsigned int keyLen = 100;

    std::vector<uint32_t> sizes;
    uint8_t poolId;

    // Test allocations. These allocations should remain after save/restore.
    // Original lru allocator
    typename AllocatorT::Config config;
    config.setCacheSize(size);
    config.enableCachePersistence(this->cacheDir_);
    std::vector<std::string> keys;
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      poolId = alloc.addPool("foobar", numBytes);
      sizes = this->getValidAllocSizes(alloc, poolId, nSlabs, keyLen);
      this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
      for (const auto& item : alloc) {
        auto key = item.getKey();
        keys.push_back(key.str());
      }

      // save
      alloc.shutDown();
    }

    testShmIsNotRemoved(config);
    // Restored lru allocator
    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);
      for (auto& key : keys) {
        auto handle = alloc.find(typename AllocatorT::Key{key});
        ASSERT_NE(nullptr, handle.get());
      }
    }

    testShmIsRemoved(config);
    // Test LRU eviction and length before and after save/restore
    // Original lru allocator
    typename AllocatorT::Config config2;
    config2.setCacheSize(size);
    config2.setRemoveCallback(removeCb);
    config2.enableCachePersistence(this->cacheDir_);
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config2);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      poolId = alloc.addPool("foobar", numBytes);

      sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

      this->testLruLength(alloc, poolId, sizes, keyLen, evictedKeys);

      // save
      alloc.shutDown();
    }
    evictedKeys.clear();

    testShmIsNotRemoved(config2);
    // Restored lru allocator
    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config2);
      this->testLruLength(alloc, poolId, sizes, keyLen, evictedKeys);
    }

    testShmIsRemoved(config2);
  }

  void testSerializationMMConfig() {
    typename AllocatorT::Config config;
    config.setCacheSize(20 * Slab::kSize);
    config.enableCachePersistence(this->cacheDir_);
    double ratio = 0.2;

    // start allocator
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      {
        typename AllocatorT::MMConfig mmConfig;
        mmConfig.lruRefreshRatio = ratio;
        auto pid =
            alloc.addPool("foobar", numBytes, /* allocSizes = */ {}, mmConfig);
        auto handle = util::allocateAccessible(alloc, pid, "key", 10);
        ASSERT_NE(nullptr, handle);
        auto& container = alloc.getMMContainer(*handle);
        EXPECT_DOUBLE_EQ(ratio, container.getConfig().lruRefreshRatio);
      }

      // save
      alloc.shutDown();
    }
    testShmIsNotRemoved(config);

    // restore allocator and check lruRefreshRatio
    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);
      auto handle = alloc.find("key");
      ASSERT_NE(nullptr, handle);
      auto& container = alloc.getMMContainer(*handle);
      EXPECT_DOUBLE_EQ(ratio, container.getConfig().lruRefreshRatio);
    }
    testShmIsRemoved(config);
  }

  void testSerializationMMConfigExtra() {
    typename AllocatorT::Config config;
    config.setCacheSize(20 * Slab::kSize);
    config.enableCachePersistence(this->cacheDir_);
    config.enableTailHitsTracking();
    const auto allocSize = 256;
    const std::set<uint32_t> allocSizes = {allocSize};
    const auto tailSize = Slab::kSize / allocSize;

    // start allocator
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      {
        typename AllocatorT::MMConfig mmConfig;
        auto pid = alloc.addPool("pool", numBytes, allocSizes, mmConfig);
        auto handle = util::allocateAccessible(alloc, pid, "key", 10);
        ASSERT_NE(nullptr, handle);
        auto& container = alloc.getMMContainer(*handle);
        EXPECT_DOUBLE_EQ(tailSize, container.getConfig().tailSize);
      }

      // save
      alloc.shutDown();
    }
    testShmIsNotRemoved(config);

    // restore allocator and check lruRefreshRatio
    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);
      auto handle = alloc.find("key");
      ASSERT_NE(nullptr, handle);
      auto& container = alloc.getMMContainer(*handle);
      EXPECT_DOUBLE_EQ(tailSize, container.getConfig().tailSize);
    }
    testShmIsRemoved(config);
  }

  // Test temporary shared memory mode which is enabled when memory
  // monitoring is enabled.
  void testShmTemporary() {
    std::string tempCacheDir;

    typename AllocatorT::Config config;
    {
      const size_t nSlabs = 20;
      config.setCacheSize(nSlabs * Slab::kSize);
      // Enable memory monitoring by setting monitoring mode and interval.
      MemoryMonitor::Config memConfig;
      memConfig.mode = MemoryMonitor::FreeMemory;
      memConfig.maxAdvisePercentPerIter = 1;
      memConfig.maxReclaimPercentPerIter = 1;
      memConfig.maxAdvisePercent = 10;
      memConfig.lowerLimitGB = 10;
      memConfig.upperLimitGB = 20;
      config.enableMemoryMonitor(std::chrono::seconds{2}, memConfig);

      AllocatorT alloc(config);
      ASSERT_TRUE(alloc.isOnShm());
      tempCacheDir = alloc.tempShm_->tempCacheDir_;
      ASSERT_TRUE(util::pathExists(tempCacheDir));

      std::vector<std::string> keys;
      const unsigned int keyLen = 100;
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      auto poolId = alloc.addPool("foobar", numBytes);
      auto sizes = this->getValidAllocSizes(alloc, poolId, nSlabs, keyLen);
      this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
      for (const auto& item : alloc) {
        auto key = item.getKey();
        keys.push_back(key.str());
      }

      for (auto& key : keys) {
        auto handle = alloc.find(typename AllocatorT::Key{key});
        ASSERT_NE(nullptr, handle.get());
      }
    }
    ASSERT_FALSE(AllocatorT::ShmManager::segmentExists(
        tempCacheDir, detail::kTempShmCacheName.str(), config.usePosixShm));
    ASSERT_FALSE(util::pathExists(tempCacheDir));
  }

  void testAccessContainerMemoryStats() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.setAccessConfig({12, 6});
    config.configureChainedItems({8, 4});
    AllocatorT alloc(config);

    using CompressedPtr = typename AllocatorT::Item::CompressedPtrType;
    const auto memStats = alloc.getCacheMemoryStats();
    EXPECT_EQ((1u << 12) * sizeof(CompressedPtr), memStats.accessContainerSize);
    EXPECT_EQ((1u << 8) * sizeof(CompressedPtr),
              memStats.chainedItemAccessContainerSize);
  }

  // make some allocations and access them and record explicitly the time it was
  // accessed. Ensure that the items that are evicted are descending in order of
  // time. To ensure the lru property, lets only allocate objects of fixed size.
  void testLruRecordAccess() {
    // TODO (sathya) we currently dont limit the number of times an item gets
    // updated in lru, which makes this test easier. When we introduce a config
    // for that, ensure that this test still defaults to no restriction on lru
    // updates.
    using LruConfig = typename AllocatorT::MMConfig;
    std::set<std::string> evictedKeys;
    std::unordered_map<std::string, unsigned int> evictedTimeStamps;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto key = data.item.getKey();
            evictedKeys.insert({key.data(), key.size()});
          }
        };

    LruConfig lruConfig;
    lruConfig.lruRefreshTime = 0;

    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId =
        alloc.addPool("foobar", numBytes, {} /* allocSizes */, lruConfig);

    // for sake of simplicity, we want to have only two allocation classes.
    const unsigned int nSizes = 2;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    const auto classId = alloc.getPool(poolId).getAllocationClassId(
        AllocatorT::Item::getRequiredSize(keyLen, sizes[0]));
    const size_t lruLength =
        alloc.getPoolStats(poolId).cacheStats.at(classId).numItems();
    ASSERT_GT(lruLength, 0);

    const unsigned int nHotItems =
        std::max(lruLength / 5, static_cast<unsigned long>(1));
    std::set<std::string> hotKeys;
    for (unsigned int i = 0; i < nHotItems; i++) {
      const auto key = this->getRandomNewKey(alloc, keyLen);
      hotKeys.insert(key);
      auto handle = util::allocateAccessible(alloc, poolId, key, sizes[0]);
      ASSERT_NE(handle, nullptr);
    }

    const unsigned int nOtherKeys = lruLength - nHotItems;
    std::set<std::string> otherKeys;
    for (unsigned int i = 0; i < nOtherKeys; i++) {
      const auto key = this->getRandomNewKey(alloc, keyLen);
      otherKeys.insert(key);
      auto handle = util::allocateAccessible(alloc, poolId, key, sizes[0]);
      ASSERT_NE(handle, nullptr);
    }

    for (const auto& key : hotKeys) {
      ASSERT_EQ(evictedKeys.find(key), evictedKeys.end())
          << "hot key evicted during setup before its first access";
    }

    auto accessHotKeys = [&alloc, &hotKeys]() {
      for (const auto& key : hotKeys) {
        auto handle = alloc.find(key);
        ASSERT_NE(handle, nullptr);
      }
      // dummy return statement.
      SUCCEED();
    };

    // while accessing the hot keys randomly, create the twice the lrulength
    // keys. this should keep the hot keys in the lru and evict the other.
    for (unsigned int i = 0; i < 2 * lruLength; i++) {
      if (i % 2 == 0) {
        accessHotKeys();
      }
      const auto key = this->getRandomNewKey(alloc, keyLen);
      auto handle = util::allocateAccessible(alloc, poolId, key, sizes[0]);
      ASSERT_NE(handle, nullptr);

      // ensure that none of the hot keys got evicted.
      for (const auto& hotKey : hotKeys) {
        ASSERT_EQ(evictedKeys.find(hotKey), evictedKeys.end()) << hotKey;
      }
    }

    for (const auto& key : otherKeys) {
      ASSERT_NE(evictedKeys.find(key), evictedKeys.end());
    }
  }

  void testApplyAll() {
    std::set<std::string> evictedKeys;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto key = data.item.getKey();
            evictedKeys.insert({key.data(), key.size()});
          }
        };
    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(10 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    // for sake of simplicity, we want to have only two allocation classes.
    const unsigned int nSizes = 2;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    // this->getValidAllocSizes above would create some allocations to figure
    // out
    // valid sizes. Lets visit them and make sure that they are removed.
    std::set<std::string> visitedKeys;
    using Item = typename AllocatorT::Item;
    auto func = [&visitedKeys](const Item& item) {
      visitedKeys.insert(item.getKey().str());
    };

    for (const auto& item : alloc) {
      func(item);
    }

    for (const auto& key : visitedKeys) {
      ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, alloc.remove(key));
      ASSERT_FALSE(alloc.find(key));
    }

    visitedKeys.clear();

    for (const auto& item : alloc) {
      func(item);
    }
    ASSERT_EQ(0, visitedKeys.size());

    // now try to allocate some keys and ensure that they are all visited.
    evictedKeys.clear();
    std::set<std::string> allocatedKeys;
    unsigned int toAllocate = 100;
    for (unsigned int i = 0; i < toAllocate; i++) {
      const auto size =
          *(sizes.begin() + folly::Random::rand32() % sizes.size());
      const auto key = this->getRandomNewKey(alloc, keyLen);
      ASSERT_EQ(alloc.find(key), nullptr);
      auto handle = util::allocateAccessible(alloc, poolId, key, size);
      ASSERT_TRUE(handle);
      if (handle) {
        allocatedKeys.insert(key);
      }
    };

    ASSERT_EQ(allocatedKeys.size(), toAllocate);
    // remove the keys that are evicted. We should not be visiting these.
    for (const auto& key : evictedKeys) {
      allocatedKeys.erase(key);
    }

    for (const auto& item : alloc) {
      func(item);
    }
    ASSERT_EQ(allocatedKeys.size(), visitedKeys.size());
    ASSERT_EQ(allocatedKeys, visitedKeys);
  }

  void testIterateAndRemoveWithKey() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    // for sake of simplicity, we want to have only two allocation classes.
    const unsigned int nSizes = 5;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    unsigned int visited = 0;
    for (auto& item : alloc) {
      auto key = item.getKey();
      ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, alloc.remove(key));
      ASSERT_FALSE(alloc.find(key));
      visited++;
    }
    ASSERT_GT(visited, 0);

    visited = 0;
    for (auto& item : alloc) {
      (void)item;
      visited++;
    }

    visited = 0;
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    // visit keys and delete them through the iterator. They should all result
    // in not found when the keys are deleted beforehand
    for (auto it = alloc.begin(); it != alloc.end(); ++it) {
      auto key = it->getKey();
      ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, alloc.remove(key));
      ASSERT_FALSE(alloc.find(it->getKey()));

      ASSERT_EQ(AllocatorT::RemoveRes::kNotFoundInRam, alloc.remove(key));
      ASSERT_EQ(key, it->getKey());
      visited++;
    }
    ASSERT_GT(visited, 0);

    visited = 0;
    for (auto& item : alloc) {
      (void)item;
      visited++;
    }
  }

  void testIterateAndRemoveWithIter() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    // for sake of simplicity, we want to have only two allocation classes.
    const unsigned int nSizes = 2;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);

    // visit keys and delete them through the iterator. They should all succeed.
    unsigned int visited = 0;
    for (auto it = alloc.begin(); it != alloc.end(); ++it) {
      ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, alloc.remove(it));
      ASSERT_FALSE(alloc.find(it->getKey()));
      visited++;
    }
    ASSERT_GT(visited, 0);

    visited = 0;
    for (auto& item : alloc) {
      (void)item;
      visited++;
    }

    ASSERT_EQ(0, visited);

    visited = 0;
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    // visit keys and delete them through the iterator. They should all result
    // in not found when the keys are deleted beforehand
    for (auto it = alloc.begin(); it != alloc.end(); ++it) {
      auto key = it->getKey();
      ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, alloc.remove(key));
      ASSERT_FALSE(alloc.find(it->getKey()));

      ASSERT_EQ(AllocatorT::RemoveRes::kNotFoundInRam, alloc.remove(it));
      ASSERT_EQ(key, it->getKey());
      visited++;
    }
    ASSERT_GT(visited, 0);

    visited = 0;
    for (auto& item : alloc) {
      (void)item;
      visited++;
    }
  }

  // Verify that LockGroupAccessIterator visits every accessible item exactly
  // once. Exercises CacheAllocator::beginLockGroup() / endLockGroup() and the
  // tryHandleMaker / findByKey wiring inside CacheAllocator.
  void testIterateLockGroup() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    std::set<uint32_t> allocSizes{1024};
    auto poolId = alloc.addPool("foobar", numBytes, allocSizes);

    const unsigned int numItems = 100;
    const uint32_t itemSize = 100;
    std::set<std::string> expectedKeys;
    for (unsigned int i = 0; i < numItems; ++i) {
      const std::string key = "key_" + folly::to<std::string>(i);
      auto handle = util::allocateAccessible(alloc, poolId, key, itemSize);
      ASSERT_NE(nullptr, handle);
      expectedKeys.insert(key);
    }

    std::set<std::string> visitedKeys;
    for (auto it = alloc.beginLockGroup(); it != alloc.endLockGroup(); ++it) {
      const bool inserted = visitedKeys.insert(it->getKey().str()).second;
      ASSERT_TRUE(inserted);
    }
    ASSERT_EQ(expectedKeys, visitedKeys);
  }

  void testIterateWithEvictions() {
    std::set<std::string> evictedKeys;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto key = data.item.getKey();
            evictedKeys.insert({key.data(), key.size()});
          }
        };
    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(50 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    // for sake of simplicity, we want to have only two allocation classes. this
    // and the fact that we have about 50MB of memory ensures that we can evict
    // while we are iterating.
    const unsigned int nSizes = 2;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    for (auto& item : alloc) {
      ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, alloc.remove(item.getKey()));
    }

    // now try to allocate some keys and ensure that they are all visited.
    evictedKeys.clear();
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);

    auto iter = alloc.begin();
    const auto iterEnd = alloc.end();
    for (unsigned int i = 0; i < 10; i++) {
      if (iter == iterEnd) {
        break;
      }
      ++iter;
      auto key = iter->getKey().str();
      this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                          numBytes);
      // the iterator key should not be evicted since there is an active
      // iterator that is holding a reference to it.
      ASSERT_EQ(evictedKeys.end(), evictedKeys.find(key));
    }
  }

  void testIOBufItemHandle() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    std::set<uint32_t> allocSizes{10000};
    auto poolId = alloc.addPool("foobar", numBytes, allocSizes);

    const std::string keyPrefix = "key";
    const uint32_t itemSize = 100;
    const uint8_t numItems = 100;

    for (uint8_t i = 0; i < numItems; ++i) {
      const std::string key = keyPrefix + folly::to<std::string>(i);
      auto item = util::allocateAccessible(alloc, poolId, key, itemSize);
      uint8_t* data = reinterpret_cast<uint8_t*>(item->getMemory());
      data[0] = i;
    }

    // We should be able to view data through IOBuf
    for (uint32_t i = 0; i < numItems; ++i) {
      const std::string key = keyPrefix + folly::to<std::string>(i);
      auto item = alloc.find(key);
      auto ioBuf = alloc.convertToIOBuf(std::move(item));
      ASSERT_EQ(100, ioBuf.length());
      const uint8_t* data = ioBuf.data();
      ASSERT_EQ(i, data[0]);
    }

    // Holding an IOBuf will keep the item's ref count and thus prevent it from
    // beiing evicted
    {
      const uint32_t i = folly::Random::rand32(numItems - 1);
      const std::string key = keyPrefix + folly::to<std::string>(i);
      auto itemHandle = alloc.find(key);
      auto itemIOBuf = alloc.convertToIOBuf(std::move(itemHandle));
      ASSERT_EQ(i, itemIOBuf.data()[0]);

      const unsigned int nSizes = 2;
      const unsigned int keyLen = 100;
      const auto sizes =
          this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);
      this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
      this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                          numBytes);

      // This one key should still be here
      auto itemHandle2 = alloc.find(key);
      auto itemIOBuf2 = alloc.convertToIOBuf(std::move(itemHandle2));

      ASSERT_EQ(i, itemIOBuf.data()[0]);
      ASSERT_EQ(itemIOBuf.data(), itemIOBuf2.data());
    }
  }

  void testIOBufSharedItemHandleWithChainedItems() {
    std::atomic<int> itemsRemoved{0};
    auto removeCb = [&itemsRemoved](const typename AllocatorT::RemoveCbData&) {
      ++itemsRemoved;
    };

    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    config.setRemoveCallback(removeCb);
    config.setRefcountThresholdForConvertingToIOBuf(0);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    std::set<uint32_t> allocSizes{10000};
    auto poolId = alloc.addPool("foobar", numBytes, allocSizes);

    {
      auto parent = util::allocateAccessible(alloc, poolId, "parent", 100);
      *reinterpret_cast<char*>(parent->getMemory()) = 'p';
      for (unsigned int i = 0; i < 1000; ++i) {
        auto chained = alloc.allocateChainedItem(parent, 100);
        ASSERT_EQ(2, alloc.getNumActiveHandles());

        *reinterpret_cast<int*>(chained->getMemory()) = i;
        alloc.addChainedItem(parent, std::move(chained));
      }
      ASSERT_EQ(1, alloc.getNumActiveHandles());
    }

    auto ioBuf = std::make_unique<folly::IOBuf>(
        alloc.convertToIOBuf(alloc.find("parent")));
    ASSERT_EQ(1, alloc.getNumActiveHandles());

    // Confirm we have all the chained item iobufs
    {
      ASSERT_EQ(*ioBuf->data(), 'p');
      ASSERT_EQ(ioBuf->length(), 100);

      auto* curr = ioBuf->next();
      for (unsigned int i = 0; i < 1000; ++i) {
        ASSERT_NE(nullptr, curr);
        ASSERT_EQ(*reinterpret_cast<const int*>(curr->data()), i);
        curr = curr->next();
        ASSERT_EQ(curr->length(), 100);
      }
    }

    // Remove the parent item and start popping IOBufs
    // the remove callback should not be triggered until
    // all ioBufs have been dropped
    alloc.remove("parent");
    for (unsigned int i = 0; i < 1000 + 1; ++i) {
      ASSERT_NE(nullptr, ioBuf);
      ASSERT_EQ(0, itemsRemoved);
      ioBuf = ioBuf->pop();
    }
    ASSERT_EQ(1, itemsRemoved);
    ASSERT_EQ(0, alloc.getNumActiveHandles());
  }

  // Make sure we can convert a chain of cached items into a chain of IOBufs
  void testIOBufItemHandleForChainedItems() {
    std::atomic<int> itemsRemoved{0};
    auto removeCb = [&itemsRemoved](const typename AllocatorT::RemoveCbData&) {
      ++itemsRemoved;
    };

    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    config.setRemoveCallback(removeCb);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    std::set<uint32_t> allocSizes{10000};
    auto poolId = alloc.addPool("foobar", numBytes, allocSizes);

    {
      auto parent = util::allocateAccessible(alloc, poolId, "parent", 100);
      *reinterpret_cast<char*>(parent->getMemory()) = 'p';
      for (unsigned int i = 0; i < 1000; ++i) {
        auto chained = alloc.allocateChainedItem(parent, 100);
        ASSERT_EQ(2, alloc.getNumActiveHandles());

        *reinterpret_cast<int*>(chained->getMemory()) = i;
        alloc.addChainedItem(parent, std::move(chained));
      }
      ASSERT_EQ(1, alloc.getNumActiveHandles());
    }

    auto ioBuf = std::make_unique<folly::IOBuf>(
        alloc.convertToIOBuf(alloc.find("parent")));
    ASSERT_EQ(1001, alloc.getNumActiveHandles());

    // Confirm we have all the chained item iobufs
    {
      ASSERT_EQ(*ioBuf->data(), 'p');
      ASSERT_EQ(ioBuf->length(), 100);

      auto* curr = ioBuf->next();
      for (unsigned int i = 0; i < 1000; ++i) {
        ASSERT_NE(nullptr, curr);
        ASSERT_EQ(*reinterpret_cast<const int*>(curr->data()), i);
        curr = curr->next();
        ASSERT_EQ(curr->length(), 100);
      }
    }

    // Remove the parent item and start popping IOBufs
    // the remove callback should not be triggered until
    // all ioBufs have been dropped
    alloc.remove("parent");
    for (unsigned int i = 0; i < 1000 + 1; ++i) {
      ASSERT_NE(nullptr, ioBuf);
      ASSERT_EQ(0, itemsRemoved);
      ioBuf = ioBuf->pop();
    }
    ASSERT_EQ(1, itemsRemoved);
    ASSERT_EQ(0, alloc.getNumActiveHandles());
  }

  void verifyIOBuf(const folly::IOBuf& bufs, const folly::IOBuf& original) {
    ASSERT_EQ(bufs.countChainElements(), original.countChainElements());
    auto itrA = bufs.begin();
    auto itrB = original.begin();
    while (itrA != bufs.end()) {
      ASSERT_EQ(*itrA, *itrB);
      ++itrA;
      ++itrB;
    }
  }

  void testIOBufWrapOnItem() {
    std::atomic<int> itemsRemoved{0};
    auto removeCb = [&itemsRemoved](const typename AllocatorT::RemoveCbData&) {
      ++itemsRemoved;
    };

    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    config.setRemoveCallback(removeCb);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    std::set<uint32_t> allocSizes{10000};
    auto poolId = alloc.addPool("foobar", numBytes, allocSizes);

    {
      auto parent = util::allocateAccessible(alloc, poolId, "parent", 100);
      *reinterpret_cast<char*>(parent->getMemory()) = 'p';
      for (unsigned int i = 0; i < 1000; ++i) {
        auto chained = alloc.allocateChainedItem(parent, 100);
        ASSERT_EQ(2, alloc.getNumActiveHandles());

        *reinterpret_cast<int*>(chained->getMemory()) = i;
        alloc.addChainedItem(parent, std::move(chained));
      }
      ASSERT_EQ(1, alloc.getNumActiveHandles());
    }

    auto handle = alloc.find("parent");
    auto ioBuf = alloc.wrapAsIOBuf(*handle);
    ASSERT_EQ(1, alloc.getNumActiveHandles());

    // Confirm we have all the chained item iobufs
    {
      ASSERT_EQ(*ioBuf.data(), 'p');
      ASSERT_EQ(ioBuf.length(), 100);

      auto* curr = ioBuf.next();
      for (unsigned int i = 0; i < 1000; ++i) {
        ASSERT_NE(nullptr, curr);
        ASSERT_EQ(*reinterpret_cast<const int*>(curr->data()), i);
        curr = curr->next();
        ASSERT_EQ(curr->length(), 100);
      }
    }

    auto ioBuf2 = alloc.convertToIOBuf(alloc.find("parent"));
    verifyIOBuf(ioBuf, ioBuf2);
  }

  void testIOBufChainCaching() {
    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(100 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    char baseChar = 'A';
    // create some IOBufs
    auto makeIOBufs = [&](unsigned int num) -> std::unique_ptr<folly::IOBuf> {
      std::unique_ptr<folly::IOBuf> bufs;
      for (unsigned int i = 0; i < num; i++) {
        const size_t size = folly::Random::rand32(10, 20);
        auto buf = folly::IOBuf::create(size);
        // fill it with data.
        std::memset(buf->writableData(), baseChar + i, size);
        buf->append(size);
        if (bufs) {
          bufs->appendChain(std::move(buf));
        } else {
          bufs = std::move(buf);
        }
      }
      return bufs;
    };

    // pick random chain length of 10 - 50
    auto bufs = makeIOBufs(folly::Random::rand32(10, 50));
    const auto key = this->getRandomNewKey(alloc, 100);
    ASSERT_TRUE(facebook::cachelib::util::insertIOBufInCache(alloc, poolId, key,
                                                             *bufs));
    auto handle = alloc.find(key);
    verifyIOBuf(alloc.convertToIOBuf(std::move(handle)), *bufs);

    // invalid IOBuf chain
    auto invalidBufs = makeIOBufs(3);
    const uint32_t invalidSize = Slab::kSize + 100;
    auto invalidBuf = folly::IOBuf::create(invalidSize);
    invalidBuf->append(invalidSize);
    invalidBufs->appendChain(std::move(invalidBuf));

    const auto invalidAllocKey = this->getRandomNewKey(alloc, 100);

    const auto numAllocsBefore = alloc.getPoolStats(poolId).numActiveAllocs();
    const auto numItemsInCacheBefore = alloc.getPoolStats(poolId).numItems();
    // buffer with invalid size should fail and this should not increase the
    // number of allocs
    ASSERT_THROW(facebook::cachelib::util::insertIOBufInCache(
                     alloc, poolId, invalidAllocKey, *invalidBufs),
                 std::invalid_argument);

    const auto numAllocsAfter = alloc.getPoolStats(poolId).numActiveAllocs();
    const auto numItemsInCacheAfter = alloc.getPoolStats(poolId).numItems();
    ASSERT_EQ(numAllocsBefore, numAllocsAfter);
    ASSERT_EQ(numItemsInCacheBefore, numItemsInCacheAfter);
  }

  void testChainedAllocsIteration() {
    std::atomic<int> itemsRemoved{0};
    auto removeCb = [&itemsRemoved](const typename AllocatorT::RemoveCbData&) {
      ++itemsRemoved;
    };

    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    config.setRemoveCallback(removeCb);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    std::set<uint32_t> allocSizes{10000};
    auto poolId = alloc.addPool("foobar", numBytes, allocSizes);

    const size_t nChainedAllocs = folly::Random::rand32(100, 1000);

    {
      auto parent = util::allocateAccessible(alloc, poolId, "parent", 100);
      *reinterpret_cast<char*>(parent->getMemory()) = 'p';
      for (unsigned int i = 0; i < nChainedAllocs; ++i) {
        auto chained = alloc.allocateChainedItem(parent, 100);
        ASSERT_EQ(2, alloc.getNumActiveHandles());

        *reinterpret_cast<int*>(chained->getMemory()) = i;
        alloc.addChainedItem(parent, std::move(chained));
      }
      ASSERT_EQ(1, alloc.getNumActiveHandles());
    }

    auto handle = alloc.find("parent");
    auto chainedAllocs = alloc.viewAsChainedAllocs(handle);
    ASSERT_EQ(2, alloc.getNumActiveHandles());

    ASSERT_EQ(nChainedAllocs, chainedAllocs.computeChainLength());
    ASSERT_EQ(alloc.find("parent").get(), &chainedAllocs.getParentItem());

    // Confirm we have all the chained item iobufs
    int i = nChainedAllocs - 1;
    for (auto& c : chainedAllocs.getChain()) {
      ASSERT_TRUE(isConst(c.getMemory()));
      ASSERT_EQ(*reinterpret_cast<const int*>(c.getMemory()), i);
      ASSERT_EQ(&c, chainedAllocs.getNthInChain(nChainedAllocs - i - 1));
      i--;
    }
  }

  void testGetTotalAllocSize() {
    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    auto parent = util::allocateAccessible(alloc, poolId, "parent", 100);
    ASSERT_NE(nullptr, parent);
    const int nChainedAllocs = 3;
    for (int i = 0; i < nChainedAllocs; ++i) {
      auto chained = alloc.allocateChainedItem(parent, 100 * (i + 1));
      ASSERT_NE(nullptr, chained);
      alloc.addChainedItem(parent, std::move(chained));
    }

    auto handle = alloc.find("parent");
    auto chainedAllocs = alloc.viewAsChainedAllocs(handle);
    auto chain = chainedAllocs.getChain();

    // Independently sum the allocation-class size across the whole chain.
    uint64_t expected = alloc.getAllocInfo(handle->getMemory()).allocSize;
    for (const auto& c : chain) {
      expected += alloc.getAllocInfo(c.getMemory()).allocSize;
    }
    // The chained parts must contribute beyond the parent's own block.
    ASSERT_GT(expected, alloc.getAllocInfo(handle->getMemory()).allocSize);

    ASSERT_EQ(expected, alloc.getTotalAllocSize(*handle, chain));
  }

  // create a chain of allocations, replace the allocation and ensure that the
  // order is preserved (multithreaded version).
  void testChainedAllocsReplaceInChainMultithread() {
    std::atomic<int> itemsRemoved{0};
    auto removeCb = [&itemsRemoved](const typename AllocatorT::RemoveCbData&) {
      ++itemsRemoved;
    };

    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    config.setRemoveCallback(removeCb);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    std::set<uint32_t> allocSizes{10000};
    auto poolId = alloc.addPool("foobar", numBytes, allocSizes);

    const size_t nChainedAllocs = 10;

    {
      auto parent = util::allocateAccessible(alloc, poolId, "parent", 100);

      for (unsigned int i = 0; i < nChainedAllocs; ++i) {
        auto chained = alloc.allocateChainedItem(parent, 100);
        alloc.addChainedItem(parent, std::move(chained));
      }
    }

    std::vector<const void*> expectedMemory(nChainedAllocs);
    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < nChainedAllocs; ++i) {
      threads.emplace_back([&expectedMemory, &alloc, i]() {
        auto parent = alloc.findToWrite("parent");
        auto* nThChain =
            alloc.viewAsWritableChainedAllocs(parent).getNthInChain(i);
        ASSERT_NE(nullptr, nThChain);
        auto newAlloc = alloc.allocateChainedItem(parent, 50);

        expectedMemory[i] = newAlloc->getMemory();
        ASSERT_EQ(
            nThChain,
            alloc.replaceChainedItem(*nThChain, std::move(newAlloc), *parent));
      });
    }

    for (auto& t : threads) {
      t.join();
    }

    auto chainedAllocs = alloc.viewAsChainedAllocs(alloc.find("parent"));
    int i = 0;
    for (const auto& c : chainedAllocs.getChain()) {
      ASSERT_EQ(c.getMemory(), expectedMemory[i++]);
    }
  }

  // tests case that correct parent item is acquired after move
  void testChainedItemParentAcquireAfterMoveLoop() {
    // create an allocator worth 250 slabs
    // first slab is for overhead, second is parent class
    // third is chained item 1 and rest are for new chained item alloc
    // to move to.
    std::unique_ptr<AllocatorT> alloc;
    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(250 * Slab::kSize);

    const std::set<uint32_t> allocSizes = {1024, 2048};
    auto sizes = std::vector<uint32_t>{500, 1500};
    std::atomic<uint64_t> numMoves{0};
    std::atomic<uint64_t> numReplaces{0};
    PoolId pid;

    using Item = typename AllocatorT::Item;
    config.enableMovingOnSlabRelease([&](Item& oldItem, Item& newItem,
                                         Item* parentPtr) {
      assert(oldItem.getSize() == newItem.getSize());
      assert(oldItem.isChainedItem());
      std::memcpy(newItem.getMemory(), oldItem.getMemory(), oldItem.getSize());
      folly::Latch latch(1);
      auto insertThread = std::make_unique<std::thread>([&]() {
        ASSERT_NO_THROW({
          auto parentReplacement =
              alloc->allocate(pid, parentPtr->getKey(), sizes[0]);
          Item* parentCopy = parentPtr;
          latch.count_down();
          while (parentCopy->isMoving())
            ;
          alloc->insertOrReplace(parentReplacement);
          ++numReplaces;
        });
      });
      insertThread->detach();
      latch.wait();
      ++numMoves;
    });

    alloc = std::make_unique<AllocatorT>(config);

    const size_t numBytes = alloc->getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;
    pid = alloc->addPool("one", poolSize, allocSizes);

    auto allocFn = [&](std::string keyPrefix, std::vector<uint32_t> sizes) {
      for (unsigned int loop = 0; loop < 20; ++loop) {
        for (unsigned int i = 0; i < 2048; ++i) {
          const auto key = keyPrefix + folly::to<std::string>(loop) + "_" +
                           folly::to<std::string>(i);
          auto itemHandle =
              util::allocateAccessible(*alloc, pid, key, sizes[0]);
          auto childItem = alloc->allocateChainedItem(itemHandle, sizes[1]);
          ASSERT_NE(nullptr, childItem);

          alloc->addChainedItem(itemHandle, std::move(childItem));
        }
      }
    };
    allocFn(std::string{"yolo"}, sizes);

    ClassId cid = static_cast<ClassId>(1);
    for (int i = 0; i < 20; i++) {
      alloc->releaseSlab(pid, cid, SlabReleaseMode::kRebalance);
    }
    while (alloc->getSlabReleaseStats().numSlabReleaseForRebalance < 20) {
      sleep(1);
    }
    // for ASSERT_EXIT
    exit(0);
  }

  // create a chain of allocations, replace the allocation and ensure that the
  // order is preserved.
  void testChainedAllocsReplaceInChain() {
    std::atomic<int> itemsRemoved{0};
    auto removeCb = [&itemsRemoved](const typename AllocatorT::RemoveCbData&) {
      ++itemsRemoved;
    };

    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    config.setRemoveCallback(removeCb);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    std::set<uint32_t> allocSizes{10000};
    auto poolId = alloc.addPool("foobar", numBytes, allocSizes);

    const size_t nChainedAllocs = folly::Random::rand32(10, 11);

    auto otherParent =
        util::allocateAccessible(alloc, poolId, "parent-other", 10);
    {
      auto otherChainedHdl = alloc.allocateChainedItem(otherParent, 220);
      alloc.addChainedItem(otherParent, std::move(otherChainedHdl));
    }

    {
      auto parent = util::allocateAccessible(alloc, poolId, "parent", 100);
      *reinterpret_cast<char*>(parent->getMemory()) = 'p';

      {
        auto otherChainedHdl = alloc.allocateChainedItem(otherParent, 220);
        auto chained = alloc.allocateChainedItem(parent, 100);
        auto chained1 = alloc.allocateChainedItem(parent, 100);
        ASSERT_THROW(
            alloc.replaceChainedItem(*chained, std::move(chained1), *parent),
            std::invalid_argument);
        ASSERT_THROW(alloc.replaceChainedItem(
                         *chained, std::move(otherChainedHdl), *parent),
                     std::invalid_argument);
        otherChainedHdl = alloc.allocateChainedItem(otherParent, 220);
        ASSERT_THROW(alloc.replaceChainedItem(*otherChainedHdl,
                                              std::move(chained), *otherParent),
                     std::invalid_argument);
      }

      for (unsigned int i = 0; i < nChainedAllocs; ++i) {
        auto chained = alloc.allocateChainedItem(parent, 100);
        *reinterpret_cast<int*>(chained->getMemory()) = i;
        alloc.addChainedItem(parent, std::move(chained));
      }
    }

    {
      auto handle = alloc.find("parent");
      auto chainedAllocs = alloc.viewAsChainedAllocs(handle);

      ASSERT_EQ(nChainedAllocs, chainedAllocs.computeChainLength());

      // Confirm we have all the chained item iobufs
      int i = nChainedAllocs;
      for (const auto& c : chainedAllocs.getChain()) {
        ASSERT_EQ(*reinterpret_cast<const int*>(c.getMemory()), --i);
      }
    }

    std::vector<const void*> expectedMemory;
    for (unsigned int i = 0; i < nChainedAllocs; i++) {
      auto parent = alloc.findToWrite("parent");
      auto* nThChain =
          alloc.viewAsWritableChainedAllocs(parent).getNthInChain(i);
      ASSERT_NE(nullptr, nThChain);
      auto newAlloc = alloc.allocateChainedItem(parent, 50);

      expectedMemory.push_back(newAlloc->getMemory());
      ASSERT_EQ(
          nThChain,
          alloc.replaceChainedItem(*nThChain, std::move(newAlloc), *parent));
    }

    auto chainedAllocs = alloc.viewAsChainedAllocs(alloc.find("parent"));
    int i = 0;
    for (const auto& c : chainedAllocs.getChain()) {
      ASSERT_EQ(c.getMemory(), expectedMemory[i++]);
    }
  }

  void testChainedAllocsTransfer() {
    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    std::set<uint32_t> allocSizes{100, 1000, 10000};
    auto poolId = alloc.addPool("foobar", numBytes, allocSizes);

    const size_t nChainedAllocs = folly::Random::rand32(100, 200);

    {
      auto parent = util::allocateAccessible(alloc, poolId, "parent", 100);
      *reinterpret_cast<char*>(parent->getMemory()) = 'p';
      for (unsigned int i = 0; i < nChainedAllocs; ++i) {
        auto chained =
            alloc.allocateChainedItem(parent, folly::Random::rand32(100, 9000));
        *reinterpret_cast<int*>(chained->getMemory()) = i;
        alloc.addChainedItem(parent, std::move(chained));
      }
      ASSERT_TRUE(parent->hasChainedItem());
    }

    auto originalParent = alloc.findToWrite("parent");

    // parent of different size
    auto newParent = alloc.allocate(poolId, "parent", 1000);

    typename AllocatorT::WriteHandle invalidParent = {};
    ASSERT_THROW(alloc.transferChainAndReplace(invalidParent, newParent),
                 std::invalid_argument);

    ASSERT_FALSE(newParent->hasChainedItem());
    ASSERT_FALSE(newParent->isAccessible());
    ASSERT_FALSE(newParent->isInMMContainer());
    ASSERT_TRUE(originalParent->hasChainedItem());
    alloc.transferChainAndReplace(originalParent, newParent);

    ASSERT_THROW(alloc.transferChainAndReplace(newParent, invalidParent),
                 std::invalid_argument);

    ASSERT_FALSE(originalParent->isAccessible());
    ASSERT_FALSE(originalParent->isInMMContainer());
    ASSERT_FALSE(originalParent->hasChainedItem());
    ASSERT_TRUE(newParent->hasChainedItem());

    ASSERT_EQ(alloc.find("parent"), newParent);

    auto chainedAllocs = alloc.viewAsChainedAllocs(newParent);

    int i = nChainedAllocs;
    // Confirm we have all the chained item iobufs
    for (const auto& c : chainedAllocs.getChain()) {
      ASSERT_EQ(*reinterpret_cast<const int*>(c.getMemory()), --i);
    }
  }

  // tests the global active item handle count, which sums across all threads.
  // However, we are NOT instantiating multiple threads. Please refer to
  // testTLHandleTracking() for the the actual test that spawns multiple
  // threads.
  void testHandleTracking() {
    typename AllocatorT::Config config{};
    config.setCacheSize(100 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    const std::string keyPrefix = "key";
    const uint32_t itemSize = 100;
    const uint32_t numItems = 100;
    std::vector<std::string> keys;

    for (uint32_t i = 0; i < numItems; ++i) {
      const std::string key = keyPrefix + folly::to<std::string>(i);
      auto item = util::allocateAccessible(alloc, poolId, key, itemSize);
      keys.push_back(key);
    }

    std::vector<typename AllocatorT::ReadHandle> handles;
    // We should be able to view data through IOBuf
    for (const auto& key : keys) {
      for (unsigned int i = 0; i < 5; i++) {
        handles.push_back(alloc.find(key));
      }
    }

    ASSERT_EQ(handles.size(), alloc.getNumActiveHandles());

    // normal handle acquisition
    for (auto& handle : handles) {
      auto before = alloc.getNumActiveHandles();
      handle.reset();
      ASSERT_EQ(before - 1, alloc.getNumActiveHandles());
    }

    ASSERT_EQ(0, alloc.getNumActiveHandles());

    // use iterators
    for (auto& it : alloc) {
      (void)it;
      ASSERT_NE(0, alloc.getNumActiveHandles());
    }

    // must drop to 0 after you drop the iterator.
    ASSERT_EQ(0, alloc.getNumActiveHandles());

    std::vector<folly::IOBuf> iobufs;
    for (const auto& key : keys) {
      const auto before = alloc.getNumActiveHandles();
      auto handle = alloc.find(key);
      ASSERT_NE(nullptr, handle);
      ASSERT_EQ(before + 1, alloc.getNumActiveHandles());
      iobufs.push_back(alloc.convertToIOBuf(std::move(handle)));
      // should be the same.
      ASSERT_EQ(before + 1, alloc.getNumActiveHandles());
    }

    ASSERT_EQ(iobufs.size(), alloc.getNumActiveHandles());
    iobufs.clear();
    // should drop down to 0 after releasing the iobufs.
    ASSERT_EQ(0, alloc.getNumActiveHandles());

    // exceptions should not bump up the active handles.
    const auto maxRefs = AllocatorT::Item::getRefcountMax();
    for (unsigned int i = 0; i < maxRefs; i++) {
      handles.push_back(alloc.find(keys[0]));
    }

    ASSERT_EQ(maxRefs, alloc.getNumActiveHandles());
    ASSERT_THROW(alloc.find(keys[0]), exception::RefcountOverflow);
    ASSERT_EQ(maxRefs, alloc.getNumActiveHandles());
    handles.clear();
    ASSERT_EQ(0, alloc.getNumActiveHandles());

    // perform some operation to trigger evicitons and ensure that handle
    // tracking is not broken.
    const size_t nSizes = 20;
    const size_t keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    // active refcount must be 0
    ASSERT_EQ(0, alloc.getNumActiveHandles());
  }

  // TODO: ideally we should use a real NvmCache for this. However,
  //       we're not able to inject mocks into Navy from CacheAllocator
  //       layer and thus there's no good way to explicitly control
  //       how we can enqueue onReady callback or convert to semi-future
  //       while an async get is in-flight. Instead, we explicitly
  //       create a waitcontext in this test and act as if we're populating
  //       it from NvmCache. This test should be kept in-sync with how
  //       create WriteHandle from NvmCache to ensure the behavior stays
  //       consistent.
  void testHandleTrackingAsync() {
    typename AllocatorT::Config config{};
    config.setCacheSize(100 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    // Create a item in ram cache to fulfill the "async item"
    {
      auto ramHdl = alloc.allocate(poolId, "test", 100);
      alloc.insertOrReplace(ramHdl);
    }
    ASSERT_EQ(0, alloc.getNumActiveHandles());
    ASSERT_EQ(0, alloc.getHandleCountForThread());

    // Test waiting for a handle
    {
      auto hdl = detail::createHandleWithWaitContextForTest<
          typename AllocatorT::WriteHandle, AllocatorT>(alloc);
      auto waitContext = detail::getWaitContextForTest(hdl);
      ASSERT_EQ(0, alloc.getNumActiveHandles());
      ASSERT_EQ(0, alloc.getHandleCountForThread());

      // This is like doing a "clone" and setting it into wait context
      waitContext->set(alloc.find("test"));
      ASSERT_EQ(0, alloc.getNumActiveHandles());
      ASSERT_EQ(0, alloc.getHandleCountForThread());

      ASSERT_TRUE(hdl.isReady());
    }
    ASSERT_EQ(0, alloc.getNumActiveHandles());
    ASSERT_EQ(0, alloc.getHandleCountForThread());

    // Test converting to SemiFuture
    {
      auto hdl = detail::createHandleWithWaitContextForTest<
          typename AllocatorT::WriteHandle, AllocatorT>(alloc);

      auto waitContext = detail::getWaitContextForTest(hdl);
      ASSERT_EQ(0, alloc.getNumActiveHandles());
      ASSERT_EQ(0, alloc.getHandleCountForThread());

      auto semiFuture = std::move(hdl).toSemiFuture().deferValue(
          [](typename AllocatorT::ReadHandle) { return true; });

      // This is like doing a "clone" and setting it into wait context
      waitContext->set(alloc.find("test"));
      ASSERT_EQ(0, alloc.getNumActiveHandles());
      ASSERT_EQ(0, alloc.getHandleCountForThread());

      ASSERT_TRUE(std::move(semiFuture).get());
    }
    ASSERT_EQ(0, alloc.getNumActiveHandles());
    ASSERT_EQ(0, alloc.getHandleCountForThread());
  }

  void testRefcountOverflow() {
    typename AllocatorT::Config config{};
    config.setCacheSize(100 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    const std::string key = "key";
    const uint32_t itemSize = 100;
    util::allocateAccessible(alloc, poolId, key, itemSize);

    // exhaust the handles.
    std::vector<typename AllocatorT::ReadHandle> handles;

    try {
      while (true) {
        auto handle = alloc.find(key);
        handles.emplace_back(std::move(handle));
      }
    } catch (const exception::RefcountOverflow&) {
    }

    ASSERT_EQ(1, alloc.getGlobalCacheStats().numRefcountOverflow);

    for (int i = 0; i < 10; i++) {
      try {
        alloc.find(key);
        ASSERT_TRUE(false);
      } catch (const exception::RefcountOverflow&) {
      }
    }

    ASSERT_EQ(11, alloc.getGlobalCacheStats().numRefcountOverflow);

    util::allocateAccessible(alloc, poolId, "newkey", 1900);

    // iterating while being out of ref counts should skip over items that
    // cant yield a refcount.
    int visited = 0;
    ASSERT_NO_THROW({
      for (auto& it : alloc) {
        (void)it;
        ++visited;
      }
    });

    ASSERT_EQ(1, visited);
    ASSERT_EQ(12, alloc.getGlobalCacheStats().numRefcountOverflow);

    // replacing an existing key  while being out of refcounts should result in
    // the item being replaced and the replacement item in a consistent state.
    auto h = alloc.allocate(poolId, key, itemSize);
    ASSERT_THROW(alloc.insertOrReplace(h), exception::RefcountOverflow);
    ASSERT_EQ(13, alloc.getGlobalCacheStats().numRefcountOverflow);

    ASSERT_FALSE(h->isInMMContainer());
    ASSERT_FALSE(h->isAccessible());

    auto freeAllocs = alloc.getPoolStats(poolId).numFreeAllocs();

    ASSERT_TRUE(handles.back()->isInMMContainer());
    ASSERT_TRUE(handles.back()->isAccessible());

    // release all handles
    handles.clear();

    // the item we were holding handles to is still in cache.
    ASSERT_EQ(freeAllocs, alloc.getPoolStats(poolId).numFreeAllocs());

    h.reset();
    // item we allocated must be now freed to the allocator.
    ASSERT_EQ(freeAllocs + 1, alloc.getPoolStats(poolId).numFreeAllocs());
  }

  // tests the API getNumActiveHandlesForThisThread specifically by
  // instantiating threads and ensuring that the handle count from one thread is
  // not reflected on other, but is included in the global count
  void testTLHandleTracking() {
    typename AllocatorT::Config config{};
    config.setCacheSize(100 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    const std::string keyPrefix = "key";
    const uint32_t itemSize = 100;
    const uint32_t numItems = 100;
    std::vector<std::string> keys;

    for (uint32_t i = 0; i < numItems; ++i) {
      const std::string key = keyPrefix + folly::to<std::string>(i);
      auto item = util::allocateAccessible(alloc, poolId, key, itemSize);
      keys.push_back(key);
    }

    std::vector<typename AllocatorT::ReadHandle> handles;
    // We should be able to view data through IOBuf
    for (const auto& key : keys) {
      for (unsigned int i = 0; i < 5; i++) {
        handles.push_back(alloc.find(key));
      }
    }

    ASSERT_EQ(handles.size(), alloc.getHandleCountForThread());

    // normal handle acquisition
    for (auto& handle : handles) {
      auto before = alloc.getHandleCountForThread();
      handle.reset();
      ASSERT_EQ(before - 1, alloc.getHandleCountForThread());
    }

    ASSERT_EQ(0, alloc.getHandleCountForThread());

    // use iterators
    for (auto& it : alloc) {
      (void)it;
      ASSERT_NE(0, alloc.getHandleCountForThread());
    }

    // must drop to 0 after you drop the iterator.
    ASSERT_EQ(0, alloc.getHandleCountForThread());

    std::vector<folly::IOBuf> iobufs;
    for (const auto& key : keys) {
      const auto before = alloc.getHandleCountForThread();
      auto handle = alloc.find(key);
      ASSERT_NE(nullptr, handle);
      ASSERT_EQ(before + 1, alloc.getHandleCountForThread());
      iobufs.push_back(alloc.convertToIOBuf(std::move(handle)));
      // should be the same.
      ASSERT_EQ(before + 1, alloc.getHandleCountForThread());
    }

    ASSERT_EQ(iobufs.size(), alloc.getHandleCountForThread());
    iobufs.clear();
    // should drop down to 0 after releasing the iobufs.
    ASSERT_EQ(0, alloc.getHandleCountForThread());

    // perform some operation to trigger evicitons and ensure that handle
    // tracking is not broken.
    const size_t nSizes = 20;
    const size_t keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
    // active refcount must be 0
    ASSERT_EQ(0, alloc.getHandleCountForThread());

    // poor mans baton to co-ordinate between the threads
    std::atomic<bool> checked1{false};
    std::atomic<bool> checked2{false};

    handles.clear();
    std::thread otherThread([&]() {
      for (const auto& key : keys) {
        handles.push_back(alloc.find(key));
      }
      ASSERT_EQ(handles.size(), alloc.getHandleCountForThread());
      checked1 = true;

      while (!checked2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      handles.clear();
      ASSERT_EQ(0, alloc.getNumActiveHandles());
    });

    // this thread should not have any active handles
    while (!checked1) {
      ASSERT_EQ(0, alloc.getHandleCountForThread());
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // global one should reflect both the threads
    ASSERT_EQ(handles.size(), alloc.getNumActiveHandles());
    checked2 = true;
    otherThread.join();
  }

  // ensure that when we call allocate and get an exception, we dont leak any
  // memory. We do so by keeping track of the number of active allocations and
  // ensuring that the number stays the same.
  void testAllocException() {
    uint64_t numEvictions = 0;
    auto removeCb =
        [&numEvictions](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            ++numEvictions;
          }
        };
    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(100 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    const unsigned int nSizes = 20;
    const unsigned int keyLen = 255;

    auto numActiveAllocs = [](const PoolStats& stats) -> uint64_t {
      const auto& allocStats = stats.mpStats;
      uint64_t numAllocs = 0;
      for (const auto& kv : allocStats.acStats) {
        numAllocs += kv.second.activeAllocs;
      }
      return numAllocs;
    };

    auto numFreeAllocs = [](const PoolStats& stats) -> uint64_t {
      const auto& allocStats = stats.mpStats;
      uint64_t numFree = 0;
      for (const auto& kv : allocStats.acStats) {
        numFree += kv.second.freeAllocs;
      }
      return numFree;
    };

    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);

    // empty the cache.
    for (const auto& it : alloc) {
      alloc.remove(it.getKey());
    }

    // try to do some random allocations across the pool with invalid key sizes.
    // this will cause the allocation to fail.
    unsigned int numFailed = 0;
    const unsigned int nTries = 1000;
    auto allocateWithExceptions = [&]() {
      numFailed = 0;
      const uint32_t maxInvalidKeyLen = 1000;
      for (unsigned int i = 0; i < nTries; i++) {
        for (auto size : sizes) {
          const auto maxlen = std::min(maxInvalidKeyLen, size - 1);
          const unsigned int invalidKeyLen = folly::Random::rand32(256, maxlen);
          const auto key = test_util::getRandomAsciiStr(invalidKeyLen);
          // to ensure that we do the same allocations as what is currently in
          // the cache, we ensure that whatever random invalid key size we pick,
          // we subtract that from previous alloc sizes we computed.
          ASSERT_THROW(
              alloc.allocate(poolId, key, size - invalidKeyLen + keyLen),
              std::invalid_argument);
          ++numFailed;
        }
      }
    };

    {
      // without any evictions
      const auto beforeStats = alloc.getPoolStats(poolId);
      ASSERT_EQ(0, numActiveAllocs(beforeStats)) << sizes.size();
      allocateWithExceptions();
      const auto afterStats = alloc.getPoolStats(poolId);

      ASSERT_EQ(nTries * sizes.size(), numFailed);
      ASSERT_EQ(0, numEvictions);
      ASSERT_EQ(numActiveAllocs(beforeStats), numActiveAllocs(afterStats));
      ASSERT_EQ(numFreeAllocs(beforeStats), numFreeAllocs(afterStats));
    }

    {
      // with evictions
      this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
      ASSERT_GT(numEvictions, sizes.size());

      const auto beforeStats = alloc.getPoolStats(poolId);
      const auto beforeEvictions = numEvictions;
      allocateWithExceptions();
      const auto afterStats = alloc.getPoolStats(poolId);
      const auto currEvictions = numEvictions - beforeEvictions;

      ASSERT_GE(numFailed, 0);
      ASSERT_GT(currEvictions, 0);
      ASSERT_EQ(numActiveAllocs(beforeStats) - currEvictions,
                numActiveAllocs(afterStats));
      ASSERT_EQ(numFreeAllocs(beforeStats) + currEvictions,
                numFreeAllocs(afterStats));
    }

    // test that the cache stats evictions matches with ours.
    const auto stats = alloc.getPoolStats(poolId);
    unsigned int numEvictionsFromStat = 0;
    for (const auto& kv : stats.cacheStats) {
      numEvictionsFromStat += kv.second.numEvictions();
    }
    ASSERT_EQ(numEvictions, numEvictionsFromStat);
  }

  // Fill up the allocator with items of the same size.
  // Release a slab. Afterwards, ensure the allocator only has enough space
  // for allocate for the same number of items as evicted by releasing the
  // slab. Any more allocation should result in new items being evicted.
  void testRebalancing() {
    std::set<std::string> evictedKeys;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto key = data.item.getKey();
            evictedKeys.insert({key.data(), key.size()});
          }
        };
    typename AllocatorT::Config config;
    config.setRemoveCallback(removeCb);
    config.setCacheSize(10 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    // fill up the pool with items of the same size
    const int keyLen = 100;
    const int valLen = 1000;
    const std::vector<uint32_t> sizes = {valLen};
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);

    uint8_t classId = -1;
    // find the AC that fits our values
    auto poolStats = alloc.getPoolStats(poolId);
    for (uint8_t i = 0; i < poolStats.mpStats.acStats.size(); ++i) {
      if (poolStats.mpStats.acStats[i].activeAllocs) {
        classId = i;
      }
    }
    // clear all the keys that have been evicted during the fill up process
    evictedKeys.clear();

    alloc.releaseSlab(poolId, classId, SlabReleaseMode::kRebalance);
    auto numItemsReleased = evictedKeys.size();
    ASSERT_GT(numItemsReleased, 0);
    ASSERT_EQ(alloc.getSlabReleaseStats().numSlabReleaseForRebalance, 1);

    // since when we release a slab, we treat it as evictions, we should
    // no longer be able to find any of the evicted keys in the cache
    for (const auto& key : evictedKeys) {
      ASSERT_EQ(nullptr, alloc.find(key));
    }

    evictedKeys.clear();
    for (unsigned int i = 0; i < numItemsReleased; ++i) {
      const auto key = this->getRandomNewKey(alloc, keyLen);
      auto handle = util::allocateAccessible(alloc, poolId, key, valLen);
      ASSERT_NE(nullptr, handle);
    }
    // no keys shold have been evicted from the process above
    ASSERT_EQ(0, evictedKeys.size());
    {
      const auto key = this->getRandomNewKey(alloc, keyLen);
      auto handle = util::allocateAccessible(alloc, poolId, key, valLen);
      ASSERT_NE(nullptr, handle);
    }
    // exaclty one key should've been evicted to allocate the item above
    ASSERT_EQ(1, evictedKeys.size());
  }

  // Test releasing a slab while one item from the slab being released
  // is held by the user. Eventually the user drops the item handle.
  // The slab release should not finish before the item handle is dropped.
  void testRebalancingWithAllocationsHeldByUser() {
    std::set<std::string> evictedKeys;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto key = data.item.getKey();
            evictedKeys.insert({key.data(), key.size()});
          }
        };

    typename AllocatorT::Config config{};
    config.setRemoveCallback(removeCb);
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    const unsigned int keyLen = 100;
    std::vector<uint32_t> sizes = {100};

    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);

    const auto key = this->getRandomNewKey(alloc, keyLen);
    auto handle = util::allocateAccessible(alloc, poolId, key, sizes[0]);
    ASSERT_NE(nullptr, handle);
    const uint8_t classId = alloc.getAllocInfo(handle->getMemory()).classId;

    auto r = std::async(std::launch::async, [&] {
      do {
        alloc.releaseSlab(poolId, classId, SlabReleaseMode::kRebalance);
      } while (handle != nullptr);
      ASSERT_EQ(nullptr, handle);
    });

    // sleep for 2 seconds to ensure the thread that releases
    // the slab is now waiting for the item handle to be dropped
    /* sleep override */ sleep(2);
    handle.reset();

    r.wait();
  }

  // Test releasing a slab while items are being evicted from the allocator.
  void testRebalancingWithEvictions() {
    std::mutex evictedKeysLock;
    std::set<std::string> evictedKeys;
    auto removeCb = [&evictedKeys, &evictedKeysLock](
                        const typename AllocatorT::RemoveCbData& data) {
      if (data.context == RemoveContext::kEviction) {
        const auto key = data.item.getKey();

        std::lock_guard<std::mutex> l(evictedKeysLock);
        evictedKeys.insert({key.data(), key.size()});
      }
    };

    typename AllocatorT::Config config{};
    config.setRemoveCallback(removeCb);
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    const unsigned int keyLen = 100;
    std::vector<uint32_t> sizes = {10000};

    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);

    const auto key = this->getRandomNewKey(alloc, keyLen);
    auto handle = util::allocateAccessible(alloc, poolId, key, sizes[0]);
    ASSERT_NE(nullptr, handle);
    const uint8_t classId = alloc.getAllocInfo(handle->getMemory()).classId;
    handle.reset();

    evictedKeys.clear();

    std::atomic<bool> isSlabReleased{false};
    auto evictionThread = std::async(std::launch::async, [&] {
      while (!isSlabReleased) {
        this->ensureAllocsOnlyFromEvictions(alloc, poolId, sizes, keyLen,
                                            numBytes, false /* check */);
      }
    });

    alloc.releaseSlab(poolId, classId, SlabReleaseMode::kRebalance);
    isSlabReleased = true;

    evictionThread.wait();

    ASSERT_LT(0, evictedKeys.size());
  }

  // Test releasing a slab while some items are already removed from the
  // allocator,
  // but they are still held by the user.
  void testRebalancingWithItemsAlreadyRemoved() {
    typename AllocatorT::Config config{};
    config.setSlabRebalanceTimeout(std::chrono::milliseconds(0));
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    const unsigned int keyLen = 100;
    const uint32_t itemSize = 100;

    std::vector<typename AllocatorT::WriteHandle> handles;
    for (unsigned int i = 0; i < 10000; ++i) {
      const auto key = this->getRandomNewKey(alloc, keyLen);
      auto handle = util::allocateAccessible(alloc, poolId, key, itemSize);
      ASSERT_NE(nullptr, handle);
      handles.push_back(std::move(handle));
    }
    const uint8_t classId = alloc.getAllocInfo(handles[0]->getMemory()).classId;

    // Remove all the items from the allocator
    for (auto& handle : handles) {
      alloc.remove(handle->getKey());
    }

    auto oldStats = alloc.getPoolStats(poolId);

    auto releaseThread = std::async(std::launch::async, [&] {
      alloc.releaseSlab(poolId, classId, SlabReleaseMode::kRebalance,
                        handles[0]->getMemory());
    });

    // sleep for 2 seconds to ensure the thread that releases
    // the slab is now waiting for the item handle to be dropped
    /* sleep override */ sleep(2);
    handles.clear(); // drop all the item handles

    releaseThread.wait();

    auto newStats = alloc.getPoolStats(poolId);
    // We shold have exaclty one more free slab in the memory pool
    ASSERT_EQ(oldStats.mpStats.freeSlabs + 1, newStats.mpStats.freeSlabs);
    // No eviction, since the user is the one who has removed all the
    // allocations
    ASSERT_EQ(0, newStats.cacheStats[classId].numEvictions());
  }

  void testFastShutdownWithAbortedPoolRebalancer() {
    const size_t nSlabs = 4;
    const size_t size = nSlabs * Slab::kSize;
    const unsigned int keyLen = 100;

    uint8_t poolId;

    // Test allocations. These allocations should remain after save/restore.
    // Original lru allocator
    std::vector<std::string> keys;
    typename AllocatorT::Config config;
    config.enableCachePersistence(this->cacheDir_);
    config.setCacheSize(size);
    config.setSlabRebalanceTimeout(std::chrono::milliseconds(0));
    {
      std::vector<typename AllocatorT::ReadHandle> handles;
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      const std::set<uint32_t> acSizes = {512 * 1024, 1024 * 1024};
      poolId = alloc.addPool("foobar", numBytes, acSizes);
      std::vector<uint32_t> sizes = {450 * 1024};
      // Fill the pool with one allocation size
      this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
      for (const auto& item : alloc) {
        auto key = item.getKey();
        keys.push_back(key.str());
      }

      // get references to the allocated items
      for (auto& key : keys) {
        auto handle = alloc.find(key);
        ASSERT_NE(nullptr, handle.get());
        handles.push_back(std::move(handle));
      }

      // Try to allocate another allocation size. This should trigger
      // slab rebalancer to free up a slab, but it would not succeed
      // because all items have references
      for (int i = 0; i < 10; i++) {
        util::allocateAccessible(alloc, poolId, fmt::format("foo{}", i),
                                 900 * 1024);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      // Wait here for the slab release to start
      while (alloc.getSlabReleaseStats().numActiveSlabReleases == 0) {
        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      // Save, this should stop any workers. However, this would fail since we
      // are holding active handles. But this is good enough to test that
      // shutdown aborts any active blocking rebalancer.
      ASSERT_EQ(alloc.shutDown(), AllocatorT::ShutDownStatus::kFailed);

      EXPECT_EQ(0, alloc.getSlabReleaseStats().numActiveSlabReleases);

      // release the handle references that were being held
      handles.clear();

      ASSERT_EQ(alloc.shutDown(), AllocatorT::ShutDownStatus::kSuccess);
    }
    /* sleep override */ std::this_thread::sleep_for(std::chrono::seconds(3));

    // Restore lru allocator
    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);
      ASSERT_EQ(1, alloc.getGlobalCacheStats().numAbortedSlabReleases);
      for (auto& key : keys) {
        auto handle = alloc.find(key);
        ASSERT_NE(nullptr, handle.get());
      }
      alloc.shutDown();
    }
  }

  // test item sampling by getting a random item from memory
  void testItemSampling() {
    typename AllocatorT::Config config{};
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    // no valid item in cache yet, so we shouldn't get anything
    ASSERT_FALSE(alloc.getSampleItem().isValid());

    // fill up the pool, so any random memory we grab is a valid item
    const unsigned int nSizes = 10;
    const unsigned int keyLen = 100;
    const auto sizes = this->getValidAllocSizes(alloc, poolId, nSizes, keyLen);
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);

    ReadHandle handle;
    {
      auto sampleItem = alloc.getSampleItem();
      ASSERT_TRUE(sampleItem.isValid());
      handle = alloc.find(sampleItem->getKey());
      ASSERT_NE(nullptr, handle);
      ASSERT_EQ(2, handle->getRefCount());
    }
    ASSERT_EQ(1, handle->getRefCount());
  }

  // test limits imposed on number of eviction search tries
  void testEvictionSearchLimit(typename AllocatorT::MMConfig mmConfig) {
    std::set<std::string> evictedKeys;
    auto removeCb =
        [&evictedKeys](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto key = data.item.getKey();
            evictedKeys.insert({key.data(), key.size()});
          }
        };

    typename AllocatorT::Config config{};
    config.setEvictionSearchLimit(2);
    config.setRemoveCallback(removeCb);
    config.setCacheSize(2 * Slab::kSize);

    AllocatorT alloc(config);
    const auto poolId =
        alloc.addPool("foobar", Slab::kSize, {} /* allocSizes */, mmConfig);

    const uint32_t size = 100;
    const std::string keyPrefix = "key_";

    // we allocate first two items and hold on to the handle
    const auto firstIt = util::allocateAccessible(alloc, poolId, "key_0", size);
    ASSERT_NE(nullptr, firstIt);
    const auto secondIt =
        util::allocateAccessible(alloc, poolId, "key_1", size);
    ASSERT_NE(nullptr, secondIt);

    // eventually, we'll run out of memory here, and due to the
    // search limit being 2, we will give up looking for another
    // item to evict after failing to evict the first two items.
    for (unsigned int i = 2;; ++i) {
      const auto key = keyPrefix + folly::to<std::string>(i);
      auto it = util::allocateAccessible(alloc, poolId, key, size);
      if (it == nullptr) {
        break;
      }
    }
    ASSERT_TRUE(evictedKeys.empty());

    const auto poolStats = alloc.getPoolStats(poolId);
    ASSERT_LT(0, poolStats.numItems());
  }

  void testEvictionPredicate() {
    std::string evictedKey;
    auto removeCb =
        [&evictedKey](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kEviction) {
            const auto k = data.item.getKey();
            evictedKey.assign(k.data(), k.size());
          }
        };

    auto predicate = [](const typename AllocatorT::Item& item) {
      return !*reinterpret_cast<const bool*>(item.getMemory());
    };

    typename AllocatorT::Config config{};
    config.setRemoveCallback(removeCb);
    config.setEvictionPredicate(predicate);
    config.setCacheSize(2 * Slab::kSize);

    AllocatorT alloc(config);
    const size_t kAllocSize = 1024;
    const size_t kValueSize = 64;
    const auto poolId = alloc.addPool("foobar", Slab::kSize, {kAllocSize});
    const size_t itemsPerSlab = Slab::kSize / kAllocSize;

    const std::string keyPrefix = "key_";

    // Fill the slab exactly. key_0 is inserted first so it sits at the
    // LRU tail.  Mark key_0 as "dirty" so the predicate will skip it.
    for (size_t i = 0; i < itemsPerSlab; ++i) {
      const auto key = keyPrefix + folly::to<std::string>(i);
      auto handle = alloc.allocate(poolId, key, kValueSize);
      ASSERT_NE(nullptr, handle);
      *reinterpret_cast<bool*>(handle->getMemory()) = (i == 0);
      alloc.insertOrReplace(handle);
    }

    // One more allocation forces an eviction.  The predicate should skip
    // key_0 (dirty) and evict key_1 instead.
    auto handle =
        util::allocateAccessible(alloc, poolId, "trigger_eviction", kValueSize);
    ASSERT_NE(nullptr, handle);
    ASSERT_EQ("key_1", evictedKey);

    // key_0 should still be in the cache (dirty, skipped by predicate).
    ASSERT_NE(nullptr, alloc.peek("key_0"));
    // key_1 should have been evicted.
    ASSERT_EQ(nullptr, alloc.peek("key_1"));
  }

  void testInsertAndFind(AllocatorT& alloc) {
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const size_t kAllocSize = 1024, kItemSize = 512;
    auto poolId = alloc.addPool("default", numBytes, {kAllocSize});
    const size_t itemsPerSlab = Slab::kSize / kAllocSize;
    ASSERT_GT(itemsPerSlab, 0);
    for (unsigned int i = 0; i < itemsPerSlab; ++i) {
      auto key = "key_" + folly::to<std::string>(i);
      auto handle = alloc.allocate(poolId, key, kItemSize);
      ASSERT_NE(nullptr, handle);
      ASSERT_TRUE(alloc.insert(handle));
    }

    for (unsigned int i = 0; i < itemsPerSlab; ++i) {
      auto key = "key_" + folly::to<std::string>(i);
      auto handle = alloc.find(key);
      ASSERT_NE(nullptr, handle);
    }
  }

  // Ensure we have fragmentation stats accurate
  void testFragmentationSize() {
    const int numSlabs = 2;
    typename AllocatorT::Config config;
    config.enableMovingOnSlabRelease(
        [](typename AllocatorT::Item& oldItem,
           typename AllocatorT::Item& newItem,
           typename AllocatorT::Item* /* parentPtr */) {
          memcpy(newItem.getMemory(), oldItem.getMemory(), oldItem.getSize());
        });
    config.setCacheSize((numSlabs + 1) * Slab::kSize);
    AllocatorT allocator(config);
    const size_t numBytes = allocator.getCacheMemoryStats().ramCacheSize;
    const size_t kAllocSize = Slab::kSize, kItemSize = 64;
    auto poolId = allocator.addPool("default", numBytes, {kAllocSize});

    // Start with fragmentation size 0
    auto current = allocator.getPoolStats(poolId).totalFragmentation();
    ASSERT_EQ(current, 0);

    // Allocate one item on the first slab. Now we have some fragmentation.
    util::allocateAccessible(allocator, poolId, "slab0_key0", kItemSize);
    const auto fragmentationForOneItem =
        allocator.getPoolStats(poolId).totalFragmentation();
    current = fragmentationForOneItem;
    ASSERT_GT(fragmentationForOneItem, 0);

    // Allocate one item on the second slab. Now we should have doubled
    // fragmentation.
    util::allocateAccessible(allocator, poolId, "slab1_key1", kItemSize);
    current = allocator.getPoolStats(poolId).totalFragmentation();
    ASSERT_EQ(current, 2 * fragmentationForOneItem);

    // Allocate one more causing eviction of slab0_key, shoukd keep the
    // fragmantation size unchanged.
    const void* expectedAlloc = allocator.findInternal("slab0_key0").get();
    ASSERT_EQ(
        util::allocateAccessible(allocator, poolId, "slab0_key2", kItemSize)
            .get(),
        expectedAlloc);
    current = allocator.getPoolStats(poolId).totalFragmentation();
    ASSERT_EQ(current, 2 * fragmentationForOneItem);

    // Release the second slab. Now the fragmentation size should be reduced.
    void* sourceAlloc =
        static_cast<void*>(allocator.findInternal("slab1_key1").get());
    const auto allocInfo = allocator.getAllocInfo(sourceAlloc);
    allocator.releaseSlab(allocInfo.poolId, allocInfo.classId,
                          SlabReleaseMode::kResize, sourceAlloc);
    ASSERT_EQ(allocator.getSlabReleaseStats().numSlabReleaseForResize, 1);
    current = allocator.getPoolStats(poolId).totalFragmentation();
    ASSERT_EQ(current, fragmentationForOneItem);

    // Remove slab1_key1 should drop the fragmentation size back to zero.
    ASSERT_NE(allocator.find("slab1_key1"), nullptr);
    ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, allocator.remove("slab1_key1"));
    current = allocator.getPoolStats(poolId).totalFragmentation();
    ASSERT_EQ(current, 0);

    // Creating item of different size will have different fragmentation size
    util::allocateAccessible(allocator, poolId, "slab0_key3", kItemSize / 2);
    current = allocator.getPoolStats(poolId).totalFragmentation();
    auto fragmentationForOneSmallItem = current;
    ASSERT_GT(current, fragmentationForOneItem);
    util::allocateAccessible(allocator, poolId, "slab0_key4", kItemSize / 2);
    current = allocator.getPoolStats(poolId).totalFragmentation();
    ASSERT_EQ(current, 2 * fragmentationForOneSmallItem);
    ASSERT_GT(current, 2 * fragmentationForOneItem);

    // Recycle allocation will adjust for fragmentation size properly
    util::allocateAccessible(allocator, poolId, "slab0_key5", kItemSize);
    current = allocator.getPoolStats(poolId).totalFragmentation();
    ASSERT_LT(current, 2 * fragmentationForOneSmallItem);
    ASSERT_EQ(current, fragmentationForOneSmallItem + fragmentationForOneItem);
  }

  using ReleaseSlabFunc =
      std::function<void(AllocatorT&, const AllocInfo&, void*)>;
  void testMoveItemHelper(bool testEviction, ReleaseSlabFunc releaseSlabFunc) {
    const auto moveCb = [](typename AllocatorT::Item& oldItem,
                           typename AllocatorT::Item& newItem,
                           typename AllocatorT::Item* /* parentPtr */) {
      // Simple move callback
      memcpy(newItem.getMemory(), oldItem.getMemory(), oldItem.getSize());
    };

    const int numSlabs = 2;

    // Request numSlabs + 1 slabs so that we get numSlabs usable slabs
    typename AllocatorT::Config config;
    config.enableMovingOnSlabRelease(moveCb, {} /* ChainedItemsMoveSync */);
    config.setCacheSize((numSlabs + 1) * Slab::kSize);
    AllocatorT allocator(config);
    const size_t numBytes = allocator.getCacheMemoryStats().ramCacheSize;
    const size_t kAllocSize = 128, kItemSize = 64;
    auto poolId = allocator.addPool("default", numBytes, {kAllocSize});
    const size_t itemsPerSlab = Slab::kSize / kAllocSize;
    ASSERT_GT(itemsPerSlab, 0);

    // This will eventually be the destination for the move
    const void* destMemory =
        util::allocateAccessible(allocator, poolId, "slab0_key0", kItemSize)
            ->getMemory();

    // Fill the rest of the first slab
    for (size_t i = 1; i < itemsPerSlab; i++) {
      util::allocateAccessible(allocator, poolId,
                               "slab0_key" + folly::to<std::string>(i),
                               kItemSize);
    }

    // Generate some random content
    std::array<char, kItemSize> content;
    for (size_t i = 0; i < kItemSize; i++) {
      content[i] = folly::Random::rand32() % 2;
    }

    // Allocate a single item from the second slab, this will be the source
    // for the move.
    char* oldContent = reinterpret_cast<char*>(
        util::allocateAccessible(allocator, poolId, "slab1_key0", kItemSize)
            ->getMemory());
    memcpy(oldContent, content.data(), kItemSize);

    // Remove a single item from the first slab to free up the destination
    ASSERT_EQ(AllocatorT::RemoveRes::kSuccess, allocator.remove("slab0_key0"));

    // Release the second slab, this should automatically move the source item
    // to the destination.
    const auto beginFragmentationSize =
        allocator.getPoolStats(poolId).totalFragmentation();
    void* sourceAlloc =
        static_cast<void*>(allocator.findInternal("slab1_key0").get());
    const auto allocInfo = allocator.getAllocInfo(sourceAlloc);

    releaseSlabFunc(allocator, allocInfo, sourceAlloc);

    // Check that the new handle points to the expected location
    const auto newHandle = allocator.find("slab1_key0");
    ASSERT_NE(newHandle, nullptr);
    const void* newMemory = newHandle->getMemory();
    ASSERT_EQ(destMemory, newMemory);

    // Check that the content has been copied over
    const auto* newContent = reinterpret_cast<const char*>(newMemory);
    ASSERT_NE(oldContent, newContent);
    for (size_t i = 0; i < kItemSize; i++) {
      ASSERT_EQ(content[i], newContent[i]);
    }
    // Check that fragmentation size didn't change
    const auto endFragmentationSize =
        allocator.getPoolStats(poolId).totalFragmentation();
    ASSERT_EQ(endFragmentationSize, beginFragmentationSize);

    // Send the released slab to a different pool so that we don't use it for
    // new allocations.
    auto newPoolId = allocator.addPool("new_pool", 0, {kAllocSize * 2});
    allocator.resizePools(allocInfo.poolId, newPoolId, Slab::kSize);

    // Make a new allocation, this should evict the allocation storing
    // "slab0_key1" and not the move destination, which was previously the
    // oldest entry in the LRU.
    // This doesn't work for more complex eviction schemes where the item
    // inserted first doesn't necessarily end up being evicted first.
    if (testEviction) {
      const void* expectedAlloc = allocator.findInternal("slab0_key1").get();
      auto handle =
          util::allocateAccessible(allocator, poolId, "test_key", kItemSize);
      ASSERT_EQ(handle.get(), expectedAlloc);
    }
  }

  // Try moving a single item from one slab to another
  void testMoveItem(bool testEviction) {
    auto releaseSlabFunc = [](AllocatorT& allocator,
                              const AllocInfo& allocInfo,
                              void* sourceAlloc) {
      allocator.releaseSlab(allocInfo.poolId,
                            allocInfo.classId,
                            SlabReleaseMode::kResize,
                            sourceAlloc);
      ASSERT_EQ(allocator.getSlabReleaseStats().numSlabReleaseForResize, 1);
    };

    testMoveItemHelper(testEviction, std::move(releaseSlabFunc));
  }

  // Try moving a single item from one slab to another while a separate thread
  // has a ref count to the slab to be released for some time. This tests the
  // retry logic.
  void testMoveItemRetryWithRefCount(bool testEviction) {
    auto releaseSlabFunc = [](AllocatorT& allocator,
                              const AllocInfo& allocInfo,
                              void* sourceAlloc) {
      std::atomic<bool> holdItemHandle{false};

      // Spin up a thread and take a hold on the item handle
      // from second slab and sleep for sometime, so that
      // the main thread attempting the slabRelease will
      // continue retrying in a loop for sometime.
      std::thread otherThread([&]() {
        const auto tempHandle = allocator.find("slab1_key0");
        holdItemHandle = true;
        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      });

      // Wait for the otherThread to take a refcount on a cache item
      // from second slab, which we are trying to move to another slab.
      while (!holdItemHandle) {
        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      allocator.releaseSlab(allocInfo.poolId,
                            allocInfo.classId,
                            SlabReleaseMode::kResize,
                            sourceAlloc);
      otherThread.join();

      // in our new version with marking item as moving, move attempts
      // will only fail if there is a concurrent set to that item, in
      // this case if the handle to an item is held, the slab release
      // will keep trying to mark the item as moving - we currently
      // don't have a counter for that (but this test assumes that
      // if handle is held then moveForSlabRelease will retry,
      // that is where the move attempts counter is incremented)
      //
      // as a fix, we increment the move attempts counter during
      // markMovingForSlabRelase too
      XLOG(INFO, "Number of move retry attempts: ",
           allocator.getSlabReleaseStats().numMoveAttempts);
      ASSERT_GT(allocator.getSlabReleaseStats().numMoveAttempts, 1);
      ASSERT_EQ(allocator.getSlabReleaseStats().numMoveSuccesses, 1);
    };

    testMoveItemHelper(testEviction, std::move(releaseSlabFunc));
  }

  void testAllocateWithTTL() {
    const int numSlabs = 2;

    // set up allocator for TTL test
    typename AllocatorT::Config config;
    config.setCacheSize((numSlabs + 1) * Slab::kSize);
    AllocatorT allocator(config);

    const size_t numBytes = allocator.getCacheMemoryStats().ramCacheSize;
    const size_t kAllocSize = 128, kItemSize = 100;
    auto poolId = allocator.addPool("default", numBytes);
    const size_t itemsPerSlab = Slab::kSize / kAllocSize;
    ASSERT_GT(itemsPerSlab, 0);

    const uint32_t maxTTL = 20;
    const int numItems = 20000;
    std::vector<uint32_t> itemsExpiryTime(numItems);

    int numAllocatedItems = 0;
    // allocate items with different TTL
    for (unsigned int i = 0; i < numItems; i++) {
      const uint32_t randomTTL = folly::Random::rand32(1, maxTTL);
      auto handle = util::allocateAccessible(
          allocator, poolId, folly::to<std::string>(i), kItemSize, randomTTL);
      if (!handle) {
        ASSERT_GT(numAllocatedItems, 0);
        break;
      }
      ++numAllocatedItems;
      itemsExpiryTime[i] = handle->getExpiryTime();
    }

    const uint32_t startTime = static_cast<uint32_t>(util::getCurrentTimeSec());
    // start to check TTL and try to evict expired items during finding
    while (static_cast<uint32_t>(util::getCurrentTimeSec()) <=
           startTime + maxTTL) {
      for (unsigned int i = 0; i < numItems; i++) {
        uint32_t currentTime = static_cast<uint32_t>(util::getCurrentTimeSec());
        const auto handle = allocator.find(folly::to<std::string>(i));
        if (currentTime < itemsExpiryTime[i] - 1) {
          ASSERT_NE(nullptr, handle);
        } else if (currentTime > itemsExpiryTime[i] + 1) {
          ASSERT_EQ(nullptr, handle);
        }
      }
    }

    for (unsigned int i = 0; i < numItems; i++) {
      const auto handle = allocator.find(folly::to<std::string>(i));
      ASSERT_EQ(nullptr, handle);
    }
  }

  // test that find returns an empty handle if the key is expired.
  void testExpiredFind() {
    const int numSlabs = 2;

    // set up allocator for TTL test
    typename AllocatorT::Config config;
    config.setCacheSize((numSlabs + 1) * Slab::kSize);
    config.reaperInterval = std::chrono::milliseconds(0);
    AllocatorT allocator(config);

    const size_t numBytes = allocator.getCacheMemoryStats().ramCacheSize;
    const size_t kAllocSize = 128, kItemSize = 100;
    auto poolId = allocator.addPool("default", numBytes);
    const size_t itemsPerSlab = Slab::kSize / kAllocSize;
    ASSERT_GT(itemsPerSlab, 0);

    const uint32_t maxTTL = 20;
    const int numItems = 20000;
    std::vector<uint32_t> itemsExpiryTime(numItems);

    int numAllocatedItems = 0;
    // allocate items with different TTL
    for (unsigned int i = 0; i < numItems; i++) {
      const uint32_t randomTTL = folly::Random::rand32(1, maxTTL);
      auto handle = util::allocateAccessible(
          allocator, poolId, folly::to<std::string>(i), kItemSize, randomTTL);
      if (!handle) {
        ASSERT_GT(numAllocatedItems, 0);
        break;
      }
      ++numAllocatedItems;
      itemsExpiryTime[i] = handle->getExpiryTime();
    }

    unsigned long totalCacheMissCount = 0;
    const uint32_t startTime = static_cast<uint32_t>(util::getCurrentTimeSec());

    auto checkExistenceCb = [&itemsExpiryTime](const auto& handle,
                                               uint32_t currentTime,
                                               unsigned int i) {
      if (currentTime < itemsExpiryTime[i] - 1) {
        EXPECT_NE(nullptr, handle);
      } else if (currentTime > itemsExpiryTime[i] + 1) {
        EXPECT_EQ(nullptr, handle);
        EXPECT_EQ(true, handle.wasExpired());
      }
    };

    // start to check TTL
    while (static_cast<uint32_t>(util::getCurrentTimeSec()) <=
           startTime + maxTTL) {
      for (unsigned int i = 0; i < numItems; i++) {
        uint32_t currentTime = static_cast<uint32_t>(util::getCurrentTimeSec());
        const auto k = folly::to<std::string>(i);
        const auto h1 = allocator.find(k);
        checkExistenceCb(h1, currentTime, i);

        const auto h2 = allocator.findFast(k);
        checkExistenceCb(h1, currentTime, i);

        const auto h3 = allocator.peek(k);
        checkExistenceCb(h1, currentTime, i);

        // if handle is null, it must have been the result of cache item being
        // expired hence the cache miss.
        if (h1 == nullptr) {
          totalCacheMissCount++;
        }

        if (h2 == nullptr) {
          totalCacheMissCount++;
        }

        // We don't need to bump totalCacheMissCount for h3, because
        // peek() API does not bump cache miss stats.
      }
    }

    int expired = 0;
    for (const auto& item : allocator) {
      ASSERT_TRUE(item.isExpired());
      ++expired;
    }
    ASSERT_EQ(numItems, expired);
    ASSERT_EQ(totalCacheMissCount,
              allocator.getGlobalCacheStats().numCacheGetMiss);
  }

  void testNonExpiredEvictionStats() {
    typename AllocatorT::Config config;
    config.setCacheSize(3 * Slab::kSize);
    config.reaperInterval = std::chrono::milliseconds{0};
    AllocatorT alloc(config);
    const auto poolId = alloc.addPool("default", Slab::kSize);
    constexpr unsigned int kKeyLen = 16;
    const std::vector<uint32_t> sizes{1000};
    // Items have no TTL, so every eviction while filling the pool is of a live
    // item and must be counted as non-expired.
    this->fillUpPoolUntilEvictions(alloc, poolId, sizes, kKeyLen);
    const auto numLiveEvictions = alloc.getGlobalCacheStats().numEvictions;
    ASSERT_GT(numLiveEvictions, 0);
    EXPECT_EQ(numLiveEvictions,
              alloc.getGlobalCacheStats().numRamNonExpiredEvict);

    // Expire every item; no eviction from here on should be counted.
    uint64_t numExpired = 0;
    for (const auto& item : alloc) {
      auto handle = alloc.findInternal(item.getKey());
      ASSERT_NE(nullptr, handle);
      ASSERT_TRUE(handle->updateExpiryTime(util::getCurrentTimeSec() - 1));
      ++numExpired;
    }
    ASSERT_GT(numExpired, 1);

    // The pool is full, so this allocation evicts an expired item from the LRU.
    auto handle = util::allocateAccessible(
        alloc, poolId, this->getRandomNewKey(alloc, kKeyLen), sizes[0]);
    ASSERT_NE(nullptr, handle);
    EXPECT_EQ(numLiveEvictions,
              alloc.getGlobalCacheStats().numRamNonExpiredEvict);
    // Expire the new item too, so the slab release only evicts expired items.
    ASSERT_TRUE(handle->updateExpiryTime(util::getCurrentTimeSec() - 1));
    const auto classId = alloc.getAllocInfo(handle->getMemory()).classId;
    handle.reset();
    // Without a moveCb, slab release evicts every item in the slab.
    alloc.releaseSlab(poolId, classId, SlabReleaseMode::kResize);
    EXPECT_GT(alloc.getGlobalCacheStats().numEvictions, numLiveEvictions);
    EXPECT_EQ(numLiveEvictions,
              alloc.getGlobalCacheStats().numRamNonExpiredEvict);

    bool counterExported = false;
    alloc.exportStats("test.", std::chrono::seconds{60},
                      [&](folly::StringPiece name, uint64_t value) {
                        if (name == "test.ram.evictions.non_expired.60") {
                          EXPECT_EQ(numLiveEvictions, value);
                          counterExported = true;
                        }
                      });
    EXPECT_TRUE(counterExported);
  }

  void testAllocateWithItemsReaper() {
    const int numSlabs = 2;

    // set up allocator for itemsReaper test
    typename AllocatorT::Config config;
    config.setCacheSize((numSlabs + 1) * Slab::kSize);
    config.enableItemReaperInBackground(std::chrono::seconds{1}, {});

    AllocatorT allocator(config);

    const size_t numBytes = allocator.getCacheMemoryStats().ramCacheSize;
    const size_t kItemSize = 100;
    auto poolId = allocator.addPool("default", numBytes);

    const uint32_t maxTTL = 20;
    const int numItems = 10000;

    // to make sure we skip reaping elements when application is using them
    // grab a handle to one of the elements and check that it is not reaped as
    // long as we are holding the handle.
    typename AllocatorT::WriteHandle randomKeyHdl;

    const uint32_t randomKey = folly::Random::rand32(0, numItems - 1);

    int numAllocatedItems = 0;
    for (unsigned int i = 0; i < numItems; i++) {
      const uint32_t randomTTL = folly::Random::rand32(1, maxTTL + 1);
      auto handle = util::allocateAccessible(
          allocator, poolId, folly::to<std::string>(i), kItemSize, randomTTL);
      if (i == randomKey) {
        randomKeyHdl = std::move(handle);
      }
      ++numAllocatedItems;
    }

    ASSERT_TRUE(randomKeyHdl);
    ASSERT_GT(numAllocatedItems, 0);

    const unsigned int keyLen = 100;
    const unsigned int nSizes = 10;
    const auto sizes =
        this->getValidAllocSizes(allocator, poolId, nSizes, keyLen);
    this->fillUpPoolUntilEvictions(allocator, poolId, sizes, keyLen);

    uint32_t startTime = static_cast<uint32_t>(util::getCurrentTimeSec());
    bool clearAllItems = false;
    bool keyWithHandleReaped = true;

    // verify all keys except the key with handle are reaped.
    while (!clearAllItems &&
           static_cast<uint32_t>(util::getCurrentTimeSec()) - startTime <
               maxTTL * 10) {
      clearAllItems = true;
      for (unsigned int i = 0; i < numItems; i++) {
        const auto handle = allocator.find(folly::to<std::string>(i));
        if (handle) {
          if (handle == randomKeyHdl) {
            keyWithHandleReaped = false;
          } else {
            clearAllItems = false;
          }
        }
      }
    }

    ASSERT_FALSE(keyWithHandleReaped);

    // reset the handle and verify that all keys are now reaped.
    randomKeyHdl.reset();
    clearAllItems = false;
    while (!clearAllItems &&
           static_cast<uint32_t>(util::getCurrentTimeSec()) - startTime <
               maxTTL * 10) {
      clearAllItems = true;
      for (unsigned int i = 0; i < numItems; i++) {
        const auto handle = allocator.find(folly::to<std::string>(i));
        if (handle) {
          clearAllItems = false;
        }
      }
    }

    startTime = (uint32_t)util::getCurrentTimeSec() - startTime;
    ASSERT_GE(startTime, maxTTL);

    for (unsigned int i = 0; i < numItems; i++) {
      auto handle = allocator.find(folly::to<std::string>(i));
      ASSERT_EQ(nullptr, handle);
    }
  }

  void testReaperNoWaitUntilEvictions() {
    const int numSlabs = 2;

    typename AllocatorT::Config config;
    config.setCacheSize((numSlabs + 1) * Slab::kSize);
    config.enableItemReaperInBackground(std::chrono::seconds{1}, {});

    AllocatorT allocator(config);

    const size_t numBytes = allocator.getCacheMemoryStats().ramCacheSize;
    const size_t kItemSize = 100;
    auto poolId = allocator.addPool("default", numBytes);

    const auto startTime = util::getCurrentTimeMs();
    unsigned int ttlSecs = 2;
    util::allocateAccessible(allocator, poolId, folly::to<std::string>(100),
                             kItemSize, ttlSecs);
    util::allocateAccessible(allocator, poolId, folly::to<std::string>(101),
                             kItemSize);

    std::this_thread::sleep_for(std::chrono::seconds(ttlSecs));
    // Wait reaper deletes the object. wait for it to go at least five times.
    auto stats = allocator.getReaperStats();
    const auto prev = stats.numTraversals;
    while (stats.numTraversals - prev < 5) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      stats = allocator.getReaperStats();
    }
    EXPECT_EQ(1, stats.numReapedItems);
    EXPECT_LE(stats.lastTraversalTimeMs, util::getCurrentTimeMs() - startTime);
  }

  void testReaperOutOfBound() {
    // This test validates a reaper will not crash when it is checking the last
    // item in a slab and it happens to have a large key beyond the end of cache
    // space.
    const int numSlabs = 2;

    typename AllocatorT::Config config;
    // start with no reaper
    auto reaperInterval = std::chrono::milliseconds(0);
    config.reaperInterval = reaperInterval;
    config.enableCachePersistence(this->cacheDir_,
                                  ((void*)(uintptr_t)0x7e0000000000));
    config.setCacheSize((numSlabs + 1) * Slab::kSize);

    AllocatorT allocator(AllocatorT::SharedMemNew, config);
    const size_t numBytes = allocator.getCacheMemoryStats().ramCacheSize;
    auto poolId =
        allocator.addPool("default", numBytes, std::set<uint32_t>{64, 1000});

    const std::string largeKey = this->getRandomNewKey(allocator, 255);

    auto largeIt = util::allocateAccessible(allocator, poolId, largeKey, 0,
                                            1 /* ttl seconds */);
    ASSERT_NE(nullptr, largeIt);

    std::vector<typename AllocatorT::WriteHandle> handles;
    int i = 0;
    while (auto it = util::allocateAccessible(allocator, poolId,
                                              folly::to<std::string>(i++), 0)) {
      handles.push_back(std::move(it));
    }
    ASSERT_FALSE(handles.empty());

    // Use the last item in the slab. This is the last 64 bytes until we
    // go out of cache space.
    auto* lastIt = handles.back().get();
    for (auto& it : handles) {
      allocator.remove(it);
    }
    handles.clear();

    // This is NOT legal. However, copying over the item header is useful to
    // simulate a garbage memory with a seemingly large key. What we want to
    // copy over is the expiry timestamp and also the key size. Key itself
    // doesn't actually matter.
    auto copySize = AllocatorT::Item::getRequiredSize("small", 0);
    ASAN_UNPOISON_MEMORY_REGION(lastIt, copySize);
    std::memcpy(lastIt, largeIt.get(), copySize);
    ASAN_POISON_MEMORY_REGION(lastIt, copySize);
    largeIt.reset();

    // Sleep for >1 second to ensure large item has expired
    std::this_thread::sleep_for(std::chrono::seconds{2});

    auto checkReaperInterval = [&]() {
      auto params = allocator.serializeConfigParams();
      EXPECT_EQ(params["reaperInterval"], util::toString(reaperInterval));
    };

    checkReaperInterval();
    // Start reaper, we should not crash
    reaperInterval = std::chrono::milliseconds{1};
    allocator.startNewReaper(reaperInterval,
                             util::Throttler::Config::makeNoThrottleConfig());

    // Check if relevent configuration is changed
    checkReaperInterval();

    // Loop until we have reaped at least one iteration
    ASSERT_EVENTUALLY_TRUE(
        [&]() { return allocator.getReaperStats().numTraversals >= 1; });
    auto stats = allocator.getReaperStats();
    EXPECT_EQ(1, stats.numReapedItems);
  }

  void testReaperSkippingSlabConcurrentTraversal() {
    // Testing if a reaper skips a slab correctly when the allocation class lock
    // is held. The scenario here is that we have two threads traversing slabs
    // concurrently.
    const int numSlabs = 2;

    typename AllocatorT::Config config;
    // start with no reaper
    config.reaperInterval = std::chrono::seconds(0);
    config.setCacheSize(numSlabs * Slab::kSize);

    AllocatorT allocator(config);
    const size_t numBytes = allocator.getCacheMemoryStats().ramCacheSize;
    auto poolId =
        allocator.addPool("default", numBytes, std::set<uint32_t>{1000, 10000});
    // Allocate 1 slab to the allocClass
    util::allocateAccessible(allocator, poolId, "test", 5000, 1);

    // Sleep for 2 seconds to ensure item has expired
    std::this_thread::sleep_for(std::chrono::seconds{2});

    // Start reaper
    allocator.startNewReaper(std::chrono::milliseconds{1},
                             util::Throttler::Config::makeNoThrottleConfig());
    // Lock the allocClass for 2 ms so reaper will skip the slab the
    // allocClass owns
    allocator.traverseAndExpireItems(
        [](void* /* unused */, AllocInfo /* unused */) {
          std::this_thread::sleep_for(std::chrono::milliseconds{2});
          return true;
        });
    // We should have at least one slab skipped since traverseAndExpireItems
    // call will make the associated allocClass hold a lock for 2 ms. The
    // reaper should be ran at least once within the 2 ms. If it ran more
    // than once numReaperSkippedSlabs could be larger than 1.
    ASSERT_GE(allocator.getGlobalCacheStats().numReaperSkippedSlabs, 1);
  }

  void testReaperSkippingSlabTraversalWhileSlabReleasing() {
    // Testing if a reaper skips a slab correctly when the allocation class lock
    // is held because one of the slabs is in the release process.
    const int numSlabs = 2;

    typename AllocatorT::Config config;
    // start with no reaper
    config.reaperInterval = std::chrono::seconds(0);
    config.setCacheSize(numSlabs * Slab::kSize);
    config.setSlabRebalanceTimeout(std::chrono::milliseconds(0));

    AllocatorT allocator(config);
    const size_t numBytes = allocator.getCacheMemoryStats().ramCacheSize;
    // We only need a single alloc class for this test
    auto poolId =
        allocator.addPool("default", numBytes, std::set<uint32_t>{64});

    // allocate and hold this item handle to STALL slab release
    auto it = util::allocateAccessible(allocator, poolId, "test", 0, 1);
    std::thread t1([&allocator, poolId] {
      allocator.releaseSlab(poolId, 0, Slab::kInvalidClassId,
                            SlabReleaseMode::kRebalance);
    });

    // Make sure releaseSlab is executed
    std::this_thread::sleep_for(std::chrono::seconds{2});

    allocator.traverseAndExpireItems(
        [](void* /* unused */, AllocInfo /* unused */) { return true; });

    it.reset();
    t1.join();
    // Verify we have at least skipped one slab
    ASSERT_GE(allocator.getGlobalCacheStats().numReaperSkippedSlabs, 1);
  }

  void testAllocSizes() {
    const int numSlabs = 2;

    typename AllocatorT::Config config;
    config.setCacheSize(numSlabs * Slab::kSize);
    AllocatorT allocator(config);
    const size_t numBytes = allocator.getCacheMemoryStats().ramCacheSize;
    std::set<uint32_t> badAllocSizes = {48, 64, 128, 256, 512};
    std::set<uint32_t> goodAllocSizes = {64, 128, 256, 512};
    ASSERT_THROW(allocator.addPool("default", numBytes, badAllocSizes),
                 std::invalid_argument);
    ASSERT_NO_THROW(allocator.addPool("default", numBytes, goodAllocSizes));
  }

  void testGenAllocClassesTuned() {
    auto allocClasses = util::genAllocClassesTuned();
    ASSERT_GT(allocClasses.size(), 0);

    // Create an allocator with enough memory to support all allocation classes
    // We need at least one slab per allocation class for testing
    const size_t numSlabs = allocClasses.size() + 10;
    typename AllocatorT::Config config;
    config.setCacheSize(numSlabs * Slab::kSize);
    AllocatorT allocator(config);

    // Create a pool using the tuned allocation classes
    const size_t numBytes = allocator.getCacheMemoryStats().ramCacheSize;
    PoolId poolId = allocator.addPool("tuned_pool", numBytes, allocClasses);

    // Try to allocate an item for each allocation size
    std::vector<typename AllocatorT::WriteHandle> handles;
    size_t allocIndex = 0;
    for (auto allocSize : allocClasses) {
      const std::string key = "key_" + std::to_string(allocIndex++);

      // Calculate the value size needed to result in this allocation size
      // getRequiredSize(key, valueSize) returns the total size needed
      // We need to find valueSize such that getRequiredSize <= allocSize
      const auto requiredSizeForZero =
          AllocatorT::Item::getRequiredSize(key, 0);
      uint32_t valueSize = allocSize - requiredSizeForZero;

      auto handle = allocator.allocate(poolId, key, valueSize);
      ASSERT_NE(nullptr, handle)
          << "Failed to allocate item for alloc size " << allocSize
          << " (value size: " << valueSize << ")";

      ASSERT_TRUE(allocator.insert(handle));
      auto foundHandle = allocator.find(key);
      ASSERT_NE(nullptr, foundHandle) << "Failed to find item with key " << key;

      // Keep the handle to prevent immediate eviction
      handles.push_back(std::move(handle));
    }

    // After allocating items, verify each allocation class now has exactly 1
    // slab
    auto finalPoolStats = allocator.getPoolStats(poolId);
    for (size_t i = 0; i < allocClasses.size(); ++i) {
      const auto classId = static_cast<ClassId>(i);
      EXPECT_EQ(1, finalPoolStats.numSlabsForClass(classId))
          << "Allocation class " << static_cast<int>(classId)
          << " should have exactly 1 slab after allocation";
    }
  }

  // Check that item is in the expected container.
  bool findItem(AllocatorT& allocator, typename AllocatorT::Item* item) {
    auto& container = allocator.getMMContainer(*item);
    auto itr = container.getEvictionIterator();
    bool found = false;
    while (itr) {
      if (itr.get() == item) {
        found = true;
        break;
      }
      ++itr;
    }
    return found;
  }

  void testBasicFreeMemStrategy() {
    const int numSlabs = 4;

    // Request numSlabs + 1 slabs so that we get numSlabs usable slabs
    typename AllocatorT::Config config;
    config.enablePoolRebalancing(
        std::make_shared<FreeMemStrategy>(FreeMemStrategy::Config{0, 3, 1000}),
        std::chrono::seconds{1});
    config.setCacheSize((numSlabs + 1) * Slab::kSize);
    AllocatorT allocator(config);

    const size_t numBytes = allocator.getCacheMemoryStats().ramCacheSize;
    const size_t small = 128;
    const size_t big = 1024;
    auto pid = allocator.addPool("default", numBytes, {small + 128, big + 128});

    // Allocate all memory to small AC
    std::vector<typename AllocatorT::WriteHandle> handles;
    for (unsigned int i = 0;; ++i) {
      auto key = "small_key_" + folly::to<std::string>(i);
      auto handle = allocator.allocate(pid, key, small);
      if (handle == nullptr) {
        break;
      }
      handles.push_back(std::move(handle));
    }

    // Drop all items
    handles.clear();

    // Verify we still cannot allocate from the other AC
    ASSERT_EQ(nullptr, allocator.allocate(pid, "blah", big));

    // Sleep for 3 seconds
    using namespace std::chrono_literals;
    /* sleep override */
    std::this_thread::sleep_for(3s);

    // Verify we can allocate from the other AC,
    // but only for one slab worth of data
    const auto numBigAllocs = Slab::kSize / (big + 128);
    for (unsigned int i = 0;; ++i) {
      auto key = "big_key_" + folly::to<std::string>(i);
      auto handle = allocator.allocate(pid, key, big);
      if (handle == nullptr) {
        break;
      }
      handles.push_back(std::move(handle));
    }
    ASSERT_EQ(numBigAllocs, handles.size());
  }

  // This test will first allocate a normal item. And then it will
  // keep allocating new chained items and appending them to the
  // first item until the system runs out of memory.
  // After that it will drop the normal item's handle and try
  // allocating again to verify that it can be evicted.
  void testAddChainedItemUntilEviction() {
    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc.addPool("one", poolSize);

    const std::vector<uint32_t> sizes = {100, 500, 1000, 2000, 100000};

    // Allocate chained allocs until allocations fail.
    // It will fail because "hello1" has an outsanding item handle alive,
    // and thus it cannot be evicted, so none of its chained allocations
    // can be evicted either
    auto itemHandle = alloc.allocate(pid, "hello1", sizes[0]);
    uint32_t exhaustedSize = 0;
    for (unsigned int i = 0;; ++i) {
      auto chainedItemHandle =
          alloc.allocateChainedItem(itemHandle, sizes[i % sizes.size()]);
      if (chainedItemHandle == nullptr) {
        exhaustedSize = sizes[i % sizes.size()];
        break;
      }
      alloc.addChainedItem(itemHandle, std::move(chainedItemHandle));
    }

    // Verify we cannot allocate a new item either.
    ASSERT_EQ(nullptr, alloc.allocate(pid, "hello2", exhaustedSize));

    // We should be able to allocate a new item after releasing the old one
    itemHandle.reset();
    ASSERT_NE(nullptr, alloc.allocate(pid, "hello2", exhaustedSize));
  }

  void testChainedItemSerialization() {
    typename AllocatorT::Config config;
    config.configureChainedItems();
    AllocatorTest<AllocatorT>::runSerializationTest(config);
  }

  // This basically tests some rudimentary behavior with chained items
  // 1. Freeing a chained item before it has been added to the parent will
  //    not affect the parent item.
  // 2. Once linked, removing the parent item will remove its chained items
  //    as well
  void testAddChainedItemSimple() {
    // the eviction call back to keep track of the evicted keys.
    std::set<std::string> removedKeys;
    auto removeCb =
        [&removedKeys](const typename AllocatorT::RemoveCbData& data) {
          const auto k = data.item.getKey();
          removedKeys.insert({k.data(), k.size()});
        };

    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setRemoveCallback(removeCb);
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc.addPool("one", poolSize);

    const auto size = 100;
    auto itemHandle = util::allocateAccessible(alloc, pid, "hello", size);

    auto chainedItemHandle = alloc.allocateChainedItem(itemHandle, size * 2);
    auto chainedItemHandle2 = alloc.allocateChainedItem(itemHandle, size * 4);
    auto chainedItemHandle3 = alloc.allocateChainedItem(itemHandle, size * 8);

    ASSERT_NE(nullptr, chainedItemHandle);
    ASSERT_NE(nullptr, chainedItemHandle2);
    ASSERT_NE(nullptr, chainedItemHandle3);

    ASSERT_EQ(size * 2, chainedItemHandle->getSize());
    ASSERT_EQ(size * 4, chainedItemHandle2->getSize());
    ASSERT_EQ(size * 8, chainedItemHandle3->getSize());

    // Make sure here each chained item and the parent are from different
    // allocation classes. And fill up the payload of each item.
    // This is to make sure the size of each item is actually what they report
    // Had a bug: D4799860 where we allocated the wrong size for chained item
    {
      const auto parentAllocInfo =
          alloc.allocator_->getAllocInfo(itemHandle->getMemory());
      const auto child1AllocInfo =
          alloc.allocator_->getAllocInfo(chainedItemHandle->getMemory());
      const auto child2AllocInfo =
          alloc.allocator_->getAllocInfo(chainedItemHandle2->getMemory());
      const auto child3AllocInfo =
          alloc.allocator_->getAllocInfo(chainedItemHandle3->getMemory());

      const auto parentCid = parentAllocInfo.classId;
      const auto child1Cid = child1AllocInfo.classId;
      const auto child2Cid = child2AllocInfo.classId;
      const auto child3Cid = child3AllocInfo.classId;

      const bool differentClasses =
          parentCid != child1Cid && parentCid != child2Cid &&
          parentCid != child3Cid && child1Cid != child2Cid &&
          child1Cid != child3Cid && child2Cid != child3Cid;
      ASSERT_TRUE(differentClasses);

      for (uint64_t i = 0; i < itemHandle->getSize(); ++i) {
        reinterpret_cast<uint8_t*>(itemHandle->getMemory())[i] = 'a';
      }
      for (uint64_t i = 0; i < chainedItemHandle->getSize(); ++i) {
        reinterpret_cast<uint8_t*>(chainedItemHandle->getMemory())[i] = 'b';
      }
      for (uint64_t i = 0; i < chainedItemHandle2->getSize(); ++i) {
        reinterpret_cast<uint8_t*>(chainedItemHandle2->getMemory())[i] = 'c';
      }
      for (uint64_t i = 0; i < chainedItemHandle3->getSize(); ++i) {
        reinterpret_cast<uint8_t*>(chainedItemHandle3->getMemory())[i] = 'd';
      }
    }

    // Only add chained item 1 and 2
    alloc.addChainedItem(itemHandle, std::move(chainedItemHandle));
    alloc.addChainedItem(itemHandle, std::move(chainedItemHandle2));

    {
      auto stats = alloc.getPoolStats(pid);
      ASSERT_EQ(0, stats.numFreeAllocs());
      ASSERT_EQ(4, stats.numActiveAllocs());
    }

    // Resetting the last chained allocation should only free itself
    // since it's not linked to the parent yet
    chainedItemHandle3.reset();
    {
      auto stats = alloc.getPoolStats(pid);
      ASSERT_EQ(1, stats.numFreeAllocs());
      ASSERT_EQ(3, stats.numActiveAllocs());
    }

    // Remove the item, but this shouldn't free it since we still have a
    // handle
    alloc.remove("hello");

    ASSERT_TRUE(removedKeys.empty());
    itemHandle.reset();
    ASSERT_FALSE(removedKeys.empty());
    {
      auto stats = alloc.getPoolStats(pid);
      ASSERT_EQ(4, stats.numFreeAllocs());
      ASSERT_EQ(0, stats.numActiveAllocs());
    }

    // A missing chained head reaches the invalid-chain exception.
    auto missing = alloc.allocate(pid, "missing-chain", size);
    ASSERT_NE(nullptr, missing);
    missing->markHasChainedItem();
    ASSERT_FALSE(alloc.findChainedItem(*missing));
    auto* item = missing.release();
    ASSERT_THROW(alloc.release(item, true), exception::ChainedItemInvalid);
    item->unmarkHasChainedItem();
    alloc.allocator_->free(item);
    EXPECT_EQ(0, alloc.getPoolStats(pid).numActiveAllocs());
  }

  // Insert some chained items and make sure they're popped in the
  // expected order
  void testPopChainedItemSimple() {
    // the eviction call back to keep track of the evicted keys.
    std::set<std::string> removedKeys;
    auto removeCb =
        [&removedKeys](const typename AllocatorT::RemoveCbData& data) {
          const auto k = data.item.getKey();
          removedKeys.insert({k.data(), k.size()});
        };

    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.configureChainedItems();
    config.setRemoveCallback(removeCb);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc.addPool("one", poolSize);

    const auto size = 100;
    auto itemHandle = util::allocateAccessible(alloc, pid, "hello", size);

    auto chainedItemHandle = alloc.allocateChainedItem(itemHandle, size * 2);
    ASSERT_NE(nullptr, chainedItemHandle);
    reinterpret_cast<char*>(chainedItemHandle->getMemory())[0] = '1';
    alloc.addChainedItem(itemHandle, std::move(chainedItemHandle));

    auto chainedItemHandle2 = alloc.allocateChainedItem(itemHandle, size * 4);
    ASSERT_NE(nullptr, chainedItemHandle2);
    reinterpret_cast<char*>(chainedItemHandle2->getMemory())[0] = '2';
    alloc.addChainedItem(itemHandle, std::move(chainedItemHandle2));

    auto chainedItemHandle3 = alloc.allocateChainedItem(itemHandle, size * 8);
    ASSERT_NE(nullptr, chainedItemHandle3);
    reinterpret_cast<char*>(chainedItemHandle3->getMemory())[0] = '3';
    alloc.addChainedItem(itemHandle, std::move(chainedItemHandle3));

    {
      auto stats = alloc.getPoolStats(pid);
      ASSERT_EQ(0, stats.numFreeAllocs());
      ASSERT_EQ(4, stats.numActiveAllocs());
    }

    {
      auto popped = alloc.popChainedItem(itemHandle);
      ASSERT_EQ('3', *reinterpret_cast<const char*>(popped->getMemory()));
      popped.reset();
      auto stats = alloc.getPoolStats(pid);
      ASSERT_EQ(1, stats.numFreeAllocs());
      ASSERT_EQ(3, stats.numActiveAllocs());
    }

    {
      auto popped = alloc.popChainedItem(itemHandle);
      ASSERT_EQ('2', *reinterpret_cast<const char*>(popped->getMemory()));
      popped.reset();
      auto stats = alloc.getPoolStats(pid);
      ASSERT_EQ(2, stats.numFreeAllocs());
      ASSERT_EQ(2, stats.numActiveAllocs());
    }

    {
      auto popped = alloc.popChainedItem(itemHandle);
      ASSERT_EQ('1', *reinterpret_cast<const char*>(popped->getMemory()));
      popped.reset();
      auto stats = alloc.getPoolStats(pid);
      ASSERT_EQ(3, stats.numFreeAllocs());
      ASSERT_EQ(1, stats.numActiveAllocs());

      // popped all the chained item, so this should be false
      ASSERT_FALSE(itemHandle->hasChainedItem());
    }

    ASSERT_THROW(alloc.popChainedItem(itemHandle), std::invalid_argument);
  }

  // This tests basically tests slab release when freeing chained items
  // will free the entire chains which they belong to as well.
  void testAddChainedItemSlabRelease() {
    // the eviction call back to keep track of the evicted keys.
    std::set<std::string> removedKeys;
    auto removeCb =
        [&removedKeys](const typename AllocatorT::RemoveCbData& data) {
          const auto k = data.item.getKey();
          removedKeys.insert({k.data(), k.size()});
        };

    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setRemoveCallback(removeCb);
    config.setCacheSize(10 * Slab::kSize);

    // Disable slab rebalancing
    config.enablePoolRebalancing(nullptr, std::chrono::seconds{0});

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc.addPool("one", poolSize);

    const auto size = 1000;

    // Allocate chained allocs until allocations fail.
    // It will fail because "hello1" has an outsanding item handle alive,
    // and thus it cannot be evicted, so none of its chained allocations
    // can be evicted either
    auto itemHandle = util::allocateAccessible(alloc, pid, "hello1", size);
    while (true) {
      auto chainedItemHandle = alloc.allocateChainedItem(itemHandle, size);
      if (chainedItemHandle == nullptr) {
        break;
      }
      alloc.addChainedItem(itemHandle, std::move(chainedItemHandle));
    }

    // Dropping the item handle. The item is still in cache since it's
    // inserted in the hash table
    ASSERT_NE(nullptr, alloc.find("hello1"));
    itemHandle.reset();
    ASSERT_NE(nullptr, alloc.find("hello1"));

    uint8_t cid = -1;
    // find the AC that fits our values
    auto poolStats = alloc.getPoolStats(pid);
    for (uint8_t i = 0; i < poolStats.mpStats.acStats.size(); ++i) {
      if (poolStats.mpStats.acStats[i].activeAllocs) {
        cid = i;
      }
    }

    // Once we release a slab, all chained allocations will be released
    // We can verify this by checking if we can still look up "hello1"
    alloc.releaseSlab(pid, cid, SlabReleaseMode::kRebalance);
    ASSERT_EQ(nullptr, alloc.find("hello1"));

    // Verify only parent item is recorded in the removed key
    ASSERT_EQ(1, removedKeys.size());
  }

  // This basically just makes sure the system does not crash when
  // there are multiple threads allocating chained items while
  // slab release is running in the background.
  void testAddChainedItemMultithread() {
    std::atomic<int> numRemovedKeys{0};
    auto removeCb =
        [&numRemovedKeys](const typename AllocatorT::RemoveCbData& /*data*/) {
          ++numRemovedKeys;
        };

    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setRemoveCallback(removeCb);
    config.setCacheSize(20 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const std::set<uint32_t> allocSizes = {150, 250, 550, 1050, 2050, 5050};
    const auto pid = alloc.addPool("one", poolSize, allocSizes);

    // Allocate 10000 Parent items and for each parent item, 10 chained
    // allocations
    auto allocFn = [&](std::string keyPrefix, std::vector<uint32_t> sizes) {
      for (unsigned int loop = 0; loop < 10; ++loop) {
        for (unsigned int i = 0; i < 1000; ++i) {
          const auto key = keyPrefix + folly::to<std::string>(loop) + "_" +
                           folly::to<std::string>(i);
          auto itemHandle = util::allocateAccessible(alloc, pid, key, sizes[0]);
          for (unsigned int j = 0; j < 10; ++j) {
            const auto size = sizes[j % sizes.size()];
            auto childItem = alloc.allocateChainedItem(itemHandle, size);
            for (unsigned int k = 0; childItem == nullptr && k < 100; ++k) {
              childItem = alloc.allocateChainedItem(itemHandle, size);
            }
            ASSERT_NE(nullptr, childItem);

            alloc.addChainedItem(itemHandle, std::move(childItem));
          }
        }

        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    };

    // Release 5 slabs
    auto releaseFn = [&] {
      for (unsigned int i = 0; i < 5;) {
        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        ClassId cid = static_cast<ClassId>(i);
        alloc.releaseSlab(pid, cid, SlabReleaseMode::kRebalance);

        ++i;
      }
    };

    auto allocateItem1 =
        std::async(std::launch::async, allocFn, std::string{"hello"},
                   std::vector<uint32_t>{100, 500, 1000});
    auto allocateItem2 =
        std::async(std::launch::async, allocFn, std::string{"world"},
                   std::vector<uint32_t>{200, 1000, 2000});
    auto allocateItem3 =
        std::async(std::launch::async, allocFn, std::string{"yolo"},
                   std::vector<uint32_t>{100, 200, 5000});
    auto slabRelease = std::async(releaseFn);

    slabRelease.wait();
    allocateItem1.wait();
    allocateItem2.wait();
    allocateItem3.wait();

    // Verify items have been evicted
    ASSERT_LT(0, numRemovedKeys);
  }

  // Add in one thread. Pop in the other. Make sure things stay consistent
  void testAddAndPopChainedItemMultithread() {
    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(100 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc.addPool("one", poolSize);

    auto addFn = [&] {
      auto itemHandle = alloc.findToWrite("parent");
      for (unsigned int j = 0; j < 100000; ++j) {
        auto childItem = alloc.allocateChainedItem(itemHandle, 100);
        ASSERT_NE(nullptr, childItem);

        alloc.addChainedItem(itemHandle, std::move(childItem));
      }
    };

    auto popFn = [&] {
      auto itemHandle = alloc.findToWrite("parent");
      for (unsigned int j = 0; j < 100000; ++j) {
        alloc.popChainedItem(itemHandle);
      }
    };

    util::allocateAccessible(alloc, pid, "parent", 100);

    // Run add once so we always have enough elements to pop
    addFn();

    // Run add and pop in two threads
    // We should end up with exactly 10000 chained items
    auto t1 = std::async(std::launch::async, addFn);
    auto t2 = std::async(std::launch::async, popFn);

    t1.wait();
    t2.wait();

    auto parentHandle = alloc.find("parent");
    ASSERT_NE(nullptr, parentHandle);

    auto headChainedItemHandle = alloc.findChainedItem(*parentHandle);
    ASSERT_NE(nullptr, headChainedItemHandle);

    auto* curr = &headChainedItemHandle->asChainedItem();
    int count = 0;
    while (curr) {
      ++count;
      curr = curr->getNext(alloc.compressor_);
    }

    ASSERT_EQ(100000, count);
  }

  // Using different hasher upon a warm roll should fail
  void testSerializationWithDifferentHasher() {
    const size_t nSlabs = 20;
    const size_t size = nSlabs * Slab::kSize;

    // Test allocations. These allocations should remain after save/restore.
    // Original lru allocator
    typename AllocatorT::Config config;
    config.setCacheSize(size);
    config.enableCachePersistence(this->cacheDir_);
    std::vector<typename AllocatorT::Key> keys;
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);

      // save
      alloc.shutDown();
    }

    testShmIsNotRemoved(config);

    // Restoring with same hasher should succeed
    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);

      // save
      alloc.shutDown();
    }

    testShmIsNotRemoved(config);

    // Restoring with different hasher should fail
    {
      config.setAccessConfig(typename AllocatorT::AccessConfig(
          10, 5, std::make_shared<FNVHash>()));
      ASSERT_THROW(AllocatorT(AllocatorT::SharedMemAttach, config),
                   std::invalid_argument);
    }

    testShmIsRemoved(config);
  }

  // Test memory fragmentation size after shut down.
  void testSerializationWithFragmentation() {
    typename AllocatorT::Config config;
    const size_t nSlabs = 40;
    const size_t size = nSlabs * Slab::kSize;
    const unsigned int keyLen = 100;

    config.setCacheSize(size);
    config.enableCachePersistence(this->cacheDir_);

    uint8_t poolId1, poolId2;
    uint64_t fragSize1 = 0;
    uint64_t fragSize2 = 0;

    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;

      // add 2 pools and fill them up.
      poolId1 = alloc.addPool("1", numBytes / 2);
      poolId2 = alloc.addPool("2", numBytes / 2);
      auto sizes = this->getValidAllocSizes(alloc, poolId1, nSlabs / 2, keyLen);
      this->fillUpPoolUntilEvictions(alloc, poolId1, sizes, keyLen);
      sizes = this->getValidAllocSizes(alloc, poolId2, nSlabs / 2, keyLen);
      this->fillUpPoolUntilEvictions(alloc, poolId2, sizes, keyLen);

      // store the fragmentation size.
      fragSize1 = alloc.getPoolStats(poolId1).totalFragmentation();
      fragSize2 = alloc.getPoolStats(poolId2).totalFragmentation();

      ASSERT_GT(alloc.getPoolStats(poolId1).totalFragmentation(), 0);
      // shutdown and save
      alloc.shutDown();
    }

    testShmIsNotRemoved(config);

    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);

      // check the fragmentation size of pool stats.
      ASSERT_EQ(alloc.getPoolStats(poolId1).totalFragmentation(), fragSize1);
      ASSERT_EQ(alloc.getPoolStats(poolId2).totalFragmentation(), fragSize2);
    }

    testShmIsRemoved(config);
  }

  void testIsOnShm() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.enableCachePersistence(this->cacheDir_);

    // Create a new cache allocator on heap memory
    {
      AllocatorT alloc(config);
      ASSERT_FALSE(alloc.isOnShm());
    }

    // Create a new cache allocator on shared memeory
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      ASSERT_TRUE(alloc.isOnShm());
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
      ASSERT_TRUE(alloc.isOnShm());
    }

    // Restoring a saved cache allocator from shared memroy
    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);
      ASSERT_TRUE(alloc.isOnShm());
    }

    // Restoring a saved cache allocator from shared memroy
    {
      MemoryMonitor::Config memConfig;
      memConfig.mode = MemoryMonitor::ResidentMemory;
      memConfig.maxAdvisePercentPerIter = 1;
      memConfig.maxReclaimPercentPerIter = 1;
      memConfig.maxAdvisePercent = 10;
      memConfig.lowerLimitGB = 1;
      memConfig.upperLimitGB = 10;
      config.enableMemoryMonitor(std::chrono::seconds{2}, memConfig);
      AllocatorT alloc(config);
      ASSERT_TRUE(alloc.isOnShm());
    }
  }

  void testReplaceChainedItem() {
    // Add a new chained item
    // Replace it with a chained item of the same size
    // Replace it with a bigger chained item
    // All should succeed

    using Item = typename AllocatorT::Item;

    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);

    AllocatorT alloc(config);

    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const std::set<uint32_t> allocSizes = {100, 1000};
    const auto pid = alloc.addPool("one", poolSize, allocSizes);

    auto parentHandle = util::allocateAccessible(alloc, pid, "hello", 50);
    ASSERT_TRUE(parentHandle);

    Item* currChainedItem;
    {
      auto chained = alloc.allocateChainedItem(parentHandle, 50);
      ASSERT_TRUE(chained);
      *(chained->template getMemoryAs<char>()) = 'a';

      currChainedItem = chained.get();

      alloc.addChainedItem(parentHandle, std::move(chained));
    }

    {
      auto chained = alloc.allocateChainedItem(parentHandle, 50);
      ASSERT_TRUE(chained);
      *(chained->template getMemoryAs<char>()) = 'b';

      Item& oldItem = *currChainedItem;
      currChainedItem = chained.get();

      auto oldHandle =
          alloc.replaceChainedItem(oldItem, std::move(chained), *parentHandle);
      ASSERT_EQ(*oldHandle->template getMemoryAs<char>(), 'a');
    }

    {
      auto chained = alloc.allocateChainedItem(parentHandle, 500);
      ASSERT_TRUE(chained);
      *(chained->template getMemoryAs<char>()) = 'c';

      Item& oldItem = *currChainedItem;
      currChainedItem = chained.get();

      auto oldHandle =
          alloc.replaceChainedItem(oldItem, std::move(chained), *parentHandle);
      ASSERT_EQ(*oldHandle->template getMemoryAs<char>(), 'b');
    }
  }

  void testAddChainedItemMultithreadWithMoving() {
    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.configureChainedItems();

    // allocate enough size to make sure evictions never occur
    config.setCacheSize(200 * Slab::kSize);

    using Item = typename AllocatorT::Item;

    std::atomic<uint64_t> numRemovedKeys{0};
    config.setRemoveCallback(
        [&](const typename AllocatorT::RemoveCbData& /* data */) {
          ++numRemovedKeys;
        });

    std::atomic<uint64_t> numMoves{0};
    config.enableMovingOnSlabRelease([&](Item& oldItem, Item& newItem,
                                         Item* parentPtr) {
      assert(oldItem.getSize() == newItem.getSize());

      // If parentPtr is present, the item has to be a chained item.
      if (parentPtr != nullptr) {
        ASSERT_TRUE(oldItem.isChainedItem());
        uint8_t* buf = reinterpret_cast<uint8_t*>(oldItem.getMemory());
        uint8_t* parentBuf = reinterpret_cast<uint8_t*>(parentPtr->getMemory());
        // Make sure we are on the right parent.
        for (int k = 0; k < 100; k++) {
          ASSERT_EQ(buf[k], (k + (*parentBuf)) % 256);
        }
      } else {
        // If parentPtr is missing, the item mustn't be a chained item.
        ASSERT_FALSE(oldItem.isChainedItem());
      }

      std::memcpy(newItem.getMemory(), oldItem.getMemory(), oldItem.getSize());
      ++numMoves;
    });

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const std::set<uint32_t> allocSizes = {150, 250, 550, 1050, 2050, 5050};
    const auto pid = alloc.addPool("one", poolSize, allocSizes);

    // Allocate 10000 Parent items and for each parent item, 10 chained
    // allocations
    auto allocFn = [&](std::string keyPrefix, std::vector<uint32_t> sizes) {
      for (unsigned int loop = 0; loop < 10; ++loop) {
        for (unsigned int i = 0; i < 1000; ++i) {
          const auto key = keyPrefix + folly::to<std::string>(loop) + "_" +
                           folly::to<std::string>(i);

          typename AllocatorT::WriteHandle itemHandle;

          itemHandle = alloc.allocate(pid, key, sizes[0]);

          uint8_t* parentBuf =
              reinterpret_cast<uint8_t*>(itemHandle->getMemory());
          (*parentBuf) = static_cast<uint8_t>(i);
          alloc.insert(itemHandle);

          for (unsigned int j = 0; j < 10; ++j) {
            auto childItem =
                alloc.allocateChainedItem(itemHandle, sizes[j % sizes.size()]);
            ASSERT_NE(nullptr, childItem);

            uint8_t* buf = reinterpret_cast<uint8_t*>(childItem->getMemory());
            for (uint8_t k = 0; k < 100; ++k) {
              buf[k] = static_cast<uint8_t>((k + i) % 256);
            }

            alloc.addChainedItem(itemHandle, std::move(childItem));
          }
        }

        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    };

    // Release 5 slabs
    auto releaseFn = [&] {
      for (unsigned int i = 0; i < 5;) {
        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        ClassId cid = static_cast<ClassId>(i);
        alloc.releaseSlab(pid, cid, SlabReleaseMode::kRebalance);

        ++i;
      }
    };

    auto allocateItem1 =
        std::async(std::launch::async, allocFn, std::string{"hello"},
                   std::vector<uint32_t>{100, 500, 1000});
    auto allocateItem2 =
        std::async(std::launch::async, allocFn, std::string{"world"},
                   std::vector<uint32_t>{200, 1000, 2000});
    auto allocateItem3 =
        std::async(std::launch::async, allocFn, std::string{"yolo"},
                   std::vector<uint32_t>{100, 200, 5000});

    auto slabRelease = std::async(releaseFn);
    slabRelease.wait();

    allocateItem1.wait();
    allocateItem2.wait();
    allocateItem3.wait();

    // Verify items have only been moved but not evicted
    ASSERT_LT(0, numMoves);
    ASSERT_EQ(0, numRemovedKeys);

    auto lookupFn = [&](std::string keyPrefix) {
      for (unsigned int loop = 0; loop < 10; ++loop) {
        for (unsigned int i = 0; i < 1000; ++i) {
          const auto key = keyPrefix + folly::to<std::string>(loop) + "_" +
                           folly::to<std::string>(i);
          auto parent = alloc.findToWrite(key);
          uint64_t numChildren = 0;
          while (parent->hasChainedItem()) {
            auto child = alloc.popChainedItem(parent);
            ASSERT_NE(nullptr, child);
            ++numChildren;

            const uint8_t* buf =
                reinterpret_cast<const uint8_t*>(child->getMemory());
            for (uint8_t k = 0; k < 100; ++k) {
              ASSERT_EQ((k + i) % 256, buf[k]);
            }
          }

          // Verify we inded have 10 children items for each parent
          ASSERT_EQ(10, numChildren);
        }
      }
    };

    lookupFn("hello");
    lookupFn("world");
    lookupFn("yolo");
  }

  // This test first writes 50 bytes into each chained item
  // Then it saves a pointer to each chained item's memory into a vector
  //
  // Then after allocating 1000 parent items and each of their 10 chained
  // items, for each chained item's buffer, we write the second batch of
  // 50 bytes.
  //
  // The idea is with a user-level sync object, the two writes for each
  // chained item should be on the same chained item, since moving cannot
  // happen until the sync object is dropped.
  void testAddChainedItemMultithreadWithMovingAndSync() {
    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.configureChainedItems();

    // allocate enough size to make sure evictions never occur
    config.setCacheSize(200 * Slab::kSize);

    using Item = typename AllocatorT::Item;

    std::atomic<uint64_t> numRemovedKeys{0};
    config.setRemoveCallback(
        [&](const typename AllocatorT::RemoveCbData&) { ++numRemovedKeys; });

    std::mutex m;
    struct TestSyncObj : public AllocatorT::SyncObj {
      std::lock_guard<std::mutex> l;
      TestSyncObj(std::mutex& m) : l(m) {}
    };
    std::atomic<uint64_t> numMoves{0};
    config.enableMovingOnSlabRelease(
        [&](Item& oldItem, Item& newItem, Item* /* parentPtr */) {
          assert(oldItem.getSize() == newItem.getSize());
          std::memcpy(newItem.getMemory(), oldItem.getMemory(),
                      oldItem.getSize());
          ++numMoves;
        },
        [&m](typename Item::Key) { return std::make_unique<TestSyncObj>(m); });

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const std::set<uint32_t> allocSizes = {150, 250, 550, 1050, 2050, 5050};
    const auto pid = alloc.addPool("one", poolSize, allocSizes);

    // Allocate 10000 Parent items and for each parent item, 10 chained
    // allocations
    auto allocFn = [&](std::string keyPrefix, std::vector<uint32_t> sizes) {
      for (unsigned int loop = 0; loop < 10; ++loop) {
        std::vector<uint8_t*> bufList;
        std::vector<typename AllocatorT::WriteHandle> parentHandles;
        std::unique_lock<std::mutex> l(m);
        for (unsigned int i = 0; i < 1000; ++i) {
          const auto key = keyPrefix + folly::to<std::string>(loop) + "_" +
                           folly::to<std::string>(i);

          auto itemHandle = util::allocateAccessible(alloc, pid, key, sizes[0]);

          for (unsigned int j = 0; j < 10; ++j) {
            auto childItem =
                alloc.allocateChainedItem(itemHandle, sizes[j % sizes.size()]);
            ASSERT_NE(nullptr, childItem);

            uint8_t* buf = reinterpret_cast<uint8_t*>(childItem->getMemory());
            // write first 50 bytes here
            for (uint8_t k = 0; k < 50; ++k) {
              buf[k] = k;
            }
            bufList.push_back(buf);

            alloc.addChainedItem(itemHandle, std::move(childItem));
          }
          parentHandles.push_back(std::move(itemHandle));
        }

        // Without sync object, we could be writing to already freed
        // memory here, since moving could've already freed the original
        // chained items and moved them to new ones
        //
        // But since we're holding a sync object here, we're safe to
        // write to these bufs.
        for (auto buf : bufList) {
          // write the second 50 bytes here
          for (uint8_t k = 50; k < 100; ++k) {
            buf[k] = k;
          }
        }

        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    };

    // Release 5 slabs
    auto releaseFn = [&] {
      for (unsigned int i = 0; i < 5;) {
        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        ClassId cid = static_cast<ClassId>(i);
        alloc.releaseSlab(pid, cid, SlabReleaseMode::kRebalance);

        ++i;
      }
    };

    auto allocateItem1 =
        std::async(std::launch::async, allocFn, std::string{"hello"},
                   std::vector<uint32_t>{100, 500, 1000});
    auto allocateItem2 =
        std::async(std::launch::async, allocFn, std::string{"world"},
                   std::vector<uint32_t>{200, 1000, 2000});
    auto allocateItem3 =
        std::async(std::launch::async, allocFn, std::string{"yolo"},
                   std::vector<uint32_t>{100, 200, 5000});

    auto slabRelease = std::async(releaseFn);
    slabRelease.wait();

    allocateItem1.wait();
    allocateItem2.wait();
    allocateItem3.wait();

    // Verify items have only been moved but not evicted
    ASSERT_EQ(0, numRemovedKeys);

    auto lookupFn = [&](std::string keyPrefix) {
      for (unsigned int loop = 0; loop < 10; ++loop) {
        for (unsigned int i = 0; i < 1000; ++i) {
          const auto key = keyPrefix + folly::to<std::string>(loop) + "_" +
                           folly::to<std::string>(i);
          auto parent = alloc.findToWrite(key);
          uint64_t numChildren = 0;
          while (parent->hasChainedItem()) {
            auto child = alloc.popChainedItem(parent);
            ASSERT_NE(nullptr, child);
            ++numChildren;

            const uint8_t* buf =
                reinterpret_cast<const uint8_t*>(child->getMemory());
            for (uint8_t k = 0; k < 100; ++k) {
              ASSERT_EQ(k, buf[k]);
            }
          }

          // Verify we inded have 10 children items for each parent
          ASSERT_EQ(10, numChildren);
        }
      }
    };

    lookupFn("hello");
    lookupFn("world");
    lookupFn("yolo");
  }

  // while a chained item could be moved - it is sync on parent moving bit.
  // try to transfer its parent after we moved and
  // validate that transfer succeeds correctly.
  void testTransferChainAfterMoving() {
    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.configureChainedItems();

    // allocate enough size to make sure evictions never occur
    config.setCacheSize(200 * Slab::kSize);

    using Item = typename AllocatorT::Item;

    std::atomic<uint64_t> numRemovedKeys{0};
    config.setRemoveCallback(
        [&](const typename AllocatorT::RemoveCbData&) { ++numRemovedKeys; });

    std::string movingKey = "helloworldmoving";

    std::atomic<uint64_t> numMoves{0};
    config.enableMovingOnSlabRelease(
        [&](Item& oldItem, Item& newItem, Item* /* parentPtr */) {
          XDCHECK_EQ(oldItem.getSize(), newItem.getSize());
          XDCHECK_EQ(oldItem.getKey(), newItem.getKey());
          std::memcpy(newItem.getMemory(), oldItem.getMemory(),
                      oldItem.getSize());
          ++numMoves;
        },
        {});

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const std::set<uint32_t> allocSizes = {250, 2050};
    const auto pid = alloc.addPool("one", poolSize, allocSizes);

    void* childPtr = nullptr;
    {
      auto parent = alloc.allocate(pid, movingKey, 1000);
      auto child = alloc.allocateChainedItem(parent, 200);
      childPtr = child.get();
      alloc.addChainedItem(parent, std::move(child));
      alloc.insertOrReplace(parent);
    }

    XLOG(INFO) << childPtr;
    // Release slab corresponding to the child to trigger a move
    auto releaseFn = [&alloc, childPtr, pid] {
      alloc.releaseSlab(pid, alloc.getAllocInfo(childPtr).classId,
                        SlabReleaseMode::kRebalance, childPtr);
    };

    auto slabRelease = std::async(releaseFn);

    // wait for slab release to complete.
    slabRelease.wait();

    // we know moving sync is held now.
    {
      auto newParent = alloc.allocate(pid, movingKey, 600);
      // parent is marked moving during moved, once finished we will get handle
      auto parent = alloc.findToWrite(movingKey);
      alloc.transferChainAndReplace(parent, newParent);
    }

    EXPECT_EQ(numMoves, 1);
    auto slabReleaseStats = alloc.getSlabReleaseStats();
    EXPECT_EQ(slabReleaseStats.numMoveAttempts, 1);
    EXPECT_EQ(slabReleaseStats.numMoveSuccesses, 1);

    auto handle = alloc.find(movingKey);
    auto chainedAllocs = alloc.viewAsChainedAllocs(handle);
    for (const auto& c : chainedAllocs.getChain()) {
      EXPECT_NE(&c, childPtr);
    }
  }

  // Test stats on counting chained items
  // 1. Alloc an item and several chained items
  //    * Before inserting chained items, make sure count is zero
  //    * Verify count after insertions
  // 2. Remove the parent item then verify the count
  void testAllocChainedCount() {
    // the eviction call back to keep track of the evicted keys.
    std::set<std::string> removedKeys;
    auto removeCb =
        [&removedKeys](const typename AllocatorT::RemoveCbData& data) {
          const auto k = data.item.getKey();
          removedKeys.insert({k.data(), k.size()});
        };

    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setRemoveCallback(removeCb);
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc.addPool("one", poolSize);

    const auto size = 100;
    auto itemHandle = util::allocateAccessible(alloc, pid, "hello", size);
    auto chainedItemHandle = alloc.allocateChainedItem(itemHandle, size * 2);
    auto chainedItemHandle2 = alloc.allocateChainedItem(itemHandle, size * 4);
    auto chainedItemHandle3 = alloc.allocateChainedItem(itemHandle, size * 8);

    ASSERT_NE(nullptr, chainedItemHandle);
    ASSERT_NE(nullptr, chainedItemHandle2);
    ASSERT_NE(nullptr, chainedItemHandle3);
    ASSERT_EQ(size * 2, chainedItemHandle->getSize());
    ASSERT_EQ(size * 4, chainedItemHandle2->getSize());
    ASSERT_EQ(size * 8, chainedItemHandle3->getSize());

    // Add chained items
    ASSERT_EQ(alloc.getGlobalCacheStats().numChainedParentItems, 0);
    ASSERT_EQ(alloc.getGlobalCacheStats().numChainedChildItems, 0);
    alloc.addChainedItem(itemHandle, std::move(chainedItemHandle));
    alloc.addChainedItem(itemHandle, std::move(chainedItemHandle2));
    alloc.addChainedItem(itemHandle, std::move(chainedItemHandle3));
    ASSERT_EQ(alloc.getGlobalCacheStats().numChainedParentItems, 1);
    ASSERT_EQ(alloc.getGlobalCacheStats().numChainedChildItems, 3);

    {
      auto stats = alloc.getPoolStats(pid);
      ASSERT_EQ(0, stats.numFreeAllocs());
      ASSERT_EQ(4, stats.numActiveAllocs());
    }

    // Remove the children
    for (unsigned int i = 0; i < 3; i++) {
      alloc.popChainedItem(itemHandle);
      ASSERT_EQ(alloc.getGlobalCacheStats().numChainedChildItems, 2 - i);
    }
    // The handle is no longer a parent
    ASSERT_EQ(alloc.getGlobalCacheStats().numChainedParentItems, 0);
    {
      auto stats = alloc.getPoolStats(pid);
      ASSERT_EQ(3, stats.numFreeAllocs());
      ASSERT_EQ(1, stats.numActiveAllocs());
    }

    // Remove the item, but this shouldn't free it since we still have a
    // handle Add it back
    auto chainedItemHandle4 = alloc.allocateChainedItem(itemHandle, size * 8);
    alloc.addChainedItem(itemHandle, std::move(chainedItemHandle4));
    alloc.remove("hello");
    ASSERT_TRUE(removedKeys.empty());
    ASSERT_EQ(alloc.getGlobalCacheStats().numChainedParentItems, 1);
    ASSERT_EQ(alloc.getGlobalCacheStats().numChainedChildItems, 1);
    itemHandle.reset();
    ASSERT_FALSE(removedKeys.empty());
    ASSERT_EQ(alloc.getGlobalCacheStats().numChainedParentItems, 0);
    ASSERT_EQ(alloc.getGlobalCacheStats().numChainedChildItems, 0);
    {
      auto stats = alloc.getPoolStats(pid);
      ASSERT_EQ(4, stats.numFreeAllocs());
      ASSERT_EQ(0, stats.numActiveAllocs());
    }
  }

  // Test stats count on chained items across multiple threads.
  void testCountItemsMultithread() {
    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.configureChainedItems();
    config.setCacheSize(300 * Slab::kSize);
    // Disable slab rebalancing
    config.enablePoolRebalancing(nullptr, std::chrono::seconds{0});
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc.addPool("one", poolSize);
    int itemSize = 100;
    // Thread to allocate chained child items
    int childCount = 17;
    auto addChild = [&] {
      auto itemHandle = alloc.findToWrite("parent");
      for (int j = 0; j < childCount; j++) {
        auto childItem = alloc.allocateChainedItem(itemHandle, itemSize);
        ASSERT_NE(nullptr, childItem);
        alloc.addChainedItem(itemHandle, std::move(childItem));
      }
    };
    // -- Chained Test --
    util::allocateAccessible(alloc, pid, "parent", 100);
    auto tc1 = std::async(std::launch::async, addChild);
    auto tc2 = std::async(std::launch::async, addChild);
    tc1.wait();
    tc2.wait();

    ASSERT_EQ(1, alloc.getGlobalCacheStats().numChainedParentItems);
    ASSERT_EQ(childCount * 2, alloc.getGlobalCacheStats().numChainedChildItems);
  }

  // Test chained item count consistency after shutdown and restore
  void testItemCountCreationTime() {
    typename AllocatorT::Config config;
    uint8_t poolId;
    config.setCacheSize(2 * Slab::kSize);
    auto ccItemCount = 7;
    config.configureChainedItems();
    config.enableCachePersistence(this->cacheDir_);

    // Disable slab rebalancing
    config.enablePoolRebalancing(nullptr, std::chrono::seconds{0});

    // Test allocations. These allocations should remain after save/restore.
    // Original lru allocator
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      poolId = alloc.addPool("foobar", numBytes);

      // Allocate chained items
      {
        auto pHandle = util::allocateAccessible(alloc, poolId, "parent", 10);
        for (int j = 0; j < ccItemCount; j++) {
          auto childItem = alloc.allocateChainedItem(pHandle, 10);
          ASSERT_NE(nullptr, childItem);
          alloc.addChainedItem(pHandle, std::move(childItem));
        }
      }

      // save
      alloc.shutDown();
    }

    /* sleep override */ std::this_thread::sleep_for(std::chrono::seconds(3));

    // Restore lru allocator, check count
    {
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);
      ASSERT_EQ(1, alloc.getGlobalCacheStats().numChainedParentItems);
      ASSERT_EQ(ccItemCount, alloc.getGlobalCacheStats().numChainedChildItems);
      alloc.shutDown();
    }

    // create new and ensure that the count is reset
    AllocatorT alloc(AllocatorT::SharedMemNew, config);
    ASSERT_NE(1, alloc.getGlobalCacheStats().numChainedParentItems);
    ASSERT_NE(ccItemCount, alloc.getGlobalCacheStats().numChainedChildItems);
  }

  void testEvictionAgeStats() {
    // create an allocator worth 10 slabs.
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const std::set<uint32_t> allocSizes{100};
    typename AllocatorT::MMConfig mmConfig;
    mmConfig.lruRefreshTime = 0;
    const auto poolId =
        alloc.addPool("default", alloc.getCacheMemoryStats().ramCacheSize,
                      allocSizes, mmConfig);

    {
      const auto stats = alloc.getPoolEvictionAgeStats(poolId, 0);
      ASSERT_EQ(0, stats.getOldestElementAge(0));
    }

    // insert some items (need to make sure at least 1 is in warm)
    ASSERT_TRUE(util::allocateAccessible(alloc, poolId, "I'm a key", 10));
    ASSERT_TRUE(util::allocateAccessible(alloc, poolId, "I'm another key", 10));
    ASSERT_TRUE(util::allocateAccessible(alloc, poolId, "I'm a third key", 10));
    ASSERT_TRUE(
        util::allocateAccessible(alloc, poolId, "I'm a fourth key", 10));
    ASSERT_NE(nullptr, alloc.find("I'm a key"));
    std::this_thread::sleep_for(std::chrono::seconds(10));
    {
      const auto stats = alloc.getPoolEvictionAgeStats(poolId, 0);
      ASSERT_LE(10, stats.getOldestElementAge(0));
    }
  }

  void testReplaceInMMContainer() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.configureChainedItems();
    AllocatorT alloc(config);
    const auto poolId =
        alloc.addPool("default", alloc.getCacheMemoryStats().ramCacheSize);

    // Make sure replaceInMMContainer correctly handles items in the same or
    // different mm containers
    {
      auto small = util::allocateAccessible(alloc, poolId, "hello", 10);
      auto big = alloc.allocate(poolId, "hello", 10000);

      alloc.replaceInMMContainer(*small, *big);
      ASSERT_FALSE(small->isInMMContainer());
      ASSERT_TRUE(big->isInMMContainer());

      auto& mmContainer = alloc.getMMContainer(*big);
      auto itr = mmContainer.getEvictionIterator();
      ASSERT_EQ(big.get(), &(*itr));

      alloc.remove("hello");
    }

    {
      auto small1 = util::allocateAccessible(alloc, poolId, "hello", 10);
      auto small2 = alloc.allocate(poolId, "hello", 10);

      alloc.replaceInMMContainer(*small1, *small2);
      ASSERT_FALSE(small1->isInMMContainer());
      ASSERT_TRUE(small2->isInMMContainer());

      auto& mmContainer = alloc.getMMContainer(*small2);
      auto itr = mmContainer.getEvictionIterator();
      ASSERT_EQ(small2.get(), &(*itr));

      alloc.remove("hello");
    }
  }

  void testReplaceIfAccessible() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.configureChainedItems();
    AllocatorT alloc(config);
    const auto poolId =
        alloc.addPool("default", alloc.getCacheMemoryStats().ramCacheSize);

    // Normal item
    {
      auto notInserted = alloc.allocate(poolId, "hello", 100);
      auto newItem = alloc.allocate(poolId, "hello", 100);
      ASSERT_FALSE(alloc.replaceIfAccessible(*notInserted, *newItem));
      ASSERT_EQ(nullptr, alloc.find("hello"));

      auto inserted = util::allocateAccessible(alloc, poolId, "hello", 100);
      ASSERT_TRUE(alloc.replaceIfAccessible(*inserted, *newItem));
      ASSERT_EQ(newItem, alloc.find("hello"));
    }

    // Chained Items
    {
      auto notInserted = alloc.allocate(poolId, "hello2", 100);
      auto notInsertedChild = alloc.allocateChainedItem(notInserted, 100);
      alloc.addChainedItem(notInserted, std::move(notInsertedChild));
      ASSERT_TRUE(notInserted->hasChainedItem());
      ASSERT_EQ(nullptr, alloc.find("hello2"));

      auto notInsertedNew = alloc.allocate(poolId, "hello2", 100);
      ASSERT_FALSE(notInsertedNew->hasChainedItem());

      alloc.transferChainAndReplace(notInserted, notInsertedNew);
      ASSERT_TRUE(notInsertedNew->hasChainedItem());
      ASSERT_EQ(nullptr, alloc.find("hello2"));

      auto inserted = util::allocateAccessible(alloc, poolId, "hello2", 100);
      auto insertedChild = alloc.allocateChainedItem(inserted, 100);
      alloc.addChainedItem(inserted, std::move(insertedChild));
      ASSERT_TRUE(inserted->hasChainedItem());
      ASSERT_EQ(inserted, alloc.find("hello2"));

      auto insertedNew = alloc.allocate(poolId, "hello2", 100);
      ASSERT_FALSE(insertedNew->hasChainedItem());

      alloc.transferChainAndReplace(inserted, insertedNew);
      ASSERT_TRUE(insertedNew->hasChainedItem());
      ASSERT_EQ(insertedNew, alloc.find("hello2"));

      auto insertedNew2 = alloc.allocate(poolId, "hello2", 10000);
      ASSERT_FALSE(insertedNew2->hasChainedItem());

      alloc.transferChainAndReplace(insertedNew, insertedNew2);
      ASSERT_TRUE(insertedNew2->hasChainedItem());
      ASSERT_EQ(insertedNew2, alloc.find("hello2"));
    }
  }

  void testChainedItemIterator() {
    typename AllocatorT::Config config;
    using Iterator =
        CacheChainedItemIterator<AllocatorT, const typename AllocatorT::Item>;

    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc.addPool("one", poolSize);

    const auto size = 100;
    auto itemHandle = util::allocateAccessible(alloc, pid, "hello", size);
    auto chainedItemHandle = alloc.allocateChainedItem(itemHandle, size);
    auto chainedItemHandle2 = alloc.allocateChainedItem(itemHandle, size);
    auto chainedItemHandle3 = alloc.allocateChainedItem(itemHandle, size);

    alloc.addChainedItem(itemHandle, std::move(chainedItemHandle));
    alloc.addChainedItem(itemHandle, std::move(chainedItemHandle2));
    alloc.addChainedItem(itemHandle, std::move(chainedItemHandle3));

    auto item = itemHandle.get();
    auto chainedItem = alloc.findChainedItem(*item);
    auto compressor = alloc.createPtrCompressor();
    Iterator it(&(chainedItem.get()->asChainedItem()), compressor);
    Iterator chainIterEnd(nullptr, compressor);

    int count = 0;
    for (; it != chainIterEnd; it++) {
      count++;
    }

    ASSERT_EQ(count, 3);
  }

  void testChainIteratorInvalidArg() {
    typename AllocatorT::Config config;
    using Iterator =
        CacheChainedItemIterator<AllocatorT, const typename AllocatorT::Item>;

    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc.addPool("one", poolSize);

    const auto size = 100;
    auto itemHandle = util::allocateAccessible(alloc, pid, "hello", size);
    auto item = itemHandle.get();
    auto compressor = alloc.createPtrCompressor();
    EXPECT_THROW(Iterator(item, compressor), std::invalid_argument);
  }

  void testRemoveCbChainedItems() {
    typename AllocatorT::Config config;

    bool found = false;
    auto removeCb = [&found](const typename AllocatorT::RemoveCbData& data) {
      int count = 0;
      auto it = data.chainedAllocs.begin();
      for (; it != data.chainedAllocs.end(); it++) {
        count++;
      }
      found = count == 3;
    };

    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    config.setRemoveCallback(removeCb);
    AllocatorT alloc(config);

    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc.addPool("one", poolSize);

    const auto size = 100;
    {
      auto itemHandle = util::allocateAccessible(alloc, pid, "hello", size);
      auto chainedItemHandle = alloc.allocateChainedItem(itemHandle, size);
      auto chainedItemHandle2 = alloc.allocateChainedItem(itemHandle, size);
      auto chainedItemHandle3 = alloc.allocateChainedItem(itemHandle, size);

      alloc.addChainedItem(itemHandle, std::move(chainedItemHandle));
      alloc.addChainedItem(itemHandle, std::move(chainedItemHandle2));
      alloc.addChainedItem(itemHandle, std::move(chainedItemHandle3));

      // Expect that all 3 chained items associated with itemHandle will be
      // seen by removeCbNew
      alloc.remove(itemHandle.get()->getKey());
    } // scope for item handle to trigger remove CB
    ASSERT_TRUE(found);
  }

  void testRemoveCbNoChainedItems() {
    typename AllocatorT::Config config;
    auto removeCb = [](const typename AllocatorT::RemoveCbData& data) {
      int count = 0;
      auto it = data.chainedAllocs.begin();
      for (; it != data.chainedAllocs.end(); it++) {
        count++;
      }
      ASSERT_EQ(count, 0);
    };
    config.configureChainedItems();
    config.setCacheSize(10 * Slab::kSize);
    config.setRemoveCallback(removeCb);
    AllocatorT alloc(config);

    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc.addPool("one", poolSize);

    const auto size = 100;
    auto itemHandle = util::allocateAccessible(alloc, pid, "hello", size);

    // Expect that removeCbNew.chainedAllocs will have empty iterator range
    alloc.remove(itemHandle.get()->getKey());
  }

  void testDumpEvictionIterator() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes;

    const auto pid = alloc.addPool("one", poolSize);

    {
      auto tail = alloc.dumpEvictionIterator(
          pid, 0 /* first allocation class */, 20 /* last 20 items */);
      ASSERT_EQ(0, tail.size());
    }

    for (unsigned int i = 0; i < 30; ++i) {
      util::allocateAccessible(alloc, pid, util::castToKey(i), 0);
    }

    {
      auto tail = alloc.dumpEvictionIterator(
          pid, 0 /* first allocation class */, 20 /* last 20 items */);
      ASSERT_EQ(20, tail.size());
    }
  }

  void testGetEvictionCandidateKeys() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    AllocatorT alloc(config);
    const auto pid =
        alloc.addPool("one", alloc.getCacheMemoryStats().ramCacheSize);

    const std::vector<std::string> expected{"key0", "key1", "key2"};
    for (const auto& key : expected) {
      util::allocateAccessible(alloc, pid, key, 10);
    }
    auto held = util::allocateAccessible(alloc, pid, "held", 10);
    const auto cid = alloc.getAllocInfo(held->getMemory()).classId;
    {
      auto parent = util::allocateAccessible(alloc, pid, "parent", 10);
      auto child = alloc.allocateChainedItem(parent, 10);
      ASSERT_EQ(cid, alloc.getAllocInfo(parent->getMemory()).classId);
      ASSERT_EQ(cid, alloc.getAllocInfo(child->getMemory()).classId);
      alloc.addChainedItem(parent, std::move(child));
    }

    auto keys = alloc.getEvictionCandidateKeys(pid, cid, 10, 10);
    std::sort(keys.begin(), keys.end());
    auto expectedWithParent = expected;
    expectedWithParent.push_back("parent");
    EXPECT_EQ(expectedWithParent, keys);
    auto unchainedKeys = alloc.getEvictionCandidateKeys(
        pid, cid, 10, 10, false /* includeChainedParents */);
    std::sort(unchainedKeys.begin(), unchainedKeys.end());
    EXPECT_EQ(expected, unchainedKeys);
    EXPECT_EQ(2, alloc.getEvictionCandidateKeys(pid, cid, 2, 10).size());
    EXPECT_TRUE(alloc.getEvictionCandidateKeys(pid, cid, 10, 0).empty());
    for (const auto& key : expected) {
      alloc.remove(key);
    }
    EXPECT_TRUE(alloc
                    .getEvictionCandidateKeys(pid, cid, 10, 10,
                                              false /* includeChainedParents */)
                    .empty());
  }

  void testMMReconfigure() {
    typename AllocatorT::Config config;
    typename AllocatorT::MMConfig mmConfig;
    mmConfig.defaultLruRefreshTime = 3;
    mmConfig.lruRefreshRatio = 0.7;
    mmConfig.mmReconfigureIntervalSecs = std::chrono::seconds(2);
    auto checkItemKey = [&](std::string itemString, std::string key) {
      return itemString.find(key) != std::string::npos;
    };
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes / 2;
    std::string key1 = "key1-some-random-string-here";
    auto poolId = alloc.addPool("one", poolSize, {} /* allocSizes */, mmConfig);
    auto handle1 = alloc.allocate(poolId, key1, 1);
    alloc.insert(handle1);
    auto handle2 = alloc.allocate(poolId, "key2", 1);
    alloc.insert(handle2);
    sleep(9);

    // upon access, refresh time will be recaculatd to 9 * 0.7 = 5.6 = 6
    ASSERT_NE(alloc.find(key1), nullptr);
    auto tail = alloc.dumpEvictionIterator(
        poolId, 0 /* first allocation class */, 2 /* last 2 items */);
    EXPECT_TRUE(checkItemKey(tail[1], key1));

    sleep(5);
    auto handle3 = alloc.allocate(poolId, "key3", 1);
    alloc.insert(handle3);
    ASSERT_NE(alloc.find(key1), nullptr);
    tail = alloc.dumpEvictionIterator(poolId, 0 /* first allocation class */,
                                      2 /* last 2 items */);
    // refresh time now 6, item 1 age 5, not promoted (INCREASE)
    EXPECT_TRUE(checkItemKey(tail[1], key1));

    alloc.remove("key2");
    sleep(3);

    ASSERT_NE(alloc.find(key1), nullptr);
    tail = alloc.dumpEvictionIterator(poolId, 0 /* first allocation class */,
                                      2 /* last 2 items */);
    // tail age 8, lru refresh time 6, item 1 age 8, promoted (DECREASE)
    EXPECT_TRUE(checkItemKey(tail[1], key1));
  }

  // This test verifies that with 2Q container, the background worker can work
  // as expected, i.e. wake up once in a while and call reconfigure() to
  // re-compute lru refresh time for the container.
  //
  // 2Q need special cases because we query warm queue for tail age, instead
  // of cold or hot
  void testMM2QReconfigure(typename AllocatorT::MMConfig mmConfig) {
    typename AllocatorT::Config config;
    mmConfig.defaultLruRefreshTime = 3;
    // default lruRefreshTime is 60 secs
    mmConfig.lruRefreshTime = 3;
    mmConfig.lruRefreshRatio = 0.7;
    mmConfig.mmReconfigureIntervalSecs = std::chrono::seconds(2);
    auto checkItemKey = [&](std::string itemString, std::string key) {
      return itemString.find(key) != std::string::npos;
    };
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const auto poolSize = numBytes / 2;
    std::string key1 = "key1-some-random-string-here";
    std::string key3 = "key3-some-random-string-here";
    auto poolId = alloc.addPool("one", poolSize, {} /* allocSizes */, mmConfig);
    auto handle1 = alloc.allocate(poolId, key1, 1);
    alloc.insert(handle1);
    auto handle2 = alloc.allocate(poolId, "key2", 1);
    alloc.insert(handle2);
    ASSERT_NE(alloc.find("key2"), nullptr);
    sleep(9);

    ASSERT_NE(alloc.find(key1), nullptr);
    auto tail = alloc.dumpEvictionIterator(
        poolId, 0 /* first allocation class */, 3 /* last 3 items */);
    // item 1 gets promoted (age 9), tail age 9, lru refresh time 3 (default)
    EXPECT_TRUE(checkItemKey(tail[1], key1));

    auto handle3 = alloc.allocate(poolId, key3, 1);
    alloc.insert(handle3);

    sleep(6);
    tail = alloc.dumpEvictionIterator(poolId, 0 /* first allocation class */,
                                      3 /* last 3 items */);
    ASSERT_NE(alloc.find(key3), nullptr);
    tail = alloc.dumpEvictionIterator(poolId, 0 /* first allocation class */,
                                      3 /* last 3 items */);
    // tail age 15, lru refresh time 6 * 0.7 = 4.2 = 4,
    // item 3 age 6 gets promoted
    EXPECT_TRUE(checkItemKey(tail[1], key1));

    alloc.remove("key2");
    sleep(3);

    ASSERT_NE(alloc.find(key3), nullptr);
    tail = alloc.dumpEvictionIterator(poolId, 0 /* first allocation class */,
                                      2 /* last 2 items */);
    // tail age 9, lru refresh time 4, item 3 age 3, not promoted
    EXPECT_TRUE(checkItemKey(tail[1], key3));
  }

  void testConfigValidation() {
    typename AllocatorT::Config config;
    using MMType = typename AllocatorT::MMType;
    // posix shm can only be enabld if cache persistence is enabled
    EXPECT_THROW(config.usePosixForShm(), std::invalid_argument);
    config.enableCachePersistence(this->cacheDir_);
    EXPECT_NO_THROW(config.usePosixForShm());

    MarginalHitsStrategy::Config strategyConfig{};
    auto strategy = std::make_shared<MarginalHitsStrategy>(strategyConfig);
    MarginalHitsOptimizeStrategy::Config optimizeStrategyConfig{};
    auto optimizeStrategy =
        std::make_shared<MarginalHitsOptimizeStrategy>(optimizeStrategyConfig);
    auto freeMemStrategy = std::make_shared<FreeMemStrategy>();
    std::shared_ptr<RebalanceStrategy> nullPtrRebalance{nullptr};
    std::shared_ptr<PoolOptimizeStrategy> nullPtrOptimize{nullptr};

    // default valid cases
    EXPECT_NO_THROW(config.validate());
    EXPECT_TRUE(config.validateStrategy(freeMemStrategy));
    EXPECT_TRUE(config.validateStrategy(nullPtrRebalance));
    EXPECT_TRUE(config.validateStrategy(nullPtrOptimize));

    // without traking tail hits enabled, cannot use marginal hits strategy
    EXPECT_FALSE(config.validateStrategy(strategy));
    EXPECT_FALSE(config.validateStrategy(optimizeStrategy));

    config.enableTailHitsTracking();

    // MarginalHitsStrategy can be used only if tracking tail hits is enabled
    EXPECT_TRUE(config.validateStrategy(strategy));
    EXPECT_TRUE(config.validateStrategy(optimizeStrategy));

    // can track tail hits only if MMType is MM2Q
    if (MMType::kId == MM2Q::kId) {
      EXPECT_NO_THROW(config.validate());
      // size cannot exceed the maximum cache size (274'877'906'944 bytes)
      config.setCacheSize(275'877'906'900);
      EXPECT_THROW(config.validate(), std::invalid_argument);
    } else {
      EXPECT_THROW(config.validate(), std::invalid_argument);
    }
  }

  void testCacheKeyValidity() {
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    AllocatorT alloc(config);

    {
      // valid case
      auto key = std::string{"a"};
      EXPECT_TRUE(alloc.isKeyValid(key));
      EXPECT_NO_THROW(alloc.throwIfKeyInvalid(key));
    }
    {
      // 1) invalid due to key length - verify exception message mentions the
      // root cause
      auto key = std::string(KAllocation::kKeyMaxLenSmall + 1, 'a');
      EXPECT_FALSE(alloc.isKeyValid(key));
      try {
        alloc.throwIfKeyInvalid(key);
        FAIL() << "Expected std::invalid_argument";
      } catch (const std::invalid_argument& e) {
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("exceeds maximum allowed") != std::string::npos)
            << "Exception message should mention key size exceeds maximum. "
               "Actual: "
            << msg;
      }
    }
    {
      // 2) invalid due to size being 0 - verify exception message mentions the
      // root cause
      auto string = std::string{"some string"};
      auto key = folly::StringPiece{string.data(), std::size_t{0}};
      EXPECT_FALSE(alloc.isKeyValid(key));
      try {
        alloc.throwIfKeyInvalid(key);
        FAIL() << "Expected std::invalid_argument";
      } catch (const std::invalid_argument& e) {
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("key is empty") != std::string::npos)
            << "Exception message should mention key is empty. Actual: " << msg;
      }
    }
    // Note: we don't test for a null stringpiece with positive size as the
    // key
    //       because folly::StringPiece now throws an exception for it
    {
      // 3) invalid due due a null key - verify exception message mentions the
      // root cause
      auto key = folly::StringPiece{nullptr, std::size_t{0}};
      EXPECT_FALSE(alloc.isKeyValid(key));
      try {
        alloc.throwIfKeyInvalid(key);
        FAIL() << "Expected std::invalid_argument";
      } catch (const std::invalid_argument& e) {
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("null data pointer") != std::string::npos)
            << "Exception message should mention null data pointer. Actual: "
            << msg;
      }
    }
  }

  void testLargeCacheKeyValidity() {
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    config.allowLargeKeys(true);
    AllocatorT alloc(config);

    {
      // valid case
      auto key = std::string{"a"};
      EXPECT_TRUE(alloc.isKeyValid(key));
      EXPECT_NO_THROW(alloc.throwIfKeyInvalid(key));
    }
    {
      // 1) invalid due to key length - verify exception message mentions the
      // root cause
      auto key = std::string(KAllocation::kKeyMaxLen + 1, 'a');
      EXPECT_FALSE(alloc.isKeyValid(key));
      try {
        alloc.throwIfKeyInvalid(key);
        FAIL() << "Expected std::invalid_argument";
      } catch (const std::invalid_argument& e) {
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("exceeds maximum allowed") != std::string::npos)
            << "Exception message should mention key size exceeds maximum. "
               "Actual: "
            << msg;
      }
    }
    {
      // 2) invalid due to size being 0 - verify exception message mentions the
      // root cause
      auto string = std::string{"some string"};
      auto key = folly::StringPiece{string.data(), std::size_t{0}};
      EXPECT_FALSE(alloc.isKeyValid(key));
      try {
        alloc.throwIfKeyInvalid(key);
        FAIL() << "Expected std::invalid_argument";
      } catch (const std::invalid_argument& e) {
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("key is empty") != std::string::npos)
            << "Exception message should mention key is empty. Actual: " << msg;
      }
    }
    // Note: we don't test for a null stringpiece with positive size as the
    // key
    //       because folly::StringPiece now throws an exception for it
    {
      // 3) invalid due due a null key - verify exception message mentions the
      // root cause
      auto key = folly::StringPiece{nullptr, std::size_t{0}};
      EXPECT_FALSE(alloc.isKeyValid(key));
      try {
        alloc.throwIfKeyInvalid(key);
        FAIL() << "Expected std::invalid_argument";
      } catch (const std::invalid_argument& e) {
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("null data pointer") != std::string::npos)
            << "Exception message should mention null data pointer. Actual: "
            << msg;
      }
    }
  }

  void testLargeKeys() {
    const size_t nSizes = 10;
    const size_t nTries = 10;
    // limit key size for the main part of the test to keep test time small
    const auto maxTestKeySize = KAllocation::kKeyMaxLen / 1024;
    size_t numSucceeded;
    size_t numFailed;

    auto allocateLargeKeys = [&](bool allowLargeKeys) {
      typename AllocatorT::Config config;
      config.setCacheSize(100 * Slab::kSize);
      config.setAllowLargeKeys(allowLargeKeys);
      AllocatorT alloc(config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      auto poolId = alloc.addPool("foobar", numBytes);

      const auto maxKeySize =
          allowLargeKeys ? maxTestKeySize : KAllocation::kKeyMaxLenSmall;
      const auto sizes =
          this->getValidAllocSizes(alloc, poolId, nSizes, maxKeySize);

      // empty the cache.
      for (const auto& it : alloc) {
        alloc.remove(it.getKey());
      }

      numSucceeded = 0;
      numFailed = 0;
      for (unsigned int i = 0; i < nTries; i++) {
        for (auto size : sizes) {
          auto keySize = folly::Random::rand32(KAllocation::kKeyMaxLenSmall + 1,
                                               maxTestKeySize);
          try {
            alloc.allocate(poolId, test_util::getRandomAsciiStr(keySize), size);
            numSucceeded++;
          } catch (const std::invalid_argument&) {
            numFailed++;
          }
        }

        for (const auto& it : alloc) {
          alloc.remove(it.getKey());
        }
      }

      EXPECT_EQ(alloc.getPoolStats(poolId).numActiveAllocs(), 0);
    };

    // disable large keys, all allocations should fail
    allocateLargeKeys(/* allowLargeKeys */ false);
    EXPECT_EQ(numSucceeded, 0);
    EXPECT_EQ(numFailed, nSizes * nTries);

    // enable large keys, all allocations should succeed
    allocateLargeKeys(/* allowLargeKeys */ true);
    EXPECT_EQ(numSucceeded, nSizes * nTries);
    EXPECT_EQ(numFailed, 0);

    // try allocating a really large key
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    config.setAllowLargeKeys(true);
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);
    const size_t mb = 1024 * 1024;
    alloc.allocate(poolId, test_util::getRandomAsciiStr(mb), /* size */ mb);
  }

  void testRebalanceByAllocFailure() {
    typename AllocatorT::Config config;
    const size_t nSlabs = 3;
    config.setCacheSize(nSlabs * Slab::kSize);
    AllocatorT alloc(config);

    const auto smallSize = 1024 * 1024;
    const auto largeSize = 3 * 1024 * 1024;
    const auto poolId =
        alloc.addPool("default", alloc.getCacheMemoryStats().ramCacheSize,
                      {smallSize + 100, largeSize + 100});

    // Allocate until the smaller objects fill up the cache
    std::vector<typename AllocatorT::WriteHandle> handles;
    for (int i = 0;; i++) {
      auto handle = util::allocateAccessible(
          alloc, poolId, fmt::format("small_{}", i), smallSize);
      if (handle == nullptr) {
        break;
      }
      handles.push_back(std::move(handle));
    }

    EXPECT_EQ(nullptr,
              util::allocateAccessible(alloc, poolId, "large", largeSize));
    handles.clear();
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
      if (util::allocateAccessible(alloc, poolId, "large", largeSize) !=
          nullptr) {
        break;
      }
    }
  }

  void testRebalanceWakeupAfterAllocFailure() {
    typename AllocatorT::Config config;
    const size_t nSlabs = 3;
    config.setCacheSize(nSlabs * Slab::kSize);
    // set a very long rebalance interval: 20 seconds
    config.enablePoolRebalancing(std::make_shared<RebalanceStrategy>(),
                                 std::chrono::seconds{20});
    AllocatorT alloc(config);

    // smaller items (e.g. 128) will cause longer time to evict in test mode,
    // to save testing time, use 1MB for smallSize and 3MB for largeSize
    const auto smallSize = 1024 * 1024;
    const auto largeSize = 3 * 1024 * 1024;
    const auto poolId =
        alloc.addPool("default", alloc.getCacheMemoryStats().ramCacheSize,
                      {smallSize + 100, largeSize + 100});
    // Allocate until the smaller objects fill up the cache
    // keeps handles in the vector to avoid eviction
    std::vector<typename AllocatorT::WriteHandle> handles;
    for (int i = 0;; i++) {
      auto handle = util::allocateAccessible(
          alloc, poolId, fmt::format("small_{}", i), smallSize);
      if (handle == nullptr) {
        break;
      }
      handles.push_back(std::move(handle));
    }

    // clear handles so it will start to evict items
    handles.clear();

    // first time alloc failure will trigger rebalancer initialization
    EXPECT_EQ(nullptr,
              util::allocateAccessible(alloc, poolId, "large", largeSize));

    std::this_thread::sleep_for(std::chrono::seconds{1});
    // trigger the slab rebalance
    EXPECT_EQ(nullptr,
              util::allocateAccessible(alloc, poolId, "large", largeSize));

    std::this_thread::sleep_for(std::chrono::seconds{1});
    // have available slab now
    EXPECT_NE(nullptr,
              util::allocateAccessible(alloc, poolId, "large", largeSize));
  }

  // changing cache name should  not affect the persistence
  void testAttachWithDifferentName() {
    typename AllocatorT::Config config;

    std::vector<uint32_t> sizes;
    uint8_t poolId;

    const size_t nSlabs = 20;
    config.setCacheSize(nSlabs * Slab::kSize);
    const unsigned int keyLen = 100;
    auto creationTime = 0;
    config.enableCachePersistence(this->cacheDir_);
    config.cacheName = "foobar";

    // Test allocations. These allocations should remain after save/restore.
    // Original lru allocator
    std::vector<std::string> keys;
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      poolId = alloc.addPool("foobar", numBytes);
      sizes = this->getValidAllocSizes(alloc, poolId, nSlabs, keyLen);
      this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
      for (const auto& item : alloc) {
        auto key = item.getKey();
        keys.push_back(key.str());
      }

      creationTime = alloc.getCacheCreationTime();

      // save
      alloc.shutDown();
    }

    /* sleep override */ std::this_thread::sleep_for(std::chrono::seconds(3));

    // Restore lru allocator, check creation time.
    {
      config.cacheName = "helloworld";
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);
      ASSERT_EQ(creationTime, alloc.getCacheCreationTime());
      for (auto& key : keys) {
        auto handle = alloc.find(typename AllocatorT::Key{key});
        ASSERT_NE(nullptr, handle.get());
      }
    }
  }

  // changing size should cause attach to fail.
  void testAttachWithDifferentSize(bool usePosix, bool smaller) {
    typename AllocatorT::Config config;
    config.enableCachePersistence(this->cacheDir_);
    if (usePosix) {
      config.usePosixForShm();
    }

    std::vector<uint32_t> sizes;
    uint8_t poolId;

    const size_t nSlabsOriginal = 20;
    const size_t nSlabsNew = smaller ? nSlabsOriginal - 5 : nSlabsOriginal + 5;
    config.setCacheSize(nSlabsOriginal * Slab::kSize);
    const unsigned int keyLen = 100;
    config.enableCachePersistence(this->cacheDir_);
    config.cacheName = "foobar";

    // Test allocations. These allocations should remain after save/restore.
    // Original lru allocator
    std::vector<std::string> keys;
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      poolId = alloc.addPool("foobar", numBytes);
      sizes = this->getValidAllocSizes(alloc, poolId, nSlabsOriginal, keyLen);
      this->fillUpPoolUntilEvictions(alloc, poolId, sizes, keyLen);
      for (const auto& item : alloc) {
        auto key = item.getKey();
        keys.push_back(key.str());
      }

      // save
      alloc.shutDown();
    }

    // Restore lru allocator with correct size
    {
      config.setCacheSize(nSlabsOriginal * Slab::kSize);
      AllocatorT alloc(AllocatorT::SharedMemAttach, config);
      for (const auto& key : keys) {
        auto it = alloc.find(key);
        ASSERT_NE(it, nullptr);
      }
      alloc.shutDown();
    }

    // Restore lru allocator with size different than previous.
    {
      config.setCacheSize(nSlabsNew * Slab::kSize);
      ASSERT_THROW(AllocatorT(AllocatorT::SharedMemAttach, config),
                   std::invalid_argument);
    }
  }

  void testNascent() {
    typename AllocatorT::Config config;
    bool isRemoveCbTriggered = false;
    config.setRemoveCallback(
        [&isRemoveCbTriggered](const typename AllocatorT::RemoveCbData& data) {
          if (data.context == RemoveContext::kNormal) {
            isRemoveCbTriggered = true;
          }
        });
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("default", numBytes);
    {
      auto handle = alloc.allocate(poolId, "test", 100);
    }
    EXPECT_EQ(false, isRemoveCbTriggered);
    {
      auto handle = alloc.allocate(poolId, "test", 100);
      objcacheUnmarkNascent(handle);
    }
    EXPECT_EQ(true, isRemoveCbTriggered);
  }

  void testDelayWorkersStart() {
    // Configure reaper and create an item with TTL
    // Verify without explicitly starting workers, the item is not reaped
    // And then verify after explicitly starting workers, the item is reaped
    typename AllocatorT::Config config;
    config.enableItemReaperInBackground(std::chrono::milliseconds{10})
        .setDelayCacheWorkersStart();

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("default", numBytes);

    {
      auto handle = alloc.allocate(poolId, "test", 100, 2);
      alloc.insertOrReplace(handle);
    }

    EXPECT_NE(nullptr, alloc.inspectCache("test").first);
    std::this_thread::sleep_for(std::chrono::seconds{3});
    // Still here because we haven't started the workers
    EXPECT_NE(nullptr, alloc.inspectCache("test").first);

    alloc.startCacheWorkers();
    std::this_thread::sleep_for(std::chrono::seconds{1});
    // Once reaper starts it will have expired this item quickly
    EXPECT_EQ(nullptr, alloc.inspectCache("test").first);
  }

  // Test to validate the logic to detect/export the slab release stuck.
  // To do so, allocate two items and intentionally hold references
  // while checking the stuck counter.
  void testSlabReleaseStuck() {
    const unsigned int releaseStuckThreshold = 10;
    typename AllocatorT::Config config{};
    config.setCacheSize(3 * Slab::kSize);
    config.setSlabReleaseStuckThreashold(
        std::chrono::seconds(releaseStuckThreshold));
    config.setSlabRebalanceTimeout(std::chrono::milliseconds(0));
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    // 3/4 * kSize to make sure items are allocated in different slabs
    std::vector<uint32_t> sizes = {Slab::kSize * 3 / 4};

    // Allocate two items to be used for tests
    auto handle1 = util::allocateAccessible(alloc, poolId, "key1", sizes[0]);
    ASSERT_NE(nullptr, handle1);

    auto handle2 = util::allocateAccessible(alloc, poolId, "key2", sizes[0]);
    ASSERT_NE(nullptr, handle2);

    const uint8_t classId = alloc.getAllocInfo(handle1->getMemory()).classId;
    ASSERT_EQ(classId, alloc.getAllocInfo(handle2->getMemory()).classId);

    // Assert that numSlabReleaseStuck is not set
    ASSERT_EQ(0, alloc.getSlabReleaseStats().numSlabReleaseStuck);

    // Trying to remove the slab where the item is allocated. Thus, the release
    // will be stuck until the reference is dropped below.
    auto r1 = std::async(std::launch::async, [&] {
      alloc.releaseSlab(poolId, classId, SlabReleaseMode::kResize,
                        handle1->getMemory());
      ASSERT_EQ(nullptr, handle1);
    });

    // Sleep for 2 + <releaseStuckThreshold> seconds; 2 seconds is an arbitrary
    // margin to allow the release is detected as being stuck after
    // <releaseStuckThreshold> seconds.
    /* sleep override */ sleep(2 + releaseStuckThreshold);

    ASSERT_EQ(1, alloc.getSlabReleaseStats().numSlabReleaseStuck);

    // Do the same for another item
    auto r2 = std::async(std::launch::async, [&] {
      alloc.releaseSlab(poolId, classId, SlabReleaseMode::kResize,
                        handle2->getMemory());
      ASSERT_EQ(nullptr, handle2);
    });

    /* sleep override */ sleep(2 + releaseStuckThreshold);

    ASSERT_EQ(2, alloc.getSlabReleaseStats().numSlabReleaseStuck);

    // Now, release handles so the releaseSlab can proceed
    handle1.reset();
    r1.wait();
    ASSERT_EQ(1, alloc.getSlabReleaseStats().numSlabReleaseStuck);

    handle2.reset();
    r2.wait();
    ASSERT_EQ(0, alloc.getSlabReleaseStats().numSlabReleaseStuck);
  }

  void testSlabReleaseTimeoutHelper(unsigned int slabRebalanceTimeoutSecs,
                                    bool shouldTimeout) {
    typename AllocatorT::Config config{};
    config.setCacheSize(3 * Slab::kSize);
    config.setSlabRebalanceTimeout(
        std::chrono::milliseconds(slabRebalanceTimeoutSecs * 1000));
    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes);

    // 3/4 * kSize to make sure items are allocated in different slabs
    std::vector<uint32_t> sizes = {Slab::kSize * 3 / 4};

    // Allocate two items to be used for tests
    auto handle1 = util::allocateAccessible(alloc, poolId, "key1", sizes[0]);
    ASSERT_NE(nullptr, handle1);

    auto handle2 = util::allocateAccessible(alloc, poolId, "key2", sizes[0]);
    ASSERT_NE(nullptr, handle2);

    const uint8_t classId = alloc.getAllocInfo(handle1->getMemory()).classId;
    ASSERT_EQ(classId, alloc.getAllocInfo(handle2->getMemory()).classId);

    // Assert that numAbortedSlabReleases is not set initially
    ASSERT_EQ(0, alloc.getSlabReleaseStats().numAbortedSlabReleases);
    ASSERT_EQ(0, alloc.getGlobalCacheStats().numAbortedSlabReleases);

    // Get initial count for verification of successful slab releases
    uint64_t initialRebalanceCount =
        alloc.getSlabReleaseStats().numSlabReleaseForRebalance;

    // Helper lambda to start a slab release operation with appropriate
    // assertions
    auto startSlabRelease = [&](const void* hint) {
      return std::async(std::launch::async, [&, hint] {
        if (shouldTimeout) {
          ASSERT_THROW(alloc.releaseSlab(poolId, classId,
                                         SlabReleaseMode::kRebalance, hint),
                       exception::SlabReleaseAborted);
        } else {
          ASSERT_NO_THROW(alloc.releaseSlab(poolId, classId,
                                            SlabReleaseMode::kRebalance, hint));
        }
      });
    };

    // Determine sleep duration based on timeout setting
    // Need to sleep longer than timeout to trigger abort, or just 1 sec if no
    // timeout
    unsigned int sleepDuration =
        shouldTimeout ? (2 + slabRebalanceTimeoutSecs) : 1;

    // Start first slab release operation (will be blocked by handle1)
    auto r1 = startSlabRelease(handle1->getMemory());
    /* sleep override */ sleep(sleepDuration);

    // Check expected abort count after first sleep
    unsigned int expectedAborts = shouldTimeout ? 1 : 0;
    ASSERT_EQ(expectedAborts,
              alloc.getSlabReleaseStats().numAbortedSlabReleases);
    ASSERT_EQ(expectedAborts,
              alloc.getGlobalCacheStats().numAbortedSlabReleases);

    // Start second slab release operation (will be blocked by handle2)
    auto r2 = startSlabRelease(handle2->getMemory());

    // Sleep again and check abort count
    /* sleep override */ sleep(sleepDuration);
    expectedAborts = shouldTimeout ? 2 : 0;
    ASSERT_EQ(expectedAborts,
              alloc.getSlabReleaseStats().numAbortedSlabReleases);
    ASSERT_EQ(expectedAborts,
              alloc.getGlobalCacheStats().numAbortedSlabReleases);

    // Release handles to allow operations to complete
    handle1.reset();
    r1.wait();

    handle2.reset();
    r2.wait();

    // Final verification: abort count should remain the same
    ASSERT_EQ(expectedAborts,
              alloc.getSlabReleaseStats().numAbortedSlabReleases);
    ASSERT_EQ(expectedAborts,
              alloc.getGlobalCacheStats().numAbortedSlabReleases);

    // When not timing out, verify slab releases actually completed successfully
    if (!shouldTimeout) {
      ASSERT_EQ(initialRebalanceCount + 2,
                alloc.getSlabReleaseStats().numSlabReleaseForRebalance);
    }
  }

  void testSlabReleaseTimeout() {
    // 5 seconds timeout, should abort
    testSlabReleaseTimeoutHelper(5, true);
  }

  void testSlabReleaseTimeoutZero() {
    // 0 = no timeout, should not abort
    testSlabReleaseTimeoutHelper(0, false);
  }

  void testRateMap() {
    RateMap counters;
    counters.updateCount("stat1", 11);
    counters.updateDelta("stat2", 22);
    counters.updateDelta("stat2", 44);
    EXPECT_EQ(22, counters.getDelta("stat2"));

    int seen = 0;
    counters.exportStats(
        std::chrono::seconds{60},
        [&seen](folly::StringPiece name, uint64_t value) mutable {
          if (name == "stat1") {
            EXPECT_EQ(11, value);
            seen++;
          } else if (name == "stat2.60") {
            EXPECT_EQ(22, value);
            seen++;
          }
        });
    EXPECT_EQ(2, seen);

    seen = 0;
    counters.exportStats(
        std::chrono::seconds{30},
        [&seen](folly::StringPiece name, uint64_t value) mutable {
          if (name == "stat1") {
            EXPECT_EQ(11, value);
            seen++;
          } else if (name == "stat2.60") {
            EXPECT_EQ(44, value);
            seen++;
          }
        });
    EXPECT_EQ(2, seen);
  }

  void testStatSnapshotTest() {
    typename AllocatorT::Config config;
    config.setCacheSize(20 * Slab::kSize);
    config.cacheName = "foobar";
    AllocatorT alloc(config);

    const std::string statPrefix = "cachelib." + config.cacheName + ".";
    const std::string numNvmGets = statPrefix + "nvm.gets";
    const std::string numNvmGetMiss = statPrefix + "nvm.gets.miss";
    const std::string numCacheGets = statPrefix + "cache.gets";
    const std::string numCacheGetMiss = statPrefix + "cache.gets.miss";

    alloc.counters_.updateDelta(numNvmGets, 2);
    alloc.counters_.updateDelta(numCacheGets, 4);
    alloc.counters_.updateDelta(numNvmGetMiss, 1);
    alloc.counters_.updateDelta(numCacheGetMiss, 2);

    const auto cacheHitRate = alloc.calculateCacheHitRate(statPrefix);
    EXPECT_EQ(cacheHitRate.ram, 50);
    EXPECT_EQ(cacheHitRate.nvm, 50);
    EXPECT_EQ(cacheHitRate.overall, 75);
    EXPECT_EQ(alloc.counters_.getDelta(numNvmGets), 2);
    EXPECT_EQ(alloc.counters_.getDelta(numCacheGets), 4);
    EXPECT_EQ(alloc.counters_.getDelta(numNvmGetMiss), 1);
    EXPECT_EQ(alloc.counters_.getDelta(numCacheGetMiss), 2);

    alloc.counters_.updateDelta(numNvmGets, 4);
    alloc.counters_.updateDelta(numCacheGets, 9);
    alloc.counters_.updateDelta(numNvmGetMiss, 2);
    alloc.counters_.updateDelta(numCacheGetMiss, 4);
    const auto cacheHitRate1 = alloc.calculateCacheHitRate(statPrefix);
    EXPECT_EQ(cacheHitRate1.ram, 60);
    EXPECT_EQ(cacheHitRate1.nvm, 50);
    EXPECT_EQ(cacheHitRate1.overall, 80);

    // Bad stats
    alloc.counters_.updateDelta(numNvmGets, 0);
    alloc.counters_.updateDelta(numCacheGets, 0);
    alloc.counters_.updateDelta(numNvmGetMiss, 0);
    alloc.counters_.updateDelta(numCacheGetMiss, 0);
    const auto cacheHitRate2 = alloc.calculateCacheHitRate(statPrefix);
    EXPECT_EQ(cacheHitRate2.ram, 0);
    EXPECT_EQ(cacheHitRate2.nvm, 0);
    EXPECT_EQ(cacheHitRate2.overall, 0);

    int intervalNameExists = 0;
    alloc.exportStats(
        statPrefix, std::chrono::seconds{60},
        [&intervalNameExists](auto name, auto value) {
          if (name == "cachelib.foobar.nvm.gets.60" && value == 0) {
            intervalNameExists++;
          }
          if (name == "cachelib.foobar.nvm.gets.miss.60" && value == 0) {
            intervalNameExists++;
          }
        });
    alloc.find("some non-existent key");
    alloc.exportStats(
        statPrefix,
        // We will convert a custom interval to 60 seconds interval. So the
        // one "FIND" will become two operations when averaged out to 60
        // seocnds.
        std::chrono::seconds{30}, [&intervalNameExists](auto name, auto value) {
          if (name == "cachelib.foobar.cache.gets.60" && value == 2) {
            intervalNameExists++;
          }
          if (name == "cachelib.foobar.cache.gets.miss.60" && value == 2) {
            intervalNameExists++;
          }
        });
    EXPECT_EQ(intervalNameExists, 4);
  }

  void testProvisionPool() {
    typename AllocatorT::Config config;
    config.setCacheSize(20 * Slab::kSize);
    config.cacheName = "foobar";

    // Provision all slabs into the first AC
    {
      AllocatorT alloc(config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      auto poolId = alloc.addPool("foobar", numBytes, {64, 1024});
      ASSERT_TRUE(alloc.provisionPool(poolId, {19, 0}));

      auto smallIt = util::allocateAccessible(alloc, poolId, "small", 10);
      EXPECT_NE(nullptr, smallIt);

      auto largeIt = util::allocateAccessible(alloc, poolId, "large", 500);
      EXPECT_EQ(nullptr, largeIt);
    }

    // Provision one slab into the second AC and rest to the first AC
    {
      AllocatorT alloc(config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      auto poolId = alloc.addPool("foobar", numBytes, {64, 1024});
      ASSERT_TRUE(alloc.provisionPool(poolId, {18, 1}));

      auto smallIt = util::allocateAccessible(alloc, poolId, "small", 10);
      EXPECT_NE(nullptr, smallIt);

      auto largeIt = util::allocateAccessible(alloc, poolId, "large", 500);
      EXPECT_NE(nullptr, largeIt);
    }

    // Invalid config. More than the number of ACs
    {
      AllocatorT alloc(config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      auto poolId = alloc.addPool("foobar", numBytes, {64, 1024});
      EXPECT_THROW(alloc.provisionPool(poolId, {17, 1, 1}),
                   std::invalid_argument);
    }

    // Invalid config. Trying to provision more slabs than available.
    {
      AllocatorT alloc(config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      auto poolId = alloc.addPool("foobar", numBytes, {64, 1024});
      EXPECT_FALSE(alloc.provisionPool(poolId, {19, 1}));

      // Correct config will succeed
      ASSERT_TRUE(alloc.provisionPool(poolId, {18, 1}));

      auto smallIt = util::allocateAccessible(alloc, poolId, "small", 10);
      EXPECT_NE(nullptr, smallIt);

      auto largeIt = util::allocateAccessible(alloc, poolId, "large", 500);
      EXPECT_NE(nullptr, largeIt);
    }
  }

  void testProvisionPoolWithPowerLaw() {
    typename AllocatorT::Config config;
    config.setCacheSize(100 * Slab::kSize);
    config.cacheName = "foobar";

    {
      AllocatorT alloc(config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      auto poolId = alloc.addPool("foobar", numBytes,
                                  {64, 128, 256, 512, 1024, 2048, 4096, 8192});
      EXPECT_TRUE(alloc.provisionPoolWithPowerLaw(poolId, 0.5 /* power */,
                                                  1 /* minSlabsPerAC */));

      auto poolStats = alloc.getPoolStats(poolId);
      EXPECT_EQ(21, poolStats.numSlabsForClass(0));
      EXPECT_EQ(15, poolStats.numSlabsForClass(1));
      EXPECT_EQ(13, poolStats.numSlabsForClass(2));
      EXPECT_EQ(11, poolStats.numSlabsForClass(3));
      EXPECT_EQ(10, poolStats.numSlabsForClass(4));
      EXPECT_EQ(9, poolStats.numSlabsForClass(5));
      EXPECT_EQ(8, poolStats.numSlabsForClass(6));
      EXPECT_EQ(8, poolStats.numSlabsForClass(7));
    }

    {
      AllocatorT alloc(config);
      const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
      auto poolId = alloc.addPool("foobar", numBytes,
                                  {64, 128, 256, 512, 1024, 2048, 4096, 8192});
      EXPECT_TRUE(alloc.provisionPoolWithPowerLaw(poolId, 2.0 /* power */,
                                                  2 /* minSlabsPerAC */));

      auto poolStats = alloc.getPoolStats(poolId);
      EXPECT_EQ(56, poolStats.numSlabsForClass(0));
      EXPECT_EQ(15, poolStats.numSlabsForClass(1));
      EXPECT_EQ(8, poolStats.numSlabsForClass(2));
      EXPECT_EQ(5, poolStats.numSlabsForClass(3));
      EXPECT_EQ(4, poolStats.numSlabsForClass(4));
      EXPECT_EQ(3, poolStats.numSlabsForClass(5));
      EXPECT_EQ(3, poolStats.numSlabsForClass(6));
      EXPECT_EQ(2, poolStats.numSlabsForClass(7));
    }
  }

  void testManualRebalanceStrategy() {
    typename AllocatorT::Config config;
    config.setCacheSize(11 * Slab::kSize);
    config.cacheName = "foobar";
    auto manualStrategy = std::make_shared<ManualStrategy>();
    config.enablePoolRebalancing(manualStrategy, std::chrono::milliseconds{10});

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId = alloc.addPool("foobar", numBytes, {64, 256, 1024});

    ASSERT_TRUE(alloc.provisionPool(poolId, {2, 5, 3}));
    {
      auto poolStats = alloc.getPoolStats(poolId);
      EXPECT_EQ(2, poolStats.numSlabsForClass(0));
      EXPECT_EQ(5, poolStats.numSlabsForClass(1));
      EXPECT_EQ(3, poolStats.numSlabsForClass(2));
    }

    manualStrategy->updateConfig(ManualStrategy::Config{
        .slabsDistribution = std::vector<uint32_t>{5, 1, 4}});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    {
      auto poolStats = alloc.getPoolStats(poolId);
      EXPECT_EQ(5, poolStats.numSlabsForClass(0));
      EXPECT_EQ(1, poolStats.numSlabsForClass(1));
      EXPECT_EQ(4, poolStats.numSlabsForClass(2));
    }
  }

 private:
  static constexpr const char* kTestCachePrefix = "cachelib.test_cache.";
  static constexpr const char* kPool1Name = "pool1";
  static constexpr const char* kPool2Name = "pool2";
  static constexpr const char* kAggregatedName = "aggregated";

  AllocatorT createPoolStatsTestAllocator(bool enableAggregation = false) {
    typename AllocatorT::Config config;
    config.setCacheSize(20 * Slab::kSize).cacheName = "test_cache";
    if (enableAggregation) {
      config.enableAggregatePoolStats();
    }
    return AllocatorT(config);
  }

  void setupTestPools(AllocatorT& alloc,
                      const std::set<uint32_t>& pool1Sizes,
                      const std::set<uint32_t>& pool2Sizes) {
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId1 = alloc.addPool(kPool1Name, numBytes / 2, pool1Sizes);
    auto poolId2 = alloc.addPool(kPool2Name, numBytes / 2, pool2Sizes);

    auto item1 = util::allocateAccessible(alloc, poolId1, "key1", 60);
    auto item2 = util::allocateAccessible(alloc, poolId2, "key2", 120);
    ASSERT_NE(nullptr, item1);
    ASSERT_NE(nullptr, item2);
  }

  bool hasAggregatedStats(AllocatorT& alloc) {
    bool foundAggregated = false;
    bool foundIndividual = false;

    std::string aggregatedNamePrefix =
        folly::to<std::string>("pool.", kAggregatedName, ".");
    std::string pool1NamePrefix =
        folly::to<std::string>("pool.", kPool1Name, ".");
    std::string pool2NamePrefix =
        folly::to<std::string>("pool.", kPool2Name, ".");

    alloc.exportStats(
        kTestCachePrefix, std::chrono::seconds{60},
        [&](folly::StringPiece name, uint64_t) {
          const std::string nameStr = name.str();
          if (nameStr.find(aggregatedNamePrefix) != std::string::npos) {
            foundAggregated = true;
          }
          if (nameStr.find(pool1NamePrefix) != std::string::npos ||
              nameStr.find(pool2NamePrefix) != std::string::npos) {
            foundIndividual = true;
          }
        });
    XDCHECK(foundAggregated ^ foundIndividual,
            "Expected to find aggregated or individual stats only, not both or "
            "neither");
    return foundAggregated;
  }

 public:
  void testAggregatePoolStatsDiffAC() {
    auto alloc = createPoolStatsTestAllocator(true);
    // Test aggregation of stats from two pools with different allocation
    // classes
    const std::set<uint32_t> pool1Sizes = {64, 128, 256};
    const std::set<uint32_t> pool2Sizes = {256, 512};
    setupTestPools(alloc, pool1Sizes, pool2Sizes);
    EXPECT_TRUE(hasAggregatedStats(alloc));

    auto poolId1 = alloc.getPoolId(kPool1Name);
    auto poolId2 = alloc.getPoolId(kPool2Name);

    // Add items for each allocation class in pool1
    auto item1_64 = util::allocateAccessible(alloc, poolId1, "key1_64", 20);
    auto item1_128 = util::allocateAccessible(alloc, poolId1, "key1_128", 80);
    auto item1_256 = util::allocateAccessible(alloc, poolId1, "key1_256", 200);
    ASSERT_NE(nullptr, item1_64);
    ASSERT_NE(nullptr, item1_128);
    ASSERT_NE(nullptr, item1_256);

    // Add items for each allocation class in pool2
    auto item2_256 = util::allocateAccessible(alloc, poolId2, "key2_256", 200);
    auto item2_512 = util::allocateAccessible(alloc, poolId2, "key2_512", 450);
    ASSERT_NE(nullptr, item2_256);
    ASSERT_NE(nullptr, item2_512);

    auto pool1Stats = alloc.getPoolStats(poolId1);
    auto pool2Stats = alloc.getPoolStats(poolId2);

    // Capture aggregated pool statistics from exportStats() callback
    uint64_t aggregatedSize = 0;
    uint64_t aggregatedItems = 0;
    uint64_t aggregatedFreeMemoryBytes = 0;

    std::string aggregatedNamePrefix =
        folly::to<std::string>("pool.", kAggregatedName);
    std::string sizeIdentifier =
        folly::to<std::string>(aggregatedNamePrefix, ".size");
    std::string itemsIdentifier =
        folly::to<std::string>(aggregatedNamePrefix, ".items");
    std::string freeMemoryBytesIdentifier =
        folly::to<std::string>(aggregatedNamePrefix, ".free_memory_bytes");

    alloc.exportStats(kTestCachePrefix, std::chrono::seconds{60},
                      [&](folly::StringPiece name, uint64_t value) {
                        if (name.endsWith(sizeIdentifier)) {
                          aggregatedSize = value;
                        } else if (name.endsWith(itemsIdentifier)) {
                          aggregatedItems = value;
                        } else if (name.endsWith(freeMemoryBytesIdentifier)) {
                          aggregatedFreeMemoryBytes = value;
                        }
                      });

    // Verify aggregated stats match the sum of individual pool stats
    EXPECT_EQ(aggregatedSize, pool1Stats.poolSize + pool2Stats.poolSize);
    EXPECT_EQ(aggregatedItems, pool1Stats.numItems() + pool2Stats.numItems());
    EXPECT_EQ(aggregatedFreeMemoryBytes,
              pool1Stats.freeMemoryBytes() + pool2Stats.freeMemoryBytes());

    // Verify we have correct number of items in both pools
    // 3 new items + 1 from setupTestPools
    EXPECT_EQ(pool1Stats.numItems(), 4);
    // 2 new items + 1 from setupTestPools
    EXPECT_EQ(pool2Stats.numItems(), 3);
  }

  void testNonAggregatePoolStats() {
    auto alloc = createPoolStatsTestAllocator(false);
    setupTestPools(alloc, {64, 128, 256}, {64, 128, 256});
    EXPECT_FALSE(hasAggregatedStats(alloc));
  }

  void testAggregatePoolStatsValues() {
    auto alloc = createPoolStatsTestAllocator(true);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;

    auto poolId1 = alloc.addPool(kPool1Name, numBytes / 2, {64, 128});
    auto poolId2 = alloc.addPool(kPool2Name, numBytes / 2, {64, 128});

    auto pool1Stats = alloc.getPoolStats(poolId1);
    auto pool2Stats = alloc.getPoolStats(poolId2);

    // Capture specific aggregated pool statistics from exportStats() callback.
    // Hand-picked a few representative stats to avoid checking 10+ different
    // stats. Verify these exported aggregated values match manually summed
    // individual pool stats.
    uint64_t aggregatedSize = 0;
    uint64_t aggregatedUsableSize = 0;
    uint64_t aggregatedItems = 0;
    uint64_t aggregatedAllocAttempts = 0;

    std::string aggregatedNamePrefix =
        folly::to<std::string>("pool.", kAggregatedName);
    std::string sizeIdentifier =
        folly::to<std::string>(aggregatedNamePrefix, ".size");
    std::string usableSizeIdentifier =
        folly::to<std::string>(aggregatedNamePrefix, ".usable_size");
    std::string itemsIdentifier =
        folly::to<std::string>(aggregatedNamePrefix, ".items");
    std::string allocAttemptsIdentifier =
        folly::to<std::string>(aggregatedNamePrefix, ".alloc.attempts");

    alloc.exportStats(kTestCachePrefix, std::chrono::seconds{60},
                      [&](folly::StringPiece name, uint64_t value) {
                        if (name.endsWith(sizeIdentifier)) {
                          aggregatedSize = value;
                        } else if (name.endsWith(usableSizeIdentifier)) {
                          aggregatedUsableSize = value;
                        } else if (name.endsWith(itemsIdentifier)) {
                          aggregatedItems = value;
                        } else if (name.endsWith(allocAttemptsIdentifier)) {
                          aggregatedAllocAttempts = value;
                        }
                      });

    EXPECT_EQ(aggregatedSize, pool1Stats.poolSize + pool2Stats.poolSize);
    EXPECT_EQ(aggregatedUsableSize,
              pool1Stats.poolUsableSize + pool2Stats.poolUsableSize);
    EXPECT_EQ(aggregatedItems, pool1Stats.numItems() + pool2Stats.numItems());
    EXPECT_EQ(aggregatedAllocAttempts,
              pool1Stats.numAllocAttempts() + pool2Stats.numAllocAttempts());
  }

  void testEvictionAgeAggregation() {
    // Create a small allocator to fill up quickly
    typename AllocatorT::Config config;
    config.setCacheSize(6 * Slab::kSize);
    AllocatorT alloc(config);

    const std::set<uint32_t> allocSizes{150};

    // Create two pools of different sizes to get different eviction patterns
    auto poolId1 = alloc.addPool("pool1", Slab::kSize, allocSizes);
    auto poolId2 = alloc.addPool("pool2", 2 * Slab::kSize, allocSizes);

    ASSERT_NE(Slab::kInvalidPoolId, poolId1);
    ASSERT_NE(Slab::kInvalidPoolId, poolId2);

    std::vector<uint32_t> sizes = {80};
    const unsigned int keyLen = 30;

    // Fill pools until evictions happen
    this->fillUpPoolUntilEvictions(alloc, poolId1, sizes, keyLen);
    this->fillUpPoolUntilEvictions(alloc, poolId2, sizes, keyLen);

    // Cause different amounts of evictions to generate different age statistics
    // Pool1 (smaller) - cause more evictions relative to its size
    this->ensureAllocsOnlyFromEvictions(alloc, poolId1, sizes, keyLen,
                                        Slab::kSize / 2, false);
    // Pool2 (larger) - cause fewer evictions relative to its size
    this->ensureAllocsOnlyFromEvictions(alloc, poolId2, sizes, keyLen,
                                        Slab::kSize / 4, false);

    // Get individual pool stats
    auto stats1 = alloc.getPoolStats(poolId1);
    auto stats2 = alloc.getPoolStats(poolId2);

    // Verify both pools have evictions
    ASSERT_GT(stats1.numEvictions(), 0) << "Pool 1 should have evictions";
    ASSERT_GT(stats2.numEvictions(), 0) << "Pool 2 should have evictions";

    auto aggregated = stats1;
    aggregated += stats2;
    const uint64_t totalEvictions =
        stats1.numEvictions() + stats2.numEvictions();

    // Verify eviction counts are summed
    ASSERT_EQ(aggregated.numEvictions(), totalEvictions);

    const double expectedWeightedAvg =
        (static_cast<double>(stats1.numEvictions()) *
             stats1.evictionAgeSecs.avg +
         static_cast<double>(stats2.numEvictions()) *
             stats2.evictionAgeSecs.avg) /
        totalEvictions;

    ASSERT_EQ(aggregated.evictionAgeSecs.avg,
              static_cast<uint64_t>(std::round(expectedWeightedAvg)))
        << "Aggregated eviction age should be weighted average";

    // Test a few percentiles to ensure the weighted averaging works across
    // all fields
    const double expectedWeightedP50 =
        (static_cast<double>(stats1.numEvictions()) *
             stats1.evictionAgeSecs.p50 +
         static_cast<double>(stats2.numEvictions()) *
             stats2.evictionAgeSecs.p50) /
        totalEvictions;

    ASSERT_EQ(aggregated.evictionAgeSecs.p50,
              static_cast<uint64_t>(std::round(expectedWeightedP50)))
        << "Aggregated p50 should be weighted average";

    const double expectedWeightedP99 =
        (static_cast<double>(stats1.numEvictions()) *
             stats1.evictionAgeSecs.p99 +
         static_cast<double>(stats2.numEvictions()) *
             stats2.evictionAgeSecs.p99) /
        totalEvictions;

    ASSERT_EQ(aggregated.evictionAgeSecs.p99,
              static_cast<uint64_t>(std::round(expectedWeightedP99)))
        << "Aggregated p99 should be weighted average";
  }

  void testPoolAggregationWithMaxClasses() {
    auto alloc = createPoolStatsTestAllocator(true);

    // Create two pools whose sum of allocation classes exceeds
    // MemoryAllocator::kMaxClasses
    std::set<uint32_t> pool1Sizes;
    std::set<uint32_t> pool2Sizes;

    for (unsigned int i = 1; i <= MemoryAllocator::kMaxClasses + 1; ++i) {
      if (i < MemoryAllocator::kMaxClasses / 2) {
        pool1Sizes.insert(64 * i);
      } else {
        pool2Sizes.insert(64 * i);
      }
    }

    // This creates exactly MemoryAllocator::kMaxClasses + 1 (=129) distinct
    // allocation sizes which does not allow aggregated stats
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    auto poolId1 = alloc.addPool(kPool1Name, numBytes / 2, pool1Sizes);
    auto poolId2 = alloc.addPool(kPool2Name, numBytes / 2, pool2Sizes);

    // Allocate one item in each pool
    auto item1 = util::allocateAccessible(alloc, poolId1, "key1", 60);
    auto item2 = util::allocateAccessible(alloc, poolId2, "key2", 4200);
    ASSERT_NE(nullptr, item1);
    ASSERT_NE(nullptr, item2);

    // Verify that aggregated stats are not present
    ASSERT_FALSE(hasAggregatedStats(alloc));
  }

  void testAggregatePoolStatsSinglePool() {
    // Test fallback to individual pool stats when only one pool exists
    // and aggregatePoolStats is enabled
    auto alloc = createPoolStatsTestAllocator(true);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;

    // Add a single pool
    auto poolId = alloc.addPool(kPool1Name, numBytes, {64, 128, 256});

    // Add some items to the pool
    auto item1 = util::allocateAccessible(alloc, poolId, "key1", 60);
    auto item2 = util::allocateAccessible(alloc, poolId, "key2", 120);
    ASSERT_NE(nullptr, item1);
    ASSERT_NE(nullptr, item2);

    // With only one pool, exportStats should fall back to individual
    // pool stats and NOT export aggregated stats (nothing to aggregate)
    EXPECT_FALSE(hasAggregatedStats(alloc));
  }

  // Reproducing issue where using 128 allocation classes with tail hits
  // tracking enabled would throw when adding a pool
  void testMaxAllocSizesWithTailHitsTracking() {
    typename AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.enableTailHitsTracking();

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;

    // Create 128 allocation sizes: 128, 256, 384, ..., 16384
    std::set<uint32_t> allocSizes;
    for (uint32_t i = 1; i <= 128; ++i) {
      allocSizes.insert(i * 128);
    }

    // Assert that adding a pool with 128 allocation sizes does not throw
    ASSERT_NO_THROW(alloc.addPool("test_pool", numBytes, allocSizes));
  }
};
} // namespace tests
} // namespace cachelib
} // namespace facebook
