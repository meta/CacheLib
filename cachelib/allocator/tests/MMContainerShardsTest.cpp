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
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/allocator/tests/CacheAllocatorTestWrapper.h"
#include "cachelib/allocator/tests/TestBase.h"

namespace facebook::cachelib::tests {
namespace {
using AllocatorT = LruAllocator;
using Item = AllocatorT::Item;

constexpr uint32_t kNumShards = 4;
// a single allocation class, so every item shares one sharded MM container
constexpr uint32_t kAllocSize = 1024;
constexpr uint32_t kValueSize = 900;
constexpr ClassId kCid = 0;
} // namespace

class MMContainerShardsTest : public AllocatorTest<AllocatorT> {
 protected:
  static AllocatorT::Config makeConfig(uint32_t shards) {
    AllocatorT::Config config;
    config.setCacheSize(10 * Slab::kSize);
    config.setMMContainerShards(shards);
    return config;
  }

  static PoolId addPool(AllocatorT& alloc) {
    return alloc.addPool("default", alloc.getCacheMemoryStats().ramCacheSize,
                         {kAllocSize});
  }

  static bool insertItem(AllocatorT& alloc,
                         PoolId pid,
                         const std::string& key) {
    auto handle = alloc.allocate(pid, key, kValueSize);
    return handle && alloc.insert(handle);
  }

  static std::vector<size_t> shardSizes(const AllocatorT& alloc, PoolId pid) {
    return CacheAllocatorTestWrapper::getMMContainerShardSizes(alloc, pid,
                                                               kCid);
  }

  static size_t sum(const std::vector<size_t>& sizes) {
    return std::accumulate(sizes.begin(), sizes.end(), size_t{0});
  }

  static size_t countItems(AllocatorT& alloc) {
    size_t numItems = 0;
    for (const auto& item : alloc) {
      (void)item;
      ++numItems;
    }
    return numItems;
  }
};

TEST_F(MMContainerShardsTest, ConfigValidation) {
  for (uint32_t shards : {1u, 2u, 64u}) {
    EXPECT_NO_THROW(makeConfig(shards).validate()) << shards;
  }
  for (uint32_t shards : {0u, 3u, 128u}) {
    EXPECT_THROW(makeConfig(shards).validate(), std::invalid_argument)
        << shards;
  }

  Lru2QAllocator::Config config2Q;
  config2Q.setMMContainerShards(2);
  EXPECT_THROW(config2Q.validate(), std::invalid_argument);
  config2Q.setMMContainerShards(1);
  EXPECT_NO_THROW(config2Q.validate());

  EXPECT_EQ("4", makeConfig(4).serialize()["mmContainerShards"]);
}

TEST_F(MMContainerShardsTest, InsertFindReplaceRemove) {
  AllocatorT alloc(makeConfig(kNumShards));
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

TEST_F(MMContainerShardsTest, EvictsOldestAcrossShards) {
  std::vector<std::string> evicted;
  auto config = makeConfig(kNumShards);
  config.setRemoveCallback([&evicted](const AllocatorT::RemoveCbData& data) {
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

TEST_F(MMContainerShardsTest, StatsAggregateAcrossShards) {
  AllocatorT alloc(makeConfig(kNumShards));
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
      alloc.getPoolEvictionAgeStats(pid, 0).getWarmEvictionStat(kCid);
  EXPECT_GE(ageStat.oldestElementAge, 2);
  EXPECT_LE(ageStat.projectedAge, ageStat.oldestElementAge);
}

TEST_F(MMContainerShardsTest, SlabReleaseWithEviction) {
  AllocatorT alloc(makeConfig(kNumShards));
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

TEST_F(MMContainerShardsTest, SlabReleaseWithMoving) {
  auto config = makeConfig(kNumShards);
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

TEST_F(MMContainerShardsTest, ConcurrentInsertFindRemove) {
  AllocatorT alloc(makeConfig(kNumShards));
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

TEST_F(MMContainerShardsTest, WarmRollRestoresShards) {
  auto config = makeConfig(kNumShards);
  config.enableCachePersistence(cacheDir_);

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

TEST_F(MMContainerShardsTest, WarmRollRefusesChangedShardCount) {
  const std::vector<std::pair<uint32_t, uint32_t>> transitions{
      {4, 2}, {4, 1}, {1, 4}};
  for (const auto& [saved, configured] : transitions) {
    const auto dir = fmt::format("{}/{}_to_{}", cacheDir_, saved, configured);
    util::makeDir(dir);
    auto config = makeConfig(saved);
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
} // namespace facebook::cachelib::tests
