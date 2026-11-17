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

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/allocator/HitsPerSlabStrategy.h"
#include "cachelib/allocator/MemoryMonitor.h"
#include "cachelib/allocator/tests/CacheAllocatorTestWrapper.h"
#include "cachelib/common/Utils.h"

namespace facebook::cachelib::tests {
namespace {
constexpr size_t kMiB = 1024 * 1024;
constexpr size_t kGiB = 1024 * kMiB;
std::atomic<size_t> rss{5 * kGiB};
std::atomic<size_t> available{5 * kGiB};
size_t rssProvider() { return rss.load(); }
size_t availableProvider() { return available.load(); }

// Delegate to a real allocator, limiting only the synchronous reclaim result
// to exercise partial failures without changing CacheAllocator's final APIs.
class RecoveryAllocator : public CacheBase {
 public:
  explicit RecoveryAllocator(const LruAllocator::Config& config)
      : allocator_(config) {}

  auto addPool(folly::StringPiece name,
               size_t bytes,
               const std::set<uint32_t>& sizes = {}) {
    return allocator_.addPool(name, bytes, sizes);
  }
  auto allocate(PoolId pool, folly::StringPiece key, uint32_t size) {
    return allocator_.allocate(pool, key, size);
  }
  void insertOrReplace(const LruAllocator::WriteHandle& handle) {
    allocator_.insertOrReplace(handle);
  }

  const std::string getCacheName() const override {
    return base().getCacheName();
  }
  bool isObjectCache() const override { return false; }
  const MemoryPool& getPool(PoolId id) const override {
    return base().getPool(id);
  }
  PoolStats getPoolStats(PoolId id) const override {
    return base().getPoolStats(id);
  }
  AllSlabReleaseEvents getAllSlabReleaseEvents(PoolId id) const override {
    return base().getAllSlabReleaseEvents(id);
  }
  PoolEvictionAgeStats getPoolEvictionAgeStats(PoolId id,
                                               unsigned int n) const override {
    return base().getPoolEvictionAgeStats(id, n);
  }
  util::StatsMap getNvmCacheStatsMap() const override {
    return base().getNvmCacheStatsMap();
  }
  std::unordered_map<std::string, uint64_t> getLegacyEventTrackerStatsMap()
      const override {
    return base().getLegacyEventTrackerStatsMap();
  }
  folly::F14FastMap<std::string, uint64_t> getEventTrackerStatsMap()
      const override {
    return base().getEventTrackerStatsMap();
  }
  CacheMetadata getCacheMetadata() const noexcept override {
    return base().getCacheMetadata();
  }
  CacheMemoryStats getCacheMemoryStats() const override {
    return base().getCacheMemoryStats();
  }
  GlobalCacheStats getGlobalCacheStats() const override {
    return base().getGlobalCacheStats();
  }
  SlabReleaseStats getSlabReleaseStats() const override {
    return base().getSlabReleaseStats();
  }
  void incrementAbortedSlabReleases() override {
    base().incrementAbortedSlabReleases();
  }
  bool isFastShutdownTriggered() const override {
    return base().isFastShutdownTriggered();
  }
  std::set<PoolId> getRegularPoolIdsForResize() const override {
    return base().getRegularPoolIdsForResize();
  }
  std::set<PoolId> getPoolIds() const override { return base().getPoolIds(); }
  std::string getPoolName(PoolId id) const override {
    return base().getPoolName(id);
  }
  std::set<PoolId> getRegularPoolIds() const override {
    return base().getRegularPoolIds();
  }
  std::set<PoolId> getCCachePoolIds() const override {
    return base().getCCachePoolIds();
  }
  bool autoResizeEnabledForPool(PoolId id) const override {
    return base().autoResizeEnabledForPool(id);
  }
  const ICompactCache& getCompactCache(PoolId id) const override {
    return base().getCompactCache(id);
  }
  bool resizePools(PoolId, PoolId, size_t) override {
    ADD_FAILURE() << "Unexpected pool resize";
    return false;
  }
  void resizeCompactCaches() override {
    ADD_FAILURE() << "Unexpected compact-cache resize";
  }
  std::map<std::string, std::string> serializeConfigParams() const override {
    ADD_FAILURE() << "Unexpected config serialization";
    return {};
  }
  void releaseSlab(PoolId id,
                   ClassId cls,
                   SlabReleaseMode mode,
                   const void* hint = nullptr) override {
    allocator_.releaseSlab(id, cls, mode, hint);
  }
  void releaseSlab(PoolId id,
                   ClassId victim,
                   ClassId receiver,
                   SlabReleaseMode mode,
                   const void* hint = nullptr) override {
    allocator_.releaseSlab(id, victim, receiver, mode, hint);
  }
  PoolAdviseReclaimData calcNumSlabsToAdviseReclaim(size_t target) override {
    ++planningCalls;
    return allocator_.calcNumSlabsToAdviseReclaim(target);
  }
  PoolAdviseReclaimData calcNumSlabsToAdviseReclaim(
      size_t target, const std::set<PoolId>& pools) override {
    ++planningCalls;
    return allocator_.calcNumSlabsToAdviseReclaim(target, pools);
  }
  size_t getNumSlabsAdvised(const std::set<PoolId>& pools) const override {
    return allocator_.getNumSlabsAdvised(pools);
  }
  unsigned int reclaimSlabs(PoolId pool, size_t count) override {
    auto& memory = CacheAllocatorTestWrapper::getMemoryAllocator(&allocator_);
    const auto actual =
        memory.reclaimSlabsAndGrow(pool, std::min(count, maxReclaimPerCall));
    requests.emplace_back(pool, count);
    reclaimed += actual;
    EXPECT_LE(actual, std::numeric_limits<unsigned int>::max());
    return static_cast<unsigned int>(actual);
  }

  size_t maxReclaimPerCall{std::numeric_limits<size_t>::max()};
  size_t reclaimed{0};
  size_t planningCalls{0};
  std::vector<std::pair<PoolId, size_t>> requests;

 private:
  CacheBase& base() { return allocator_; }
  const CacheBase& base() const { return allocator_; }
  LruAllocator allocator_;
};

class MemoryMonitorRecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    rss = 5 * kGiB;
    available = 5 * kGiB;
    util::setRSSMemoryAdvising(rssProvider);
    util::setCgroupMemoryAdvising(availableProvider);
  }

  void TearDown() override {
    util::setRSSMemoryAdvising(nullptr);
    util::setCgroupMemoryAdvising(nullptr);
  }

  static LruAllocator::Config cacheConfig(size_t bytes = 128 * kMiB) {
    LruAllocator::Config config;
    config.setCacheSize(bytes);
    config.setAccessConfig({18, 10});
    // Memory monitoring selects shared memory, required by MADV_REMOVE.
    auto monitor = monitorConfig(false);
    monitor.mode = MemoryMonitor::TestMode;
    config.enableMemoryMonitor(std::chrono::hours(1), monitor);
    config.setDelayCacheWorkersStart();
    return config;
  }

  static MemoryMonitor::Config monitorConfig(bool enabled) {
    MemoryMonitor::Config config;
    config.mode = MemoryMonitor::ResidentMemory;
    config.lowerLimitGB = 15;
    config.upperLimitGB = 16;
    config.maxAdvisePercent = 50;
    config.maxAdvisePercentPerIter = 25;
    // floor(1% of 1 GiB / 4 MiB) = two slabs per low-pressure tick.
    config.maxReclaimPercentPerIter = 1;
    if (enabled) {
      config.enableEmptyPoolRecovery = true;
    }
    return config;
  }

  static bool tick(MemoryMonitor& monitor) {
    constexpr auto timeout = std::chrono::seconds(30);
    const auto before = monitor.getRunCount();
    if (!monitor.start(std::chrono::hours(1))) {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (monitor.getRunCount() == before &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    const auto stopped = monitor.stop(timeout);
    return stopped && monitor.getRunCount() == before + 1;
  }

  static PoolId addPool(RecoveryAllocator& cache,
                        const char* name,
                        size_t slabs = 4) {
    return cache.addPool(name, slabs * Slab::kSize, {1024 * 1024});
  }

  static void fillPool(RecoveryAllocator& cache, PoolId pool, size_t slabs) {
    for (size_t i = 0; i < slabs * 4; ++i) {
      const auto key = std::to_string(pool) + ":" + std::to_string(i);
      auto handle = cache.allocate(pool, key, 900 * 1024);
      ASSERT_TRUE(handle);
      cache.insertOrReplace(handle);
    }
    ASSERT_EQ(slabs * Slab::kSize, cache.getPool(pool).getCurrentUsedSize());
  }

  static void advisePool(RecoveryAllocator& cache, PoolId pool, size_t slabs) {
    for (size_t i = 0; i < slabs; ++i) {
      cache.releaseSlab(pool, 0, SlabReleaseMode::kAdvise);
    }
    ASSERT_EQ(slabs, cache.getPool(pool).getNumSlabsAdvised());
  }

  template <typename Cache>
  static size_t advised(const Cache& cache) {
    size_t count = 0;
    for (const auto pool : cache.getRegularPoolIds()) {
      count += cache.getPool(pool).getNumSlabsAdvised();
    }
    return count;
  }
};

TEST_F(MemoryMonitorRecoveryTest, DefaultKeepsLegacyEmptyPoolBehaviour) {
  RecoveryAllocator cache(cacheConfig());
  const auto pool = addPool(cache, "mapping");
  fillPool(cache, pool, 4);
  advisePool(cache, pool, 4);
  auto config = monitorConfig(false);
  MemoryMonitor monitor(cache, config, nullptr, 100);
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(tick(monitor));
    EXPECT_EQ(4, advised(cache));
    EXPECT_FALSE(cache.allocate(pool, "still-empty", 128));
  }
  EXPECT_TRUE(cache.requests.empty());
}

TEST_F(MemoryMonitorRecoveryTest, RecoversEmptyPoolsWithinEachTickBudget) {
  RecoveryAllocator cache(cacheConfig());
  const auto first = addPool(cache, "blobs");
  const auto second = addPool(cache, "mapping");
  fillPool(cache, first, 4);
  fillPool(cache, second, 4);
  advisePool(cache, first, 4);
  advisePool(cache, second, 4);
  MemoryMonitor monitor(cache, monitorConfig(true), nullptr, 100);
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(3, cache.getPool(first).getNumSlabsAdvised());
  EXPECT_EQ(3, cache.getPool(second).getNumSlabsAdvised());
  EXPECT_TRUE(cache.allocate(second, "usable-again", 128));
  for (int i = 0; i < 8 && advised(cache) != 0; ++i) {
    const auto before = advised(cache);
    ASSERT_TRUE(tick(monitor));
    EXPECT_LE(before - advised(cache), 2);
  }
  EXPECT_EQ(0, advised(cache));
  EXPECT_EQ(8, cache.reclaimed);
}

TEST_F(MemoryMonitorRecoveryTest, RotatesWhenBudgetIsSmallerThanPoolCount) {
  RecoveryAllocator cache(cacheConfig());
  std::vector<PoolId> pools;
  for (const auto name : {"blobs", "presence", "mapping"}) {
    const auto pool = addPool(cache, name);
    pools.push_back(pool);
    fillPool(cache, pool, 4);
    advisePool(cache, pool, 4);
  }
  MemoryMonitor monitor(cache, monitorConfig(true), nullptr, 12);
  ASSERT_TRUE(tick(monitor));
  for (const auto pool : {pools[0], pools[1]}) {
    fillPool(cache, pool, 1);
    cache.releaseSlab(pool, 0, SlabReleaseMode::kAdvise);
    ASSERT_EQ(4, cache.getPool(pool).getNumSlabsAdvised());
  }
  ASSERT_TRUE(tick(monitor));
  const std::vector<std::pair<PoolId, size_t>> expected{
      {pools[0], 1}, {pools[1], 1}, {pools[2], 1}, {pools[0], 1}};
  EXPECT_EQ(expected, cache.requests);
  EXPECT_EQ(4, cache.reclaimed);
}

TEST_F(MemoryMonitorRecoveryTest, RestrictsRecoveryToMonitoredPools) {
  RecoveryAllocator cache(cacheConfig());
  const auto excluded = addPool(cache, "excluded");
  const auto first = addPool(cache, "first");
  const auto second = addPool(cache, "second");
  const auto third = addPool(cache, "third");
  for (const auto pool : {excluded, first, second, third}) {
    fillPool(cache, pool, 4);
    advisePool(cache, pool, 4);
  }
  auto config = monitorConfig(true);
  config.poolIds = {first, second, third};
  MemoryMonitor monitor(cache, config, nullptr, 100);
  ASSERT_TRUE(tick(monitor));
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(4, cache.getPool(excluded).getNumSlabsAdvised());
  for (const auto pool : config.poolIds) {
    EXPECT_LT(cache.getPool(pool).getNumSlabsAdvised(), 4);
  }
  for (const auto& request : cache.requests) {
    EXPECT_TRUE(config.poolIds.contains(request.first));
  }
  EXPECT_EQ(4, cache.reclaimed);
}

TEST_F(MemoryMonitorRecoveryTest, MixedPoolsUseExistingReclaimDistribution) {
  RecoveryAllocator cache(cacheConfig());
  const auto used = addPool(cache, "used");
  const auto empty = addPool(cache, "empty");
  fillPool(cache, used, 4);
  fillPool(cache, empty, 4);
  advisePool(cache, empty, 4);
  const std::vector<std::pair<PoolId, size_t>> expected{{empty, 2}};
  MemoryMonitor monitor(cache, monitorConfig(true), nullptr, 100);
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(0, cache.getPool(used).getNumSlabsAdvised());
  EXPECT_EQ(2, cache.getPool(empty).getNumSlabsAdvised());
  EXPECT_EQ(expected, cache.requests);
}

TEST_F(MemoryMonitorRecoveryTest, PartialReclaimDoesNotRunLaterInDeadband) {
  RecoveryAllocator cache(cacheConfig());
  const auto used = addPool(cache, "used");
  const auto pool = addPool(cache, "mapping");
  fillPool(cache, used, 4);
  fillPool(cache, pool, 4);
  advisePool(cache, pool, 4);
  cache.maxReclaimPerCall = 1;
  MemoryMonitor monitor(cache, monitorConfig(true), nullptr, 0);
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(3, advised(cache));
  const auto requests = cache.requests;
  cache.maxReclaimPerCall = 100;
  rss = 15 * kGiB + kGiB / 2;
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(3, advised(cache));
  EXPECT_EQ(requests, cache.requests);
  rss = 5 * kGiB;
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(1, advised(cache));
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(0, advised(cache));
}

TEST_F(MemoryMonitorRecoveryTest, ZeroReturnRetriesOnlyAfterAnotherLowTick) {
  RecoveryAllocator cache(cacheConfig());
  const auto used = addPool(cache, "used");
  const auto pool = addPool(cache, "mapping");
  fillPool(cache, used, 4);
  fillPool(cache, pool, 4);
  advisePool(cache, pool, 4);
  cache.maxReclaimPerCall = 0;
  MemoryMonitor monitor(cache, monitorConfig(true), nullptr, 0);
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(4, advised(cache));
  const auto requests = cache.requests;
  rss = 30 * kGiB;
  cache.maxReclaimPerCall = 100;
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(4, advised(cache));
  EXPECT_EQ(requests, cache.requests);
  rss = 5 * kGiB;
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(2, advised(cache));
}

TEST_F(MemoryMonitorRecoveryTest, RateLimitBlocksOutstandingReclaimDebt) {
  RecoveryAllocator cache(cacheConfig());
  const auto used = addPool(cache, "used");
  const auto pool = addPool(cache, "mapping");
  fillPool(cache, used, 4);
  fillPool(cache, pool, 4);
  advisePool(cache, pool, 4);
  auto config = monitorConfig(true);
  // RateLimiter keeps one additional sample: two intervals need three ticks.
  config.reclaimRateLimitWindowSecs = std::chrono::hours(2);
  MemoryMonitor monitor(cache, config, nullptr, 0);
  ASSERT_TRUE(tick(monitor));
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(4, advised(cache));
  cache.maxReclaimPerCall = 1;
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(3, advised(cache));
  const auto requests = cache.requests;
  rss = 15 * kGiB + kGiB / 2;
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(3, advised(cache));
  EXPECT_EQ(requests, cache.requests);
  cache.maxReclaimPerCall = 100;
  rss = 5 * kGiB + 33 * kMiB;
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(3, advised(cache));
  EXPECT_EQ(requests, cache.requests);
  EXPECT_EQ(1, cache.reclaimed);
}

TEST_F(MemoryMonitorRecoveryTest,
       ZeroBudgetCancelsStaleAdviceWithNoAdvisedSlabs) {
  RecoveryAllocator cache(cacheConfig());
  const auto pool = addPool(cache, "mapping");
  fillPool(cache, pool, 4);
  auto config = monitorConfig(true);
  config.maxReclaimPercentPerIter = 0;
  MemoryMonitor monitor(cache, config, nullptr, 4);
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(0, advised(cache));
  rss = 15 * kGiB + kGiB / 2;
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(0, advised(cache));
  EXPECT_TRUE(cache.requests.empty());
}

TEST_F(MemoryMonitorRecoveryTest, FreeMemoryModeRecoversAtHighAvailableMemory) {
  RecoveryAllocator cache(cacheConfig());
  const auto pool = addPool(cache, "mapping");
  fillPool(cache, pool, 4);
  advisePool(cache, pool, 4);
  auto config = monitorConfig(true);
  config.mode = MemoryMonitor::FreeMemory;
  config.lowerLimitGB = 1;
  config.upperLimitGB = 2;
  MemoryMonitor monitor(cache, config, nullptr, 100);
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(2, advised(cache));
}

TEST_F(MemoryMonitorRecoveryTest, FreeMemoryPressureStillAdvises) {
  for (const auto enabled : {false, true}) {
    RecoveryAllocator cache(cacheConfig());
    const auto pool = addPool(cache, "mapping");
    fillPool(cache, pool, 4);
    auto config = monitorConfig(enabled);
    config.mode = MemoryMonitor::FreeMemory;
    config.lowerLimitGB = 1;
    config.upperLimitGB = 2;
    MemoryMonitor monitor(cache, config, nullptr, 0);
    // A zero provider result falls back to /proc; one byte is the lowest
    // reading the public provider accepts and is still extreme pressure.
    available = 1;
    ASSERT_TRUE(tick(monitor));
    EXPECT_EQ(2, advised(cache));
    EXPECT_TRUE(cache.requests.empty());
  }
}

TEST_F(MemoryMonitorRecoveryTest, TestModeKeepsItsLegacyTargetPath) {
  for (const auto enabled : {false, true}) {
    RecoveryAllocator cache(cacheConfig());
    const auto pool = addPool(cache, "mapping");
    fillPool(cache, pool, 4);
    advisePool(cache, pool, 4);
    auto config = monitorConfig(enabled);
    config.mode = MemoryMonitor::TestMode;
    MemoryMonitor monitor(cache, config, nullptr, 0);
    ASSERT_TRUE(tick(monitor));
    EXPECT_EQ(4, advised(cache));
    EXPECT_TRUE(cache.requests.empty());
  }
}

TEST_F(MemoryMonitorRecoveryTest, RepeatedPressureAndRecovery) {
  RecoveryAllocator cache(cacheConfig());
  const auto pool = addPool(cache, "mapping");
  auto config = monitorConfig(true);
  MemoryMonitor monitor(cache, config, nullptr, 0);
  for (int cycle = 0; cycle < 3; ++cycle) {
    fillPool(cache, pool, 4);
    rss = 30 * kGiB;
    ASSERT_TRUE(tick(monitor));
    EXPECT_EQ(2, advised(cache));
    rss = 5 * kGiB;
    ASSERT_TRUE(tick(monitor));
    EXPECT_EQ(0, advised(cache));
    EXPECT_TRUE(cache.allocate(pool, "recovered", 128));
  }
}

TEST_F(MemoryMonitorRecoveryTest, EmptyPoolSetDoesNotKeepStaleAdvice) {
  RecoveryAllocator cache(cacheConfig());
  MemoryMonitor monitor(cache, monitorConfig(true), nullptr, 100);
  ASSERT_TRUE(tick(monitor));
  const auto pool = addPool(cache, "added-later");
  fillPool(cache, pool, 4);
  rss = 15 * kGiB + kGiB / 2;
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(0, advised(cache));
  EXPECT_TRUE(cache.requests.empty());
}

TEST_F(MemoryMonitorRecoveryTest, SingleSlabDoesNotGrowBeyondPoolCapacity) {
  RecoveryAllocator cache(cacheConfig());
  const auto pool = addPool(cache, "single", 1);
  fillPool(cache, pool, 1);
  advisePool(cache, pool, 1);
  MemoryMonitor monitor(cache, monitorConfig(true), nullptr, 100);
  ASSERT_TRUE(tick(monitor));
  ASSERT_TRUE(tick(monitor));
  EXPECT_EQ(1, cache.reclaimed);
  EXPECT_EQ(Slab::kSize, cache.getPool(pool).getPoolUsableSize());
  EXPECT_TRUE(cache.allocate(pool, "recovered", 128));
}

TEST_F(MemoryMonitorRecoveryTest, ConcurrentEmptyAndMixedPoolRecovery) {
  const auto waitForProgress = [](const std::atomic<size_t>& counter,
                                  size_t before) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (counter.load() <= before &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    return counter.load() > before;
  };
  for (const auto mixed : {false, true}) {
    SCOPED_TRACE(mixed);
    RecoveryAllocator cache(cacheConfig());
    const auto pool = addPool(cache, "mapping");
    if (mixed) {
      const auto used = addPool(cache, "used");
      fillPool(cache, used, 4);
    }
    MemoryMonitor monitor(cache, monitorConfig(true), nullptr, 0);
    for (int cycle = 0; cycle < 5; ++cycle) {
      fillPool(cache, pool, 4);
      advisePool(cache, pool, 4);
      ASSERT_FALSE(cache.allocate(pool, "before-recovery", 64));
      std::atomic<size_t> successful{0};
      std::atomic<size_t> failed{0};
      std::jthread client([&](const std::stop_token& stop) {
        size_t key = 0;
        while (!stop.stop_requested()) {
          auto handle =
              cache.allocate(pool, "concurrent" + std::to_string(key++), 64);
          if (handle) {
            cache.insertOrReplace(handle);
            ++successful;
          } else {
            ++failed;
          }
        }
      });
      ASSERT_TRUE(waitForProgress(failed, 0));
      const auto plansBefore = cache.planningCalls;
      ASSERT_TRUE(tick(monitor));
      EXPECT_EQ(mixed ? 1 : 0, cache.planningCalls - plansBefore);
      EXPECT_EQ(2, advised(cache));
      ASSERT_TRUE(waitForProgress(successful, 0));
      const auto successfulBefore = successful.load();
      ASSERT_TRUE(tick(monitor));
      ASSERT_TRUE(waitForProgress(successful, successfulBefore));
      client.request_stop();
      client.join();
      EXPECT_EQ(0, advised(cache));
    }
    EXPECT_EQ(20, cache.reclaimed);
    EXPECT_TRUE(cache.allocate(pool, "after-cycles", 128));
  }
}

TEST_F(MemoryMonitorRecoveryTest, GitCacheGeometryWithScaledMappingPool) {
  for (const auto enabled : {false, true}) {
    SCOPED_TRACE(enabled);
    // Git's cache/index geometry and monitor limits, with a scaled mapping
    // pool so the differential remains suitable for sanitizer CI.
    constexpr size_t mappingBytes = 4 * Slab::kSize;
    LruAllocator cache(cacheConfig(20 * kGiB));
    const auto presence = cache.addPool("blobstore-presence", 1073532108);
    const auto graph = cache.addPool("commit_graph", 1073532108);
    const auto mapping = cache.addPool("bonsai_git_mapping", mappingBytes);
    const auto blobs = cache.addPool("blobstore-blobs", 18422905448);
    cache.addPool("unused", cache.getCacheMemoryStats().unReservedSize);
    size_t nextKey = 0;
    const auto fullMappingSlabs = mappingBytes / Slab::kSize;
    while (cache.getPool(mapping).getCurrentUsedSize() / Slab::kSize <
               fullMappingSlabs &&
           nextKey < 10000000) {
      const auto key = "scm.mononoke.bonsai_git_mapping.repo.1234.sha1." +
                       std::to_string(nextKey++);
      auto handle = cache.allocate(mapping, key, 64);
      ASSERT_TRUE(handle);
      cache.insertOrReplace(handle);
    }
    ASSERT_EQ(fullMappingSlabs,
              cache.getPool(mapping).getCurrentUsedSize() / Slab::kSize);
    for (const auto pool : {presence, graph, blobs}) {
      for (size_t i = 0; i < 64; ++i) {
        const auto key = std::to_string(pool) + ":" + std::to_string(i);
        auto handle = cache.allocate(pool, key, 60000);
        ASSERT_TRUE(handle);
        cache.insertOrReplace(handle);
      }
    }
    auto config = monitorConfig(enabled);
    config.upperLimitGB = 25;
    config.maxReclaimPercentPerIter = 25;
    HitsPerSlabStrategy::Config strategyConfig;
    strategyConfig.diffRatio = 0.05;
    strategyConfig.minSlabs = 1;
    strategyConfig.minLruTailAge = 30;
    MemoryMonitor monitor(cache, config,
                          std::make_shared<HitsPerSlabStrategy>(strategyConfig),
                          0);
    rss = 30 * kGiB;
    for (int i = 0; i < 3; ++i) {
      ASSERT_TRUE(tick(monitor));
    }
    for (const auto pool : cache.getRegularPoolIds()) {
      ASSERT_EQ(0, cache.getPool(pool).getCurrentUsedSize());
    }
    ASSERT_FALSE(cache.allocate(mapping, "during-pressure", 64));
    rss = 5 * kGiB;
    for (int i = 0; i < 30; ++i) {
      const auto before = advised(cache);
      ASSERT_TRUE(tick(monitor));
      EXPECT_LE(before - advised(cache), 640);
    }
    size_t successful = 0;
    for (size_t i = 0; i < 10000; ++i) {
      auto handle = cache.allocate(mapping, "probe" + std::to_string(i), 64);
      if (handle) {
        cache.insertOrReplace(handle);
        ++successful;
      }
    }
    EXPECT_EQ(enabled ? 10000 : 0, successful);
    if (enabled) {
      EXPECT_EQ(0, advised(cache));
      EXPECT_TRUE(cache.peek("probe0"));
    }
    std::printf(
        "scaled_git_layout recovery=%d mapping_inserts=%zu probe_success=%zu "
        "advised_remaining=%zu\n",
        enabled, nextKey, successful, advised(cache));
  }
}
} // namespace
} // namespace facebook::cachelib::tests
