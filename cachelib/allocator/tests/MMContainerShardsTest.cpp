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

#include <fmt/format.h>
#include <folly/Random.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/allocator/tests/CacheAllocatorTestWrapper.h"
#include "cachelib/allocator/tests/TestBase.h"

namespace facebook::cachelib::tests {
namespace {
constexpr uint32_t kNumShards = 4;
// a single allocation class, so every item shares one sharded MM container
constexpr uint32_t kAllocSize = 1024;
constexpr uint32_t kValueSize = 900;
constexpr ClassId kCid = 0;

template <typename AllocatorT>
typename AllocatorT::Config makeConfig(uint32_t shards) {
  typename AllocatorT::Config config;
  config.setCacheSize(10 * Slab::kSize);
  config.setMMContainerShards(shards);
  return config;
}

template <typename AllocatorT>
PoolId addPool(AllocatorT& alloc) {
  return alloc.addPool("default", alloc.getCacheMemoryStats().ramCacheSize,
                       {kAllocSize});
}

template <typename AllocatorT>
bool insertItem(AllocatorT& alloc, PoolId pid, const std::string& key) {
  auto handle = alloc.allocate(pid, key, kValueSize);
  return handle && alloc.insert(handle);
}

template <typename AllocatorT>
std::vector<size_t> shardSizes(const AllocatorT& alloc, PoolId pid) {
  return CacheAllocatorTestWrapper::getMMContainerShardSizes(alloc, pid, kCid);
}

size_t sum(const std::vector<size_t>& sizes) {
  return std::accumulate(sizes.begin(), sizes.end(), size_t{0});
}

template <typename AllocatorT>
size_t countItems(AllocatorT& alloc) {
  size_t numItems = 0;
  for (const auto& item : alloc) {
    (void)item;
    ++numItems;
  }
  return numItems;
}

// Stats of the queue that items age into when they are never accessed.
template <typename AllocatorT>
const EvictionStatPerType& unaccessedQueueStat(
    const PoolEvictionAgeStats& stats) {
  if constexpr (std::is_same_v<typename AllocatorT::MMType, MM2Q>) {
    return stats.getColdEvictionStat(kCid);
  } else {
    return stats.getWarmEvictionStat(kCid);
  }
}
} // namespace

template <typename AllocatorT>
class MMContainerShardsTest : public AllocatorTest<AllocatorT> {};

using ShardedAllocatorTypes = ::testing::Types<LruAllocator, Lru2QAllocator>;
TYPED_TEST_SUITE(MMContainerShardsTest, ShardedAllocatorTypes);

TYPED_TEST(MMContainerShardsTest, ConfigValidation) {
  using AllocatorT = TypeParam;
  for (uint32_t shards : {1u, 2u, 64u}) {
    EXPECT_NO_THROW(makeConfig<AllocatorT>(shards).validate()) << shards;
  }
  for (uint32_t shards : {0u, 3u, 128u}) {
    EXPECT_THROW(makeConfig<AllocatorT>(shards).validate(),
                 std::invalid_argument)
        << shards;
  }

  EXPECT_EQ("4", makeConfig<AllocatorT>(4).serialize()["mmContainerShards"]);
}

TEST(MMContainerShardsConfigTest, RejectsUnsupportedMMType) {
  TinyLFUAllocator::Config config;
  config.setMMContainerShards(2);
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.setMMContainerShards(1);
  EXPECT_NO_THROW(config.validate());
}

TYPED_TEST(MMContainerShardsTest, InsertFindReplaceRemove) {
  using AllocatorT = TypeParam;
  AllocatorT alloc(makeConfig<AllocatorT>(kNumShards));
  const auto pid = addPool(alloc);

  constexpr size_t kNumItems = 1000;
  for (size_t i = 0; i < kNumItems; ++i) {
    ASSERT_TRUE(insertItem(alloc, pid, fmt::format("key_{}", i)));
  }
  for (size_t i = 0; i < kNumItems; ++i) {
    ASSERT_NE(nullptr, alloc.find(fmt::format("key_{}", i)));
  }
  auto sizes = shardSizes(alloc, pid);
  ASSERT_EQ(kNumShards, sizes.size());
  EXPECT_EQ(kNumItems, sum(sizes));
  for (const auto size : sizes) {
    EXPECT_GT(size, kNumItems / kNumShards / 2);
    EXPECT_LT(size, kNumItems / kNumShards * 3 / 2);
  }

  // the replacement usually lands in a different shard than the original
  for (size_t i = 0; i < kNumItems / 2; ++i) {
    auto handle = alloc.allocate(pid, fmt::format("key_{}", i), kValueSize);
    ASSERT_NE(nullptr, handle);
    ASSERT_NE(nullptr, alloc.insertOrReplace(handle));
  }
  EXPECT_EQ(kNumItems, sum(shardSizes(alloc, pid)));

  constexpr size_t kNumRemoved = 100;
  for (size_t i = 0; i < kNumRemoved; ++i) {
    ASSERT_EQ(AllocatorT::RemoveRes::kSuccess,
              alloc.remove(fmt::format("key_{}", i)));
  }
  for (size_t i = 0; i < kNumItems; ++i) {
    EXPECT_EQ(i >= kNumRemoved,
              alloc.find(fmt::format("key_{}", i)) != nullptr);
  }
  EXPECT_EQ(kNumItems - kNumRemoved, sum(shardSizes(alloc, pid)));
  EXPECT_EQ(kNumItems - kNumRemoved,
            alloc.getPoolStats(pid).cacheStats.at(kCid).containerStat.size);
}

TYPED_TEST(MMContainerShardsTest, EvictsOldestAcrossShards) {
  using AllocatorT = TypeParam;
  std::vector<std::string> evicted;
  auto config = makeConfig<AllocatorT>(kNumShards);
  config.setRemoveCallback(
      [&evicted](const typename AllocatorT::RemoveCbData& data) {
        if (data.context == RemoveContext::kEviction) {
          evicted.push_back(data.item.getKey().str());
        }
      });
  AllocatorT alloc(config);
  const auto pid = addPool(alloc);

  size_t numOld = 0;
  while (evicted.empty()) {
    ASSERT_TRUE(insertItem(alloc, pid, fmt::format("old_{}", numOld++)));
  }
  // MM update times have a resolution of one second
  /* sleep override */ std::this_thread::sleep_for(std::chrono::seconds(2));

  // Each new item evicts one. While every shard still holds old items, the
  // tail hints should steer all evictions to old items.
  const size_t numNew = numOld / 2;
  for (size_t i = 0; i < numNew; ++i) {
    ASSERT_TRUE(insertItem(alloc, pid, fmt::format("new_{}", i)));
  }
  EXPECT_GE(evicted.size(), numNew);
  EXPECT_EQ(0, std::count_if(evicted.begin(), evicted.end(), [](auto& key) {
              return key.starts_with("new_");
            }));
}

TYPED_TEST(MMContainerShardsTest, StatsAggregateAcrossShards) {
  using AllocatorT = TypeParam;
  AllocatorT alloc(makeConfig<AllocatorT>(kNumShards));
  const auto pid = addPool(alloc);

  constexpr size_t kNumItems = 1000;
  for (size_t i = 0; i < kNumItems; ++i) {
    ASSERT_TRUE(insertItem(alloc, pid, fmt::format("old_{}", i)));
  }
  /* sleep override */ std::this_thread::sleep_for(std::chrono::seconds(2));
  for (size_t i = 0; i < kNumItems; ++i) {
    ASSERT_TRUE(insertItem(alloc, pid, fmt::format("new_{}", i)));
  }

  const auto containerStat =
      alloc.getPoolStats(pid).cacheStats.at(kCid).containerStat;
  EXPECT_EQ(2 * kNumItems, containerStat.size);
  EXPECT_EQ(2 * kNumItems, sum(shardSizes(alloc, pid)));
  EXPECT_LE(containerStat.oldestTimeSec, util::getCurrentTimeSec() - 2);

  const auto ageStat =
      unaccessedQueueStat<AllocatorT>(alloc.getPoolEvictionAgeStats(pid, 0));
  EXPECT_GE(ageStat.oldestElementAge, 2);
  EXPECT_LE(ageStat.projectedAge, ageStat.oldestElementAge);
}

TYPED_TEST(MMContainerShardsTest, SlabReleaseWithEviction) {
  using AllocatorT = TypeParam;
  AllocatorT alloc(makeConfig<AllocatorT>(kNumShards));
  const auto pid = addPool(alloc);

  const size_t numItems = 2 * Slab::kSize / kAllocSize;
  for (size_t i = 0; i < numItems; ++i) {
    ASSERT_TRUE(insertItem(alloc, pid, fmt::format("key_{}", i)));
  }
  const auto* hint = alloc.find("key_0").get();
  alloc.releaseSlab(pid, kCid, SlabReleaseMode::kRebalance, hint);

  const auto remaining = countItems(alloc);
  EXPECT_LT(remaining, numItems);
  EXPECT_EQ(remaining, sum(shardSizes(alloc, pid)));
  EXPECT_EQ(nullptr, alloc.find("key_0"));
}

TYPED_TEST(MMContainerShardsTest, SlabReleaseWithMoving) {
  using AllocatorT = TypeParam;
  using Item = typename AllocatorT::Item;
  auto config = makeConfig<AllocatorT>(kNumShards);
  config.enableMovingOnSlabRelease(
      [](Item& oldItem, Item& newItem, Item* /* parent */) {
        std::memcpy(newItem.getMemory(), oldItem.getMemory(),
                    oldItem.getSize());
      },
      {});
  AllocatorT alloc(config);
  const auto pid = addPool(alloc);

  // leave free slabs so every item in the released slab can be moved
  const size_t numItems = 2 * Slab::kSize / kAllocSize;
  for (size_t i = 0; i < numItems; ++i) {
    ASSERT_TRUE(insertItem(alloc, pid, fmt::format("key_{}", i)));
  }
  const auto* hint = alloc.find("key_0").get();
  alloc.releaseSlab(pid, kCid, SlabReleaseMode::kRebalance, hint);

  EXPECT_EQ(numItems, countItems(alloc));
  EXPECT_EQ(numItems, sum(shardSizes(alloc, pid)));
  for (size_t i = 0; i < numItems; ++i) {
    ASSERT_NE(nullptr, alloc.find(fmt::format("key_{}", i)));
  }
}

TYPED_TEST(MMContainerShardsTest, ConcurrentInsertFindRemove) {
  using AllocatorT = TypeParam;
  AllocatorT alloc(makeConfig<AllocatorT>(kNumShards));
  const auto pid = addPool(alloc);

  // many more keys than fit, so inserts also evict
  constexpr uint32_t kKeysPerThread = 40000;
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&alloc, pid, t]() {
      for (int i = 0; i < 20000; ++i) {
        const auto key =
            fmt::format("key_{}_{}", t, folly::Random::rand32(kKeysPerThread));
        if (i % 4 < 2) {
          if (auto handle = alloc.allocate(pid, key, kValueSize)) {
            alloc.insertOrReplace(handle);
          }
        } else if (i % 4 == 2) {
          alloc.find(key);
        } else {
          alloc.remove(key);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_GT(alloc.getPoolStats(pid).numEvictions(), 0);
  EXPECT_EQ(countItems(alloc), sum(shardSizes(alloc, pid)));
}

TYPED_TEST(MMContainerShardsTest, WarmRollRestoresShards) {
  using AllocatorT = TypeParam;
  auto config = makeConfig<AllocatorT>(kNumShards);
  config.enableCachePersistence(this->cacheDir_);

  constexpr size_t kNumItems = 1000;
  std::vector<size_t> savedSizes;
  {
    AllocatorT alloc(AllocatorT::SharedMemNew, config);
    const auto pid = addPool(alloc);
    for (size_t i = 0; i < kNumItems; ++i) {
      ASSERT_TRUE(insertItem(alloc, pid, fmt::format("key_{}", i)));
    }
    savedSizes = shardSizes(alloc, pid);
    ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
  }

  AllocatorT alloc(AllocatorT::SharedMemAttach, config);
  const auto pid = alloc.getPoolId("default");
  EXPECT_EQ(savedSizes, shardSizes(alloc, pid));
  for (size_t i = 0; i < kNumItems; ++i) {
    ASSERT_EQ(AllocatorT::RemoveRes::kSuccess,
              alloc.remove(fmt::format("key_{}", i)));
  }
  EXPECT_EQ(0, sum(shardSizes(alloc, pid)));
}

TYPED_TEST(MMContainerShardsTest, WarmRollRefusesChangedShardCount) {
  using AllocatorT = TypeParam;
  const std::vector<std::pair<uint32_t, uint32_t>> transitions{
      {4, 2}, {4, 1}, {1, 4}};
  for (const auto& [saved, configured] : transitions) {
    const auto dir =
        fmt::format("{}/{}_to_{}", this->cacheDir_, saved, configured);
    util::makeDir(dir);
    auto config = makeConfig<AllocatorT>(saved);
    config.enableCachePersistence(dir);
    {
      AllocatorT alloc(AllocatorT::SharedMemNew, config);
      addPool(alloc);
      ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, alloc.shutDown());
    }

    config.setMMContainerShards(configured);
    EXPECT_THROW(AllocatorT(AllocatorT::SharedMemAttach, config),
                 std::invalid_argument)
        << saved << " -> " << configured;
    AllocatorT::cleanupStrayShmSegments(dir, config.usePosixShm);
  }
}

class MM2QContainerShardsTest : public AllocatorTest<Lru2QAllocator> {
 protected:
  static size_t countColdTailItems(Lru2QAllocator& alloc) {
    size_t numItems = 0;
    for (const auto& item : alloc) {
      if (alloc.getItemLruType(item) == MM2Q::LruType::ColdTail) {
        ++numItems;
      }
    }
    return numItems;
  }
};

TEST_F(MM2QContainerShardsTest, TailHitsTrackOneSlabAcrossShards) {
  auto config = makeConfig<Lru2QAllocator>(kNumShards);
  config.enableTailHitsTracking();
  config.enableCachePersistence(cacheDir_);
  constexpr size_t kAllocsPerSlab = Slab::kSize / kAllocSize;
  // never accessed, so they move from Hot into Cold and fill each cold tail
  constexpr size_t kNumItems = 4 * kAllocsPerSlab;

  {
    Lru2QAllocator alloc(Lru2QAllocator::SharedMemNew, config);
    const auto pid = addPool(alloc);
    for (size_t i = 0; i < kNumItems; ++i) {
      ASSERT_TRUE(insertItem(alloc, pid, fmt::format("old_{}", i)));
    }
    EXPECT_EQ(kAllocsPerSlab, countColdTailItems(alloc));
    ASSERT_EQ(Lru2QAllocator::ShutDownStatus::kSuccess, alloc.shutDown());
  }

  // restored shards keep their share of the tail as they keep filling
  Lru2QAllocator alloc(Lru2QAllocator::SharedMemAttach, config);
  const auto pid = alloc.getPoolId("default");
  for (size_t i = 0; i < kNumItems; ++i) {
    ASSERT_TRUE(insertItem(alloc, pid, fmt::format("new_{}", i)));
  }
  EXPECT_EQ(kAllocsPerSlab, countColdTailItems(alloc));
}

TEST_F(MM2QContainerShardsTest, TailSizesAddUpToOneSlab) {
  auto config = makeConfig<Lru2QAllocator>(kNumShards);
  config.enableTailHitsTracking();
  config.enableCachePersistence(cacheDir_);
  // allocs per slab: a multiple of kNumShards (4096), not a multiple (6), and
  // fewer than kNumShards (2)
  const std::set<uint32_t> allocSizes{kAllocSize, 600 * 1024, 2 * 1024 * 1024};
  auto expectOneSlabOfTail = [&allocSizes](const Lru2QAllocator& alloc,
                                           PoolId pid) {
    for (const auto allocSize : allocSizes) {
      const auto cid = alloc.getPool(pid).getAllocationClassId(allocSize);
      EXPECT_EQ(Slab::kSize / allocSize,
                sum(CacheAllocatorTestWrapper::getMMContainerShardTailSizes(
                    alloc, pid, cid)))
          << allocSize;
    }
  };

  {
    Lru2QAllocator alloc(Lru2QAllocator::SharedMemNew, config);
    const auto pid = alloc.addPool(
        "default", alloc.getCacheMemoryStats().ramCacheSize, allocSizes);
    expectOneSlabOfTail(alloc, pid);
    alloc.overridePoolConfig(pid, Lru2QAllocator::MMConfig{});
    expectOneSlabOfTail(alloc, pid);
    ASSERT_EQ(Lru2QAllocator::ShutDownStatus::kSuccess, alloc.shutDown());
  }

  Lru2QAllocator alloc(Lru2QAllocator::SharedMemAttach, config);
  expectOneSlabOfTail(alloc, alloc.getPoolId("default"));
}
} // namespace facebook::cachelib::tests
