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

#include <folly/testing/TestUtil.h>

#include <cstring>
#include <filesystem>

#include "cachelib/allocator/CacheAllocatorConfig.h"
#include "cachelib/allocator/MemoryTierCacheConfig.h"
#include "cachelib/allocator/ReadOnlySharedCacheView.h"
#include "cachelib/allocator/memory/Slab.h"
#include "cachelib/allocator/tests/TestBase.h"
#include "cachelib/common/EventSink.h"
#include "cachelib/common/EventTracker.h"
#include "cachelib/shm/HugePageTestUtils.h"
#include "cachelib/shm/ShmCommon.h"

namespace facebook {
namespace cachelib {

namespace tests {

using AllocatorT = LruAllocator;
using MemoryTierConfigs = CacheAllocatorConfig<AllocatorT>::MemoryTierConfigs;

size_t defaultTotalSize = 1 * 1024LL * 1024LL * 1024LL;

class CacheAllocatorConfigTest : public testing::Test {};

MemoryTierConfigs generateTierConfigs(size_t numTiers,
                                      MemoryTierCacheConfig& config) {
  return MemoryTierConfigs(numTiers, config);
}

TEST_F(CacheAllocatorConfigTest, MultipleTier0Config) {
  AllocatorT::Config config;
  // Throws if vector of tier configs is emptry
  EXPECT_THROW(config.configureMemoryTiers(MemoryTierConfigs()),
               std::invalid_argument);
}

TEST_F(CacheAllocatorConfigTest, MultipleTier1Config) {
  AllocatorT::Config config;
  // Accepts single-tier configuration
  config.setCacheSize(defaultTotalSize)
      .configureMemoryTiers({MemoryTierCacheConfig::fromShm().setRatio(1)});
  config.validateMemoryTiers();
}

TEST_F(CacheAllocatorConfigTest, InvalidTierRatios) {
  AllocatorT::Config config;
  EXPECT_THROW(config.configureMemoryTiers(generateTierConfigs(
                   config.kMaxCacheMemoryTiers + 1,
                   MemoryTierCacheConfig::fromShm().setRatio(0))),
               std::invalid_argument);
}

TEST_F(CacheAllocatorConfigTest, TotalCacheSizeLessThanRatios) {
  AllocatorT::Config config;
  // Throws if total cache size is set to 0
  config.setCacheSize(defaultTotalSize)
      .configureMemoryTiers(
          {MemoryTierCacheConfig::fromShm().setRatio(defaultTotalSize + 1)});
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

namespace {
MemoryMonitor::Config advisingMonitorConfig() {
  MemoryMonitor::Config memConfig;
  memConfig.mode = MemoryMonitor::ResidentMemory;
  memConfig.maxAdvisePercent = 10;
  memConfig.lowerLimitGB = 1;
  memConfig.upperLimitGB = 10;
  return memConfig;
}
} // namespace

TEST_F(CacheAllocatorConfigTest, HugePageLargerThanSlabWithMemoryMonitor) {
  AllocatorT::Config config;
  config.setCacheSize(defaultTotalSize)
      .enableHugePages(PageSize(Slab::kSize * 256))
      .enableMemoryMonitor(std::chrono::seconds{2}, advisingMonitorConfig());
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST_F(CacheAllocatorConfigTest, HugePageDividingSlabWithMemoryMonitor) {
  AllocatorT::Config config;
  config.setCacheSize(defaultTotalSize)
      .enableHugePages(PageSize(Slab::kSize / 2))
      .enableMemoryMonitor(std::chrono::seconds{2}, advisingMonitorConfig());
  EXPECT_NO_THROW(config.validate());
}

TEST_F(CacheAllocatorConfigTest, HugePageLargerThanSlabWithoutMonitor) {
  AllocatorT::Config config;
  config.setCacheSize(defaultTotalSize)
      .enableHugePages(PageSize(Slab::kSize * 256));
  EXPECT_NO_THROW(config.validate());
}

TEST_F(CacheAllocatorConfigTest, ConflictingHugePageMountDirsThrow) {
  if (!PageSize::supportedHugePageSizes().contains(
          PageSize::kHugePageSize2MB)) {
    GTEST_SKIP() << "2MB huge pages not supported by kernel";
  }
  const PageSize huge{PageSize::kHugePageSize2MB};

  AllocatorT::Config config;
  config.enableSlabHugePages(huge, "/mnt/hugetlbfs-a");
  EXPECT_THROW(config.enableAccessHugePages(huge, "/mnt/hugetlbfs-b"),
               std::invalid_argument);
  EXPECT_NO_THROW(config.enableAccessHugePages(huge, "/mnt/hugetlbfs-a"));
  EXPECT_NO_THROW(config.enableAccessHugePages(huge));
  EXPECT_EQ("/mnt/hugetlbfs-a", config.hugePageMountDir);
}

TEST_F(CacheAllocatorConfigTest, PosixShmDirectory) {
  AllocatorT::Config config;
  // Needs cache persistence, as usePosixForShm() does.
  EXPECT_THROW(config.usePosixShmDirectory("/segments"), std::invalid_argument);
  config.enableCachePersistence("/cache");
  EXPECT_THROW(config.usePosixShmDirectory(""), std::invalid_argument);

  config.usePosixShmDirectory("/segments");
  EXPECT_TRUE(config.isUsingPosixShm());
  EXPECT_EQ("/segments", config.posixShmDirectory);
  EXPECT_EQ("/segments", config.serialize()["posixShmDirectory"]);
}

TEST_F(CacheAllocatorConfigTest, PosixShmDirectoryWarmRoll) {
  folly::test::TemporaryDirectory cacheDir;
  folly::test::TemporaryDirectory segmentDir("", "/dev/shm");
  const std::string segments = segmentDir.path().string();
  const auto makeConfig = [&](const std::string& directory) {
    AllocatorT::Config config;
    config.setCacheSize(64 * Slab::kSize)
        .enableCachePersistence(cacheDir.path().string())
        .usePosixShmDirectory(directory);
    return config;
  };
  {
    AllocatorT cache(AllocatorT::SharedMemNew, makeConfig(segments));
    const auto pool =
        cache.addPool("default", cache.getCacheMemoryStats().ramCacheSize);
    auto handle = cache.allocate(pool, "key", 100);
    ASSERT_NE(nullptr, handle);
    std::memset(handle->getMemory(), 'v', 100);
    cache.insertOrReplace(handle);
    handle.reset();
    ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, cache.shutDown());
  }
  ASSERT_FALSE(std::filesystem::is_empty(segments));
  {
    ReadOnlySharedCacheView view(cacheDir.path().string(), true /* posix */,
                                 nullptr, PageSize(), "", segments);
    EXPECT_NE(0u, view.getShmMappingAddress());
  }

  {
    AllocatorT cache(AllocatorT::SharedMemAttach, makeConfig(segments));
    auto handle = cache.find("key");
    ASSERT_NE(nullptr, handle);
    EXPECT_EQ('v', *reinterpret_cast<const char*>(handle->getMemory()));
    handle.reset();
    ASSERT_EQ(AllocatorT::ShutDownStatus::kSuccess, cache.shutDown());
  }

  EXPECT_TRUE(AllocatorT::cleanupStrayShmSegments(
      cacheDir.path().string(), true /* posix */, "", segments));
  EXPECT_TRUE(std::filesystem::is_empty(segments));
}

TEST_F(CacheAllocatorConfigTest, SerializeEvictionPolicyLru) {
  LruAllocator::Config config;
  auto serialized = config.serialize();
  EXPECT_EQ(serialized["evictionPolicy"], "MMLru");
}

TEST_F(CacheAllocatorConfigTest, SerializeEvictionPolicy2Q) {
  Lru2QAllocator::Config config;
  auto serialized = config.serialize();
  EXPECT_EQ(serialized["evictionPolicy"], "MM2Q");
}

TEST_F(CacheAllocatorConfigTest, SerializeEvictionPolicyTinyLFU) {
  TinyLFUAllocator::Config config;
  auto serialized = config.serialize();
  EXPECT_EQ(serialized["evictionPolicy"], "MMTinyLFU");
}

TEST_F(CacheAllocatorConfigTest, SerializeEvictionPolicyWTinyLFU) {
  WTinyLFUAllocator::Config config;
  auto serialized = config.serialize();
  EXPECT_EQ(serialized["evictionPolicy"], "MMWTinyLFU");
}

TEST_F(CacheAllocatorConfigTest, SerializeMemMonitorPoolIds) {
  AllocatorT::Config config;
  EXPECT_EQ(config.serialize()["memMonitorPoolIds"], "all");

  MemoryMonitor::Config memConfig = advisingMonitorConfig();
  memConfig.poolIds = {3, 0};
  config.enableMemoryMonitor(std::chrono::seconds{2}, memConfig);
  EXPECT_EQ(config.serialize()["memMonitorPoolIds"], "0,3");
}

TEST_F(CacheAllocatorConfigTest, SetEventTrackerConfigFactory) {
  AllocatorT::Config config;
  int factoryCallCount = 0;
  config.setEventTrackerConfigFactory([&factoryCallCount]() {
    ++factoryCallCount;
    EventTracker::Config trackerConfig;
    trackerConfig.queueSize = 100;
    trackerConfig.eventSink = std::make_unique<InMemoryEventSink>();
    trackerConfig.sampler = std::make_unique<FurcHashSampler>(1);
    return trackerConfig;
  });
  EXPECT_TRUE(config.eventTrackerConfigFactory != nullptr);
  EXPECT_EQ(factoryCallCount, 0);

  // Invoke the factory and verify it produces a valid config.
  auto trackerConfig = config.eventTrackerConfigFactory();
  EXPECT_EQ(factoryCallCount, 1);
  EXPECT_EQ(trackerConfig.queueSize, 100);
  EXPECT_NE(trackerConfig.eventSink, nullptr);
  EXPECT_NE(trackerConfig.sampler, nullptr);
}

TEST_F(CacheAllocatorConfigTest, EventTrackerConfigFactoryProducesFreshConfig) {
  AllocatorT::Config config;
  config.setEventTrackerConfigFactory([]() {
    EventTracker::Config trackerConfig;
    trackerConfig.queueSize = 50;
    trackerConfig.eventSink = std::make_unique<InMemoryEventSink>();
    return trackerConfig;
  });

  // Call factory twice and verify each produces independent, valid configs.
  auto config1 = config.eventTrackerConfigFactory();
  auto config2 = config.eventTrackerConfigFactory();
  EXPECT_NE(config1.eventSink, nullptr);
  EXPECT_NE(config2.eventSink, nullptr);
  // The two sinks must be distinct objects.
  EXPECT_NE(config1.eventSink.get(), config2.eventSink.get());
}

TEST_F(CacheAllocatorConfigTest, SerializeEventTrackerConfigFactory) {
  AllocatorT::Config config;
  // Before setting, serialization should show "empty".
  auto serialized = config.serialize();
  EXPECT_EQ(serialized["eventTrackerConfigFactory"], "empty");

  config.setEventTrackerConfigFactory([]() {
    EventTracker::Config trackerConfig;
    trackerConfig.queueSize = 10;
    trackerConfig.eventSink = std::make_unique<InMemoryEventSink>();
    return trackerConfig;
  });
  serialized = config.serialize();
  EXPECT_EQ(serialized["eventTrackerConfigFactory"], "set");
}

TEST_F(CacheAllocatorConfigTest, HeapHugePageCache) {
  const size_t cacheSize = 20 * Slab::kSize;
  PageSize huge{PageSize::kHugePageSize2MB};

  if (!canReserveHugePages(cacheSize, huge)) {
    GTEST_SKIP() << "Not enough free 2MB huge pages for " << cacheSize;
  }

  LruAllocator::Config configHuge;
  configHuge.setCacheSize(cacheSize);
  configHuge.enableHugePages(huge);
  LruAllocator allocHuge(configHuge);
  EXPECT_FALSE(allocHuge.isOnShm());

  auto poolId = allocHuge.addPool("test_huge",
                                  allocHuge.getCacheMemoryStats().ramCacheSize);
  for (int i = 0; i < 100; ++i) {
    auto h = allocHuge.allocate(poolId, folly::to<std::string>(i), 100);
    if (h) {
      allocHuge.insertOrReplace(h);
    }
  }
}

TEST_F(CacheAllocatorConfigTest, HugePageSplitSetters) {
  if (!PageSize::supportedHugePageSizes().contains(
          PageSize::kHugePageSize2MB)) {
    GTEST_SKIP() << "2MB huge pages not supported by kernel";
  }
  const PageSize huge{PageSize::kHugePageSize2MB};
  {
    AllocatorT::Config config;
    EXPECT_FALSE(config.slabHugePageSize.isHugePage());
    EXPECT_FALSE(config.accessHugePageSize.isHugePage());
    EXPECT_TRUE(config.hugePageMountDir.empty());
  }
  {
    AllocatorT::Config config;
    config.enableHugePages(huge, "/mnt/huge");
    EXPECT_TRUE(config.slabHugePageSize.isHugePage());
    EXPECT_TRUE(config.accessHugePageSize.isHugePage());
    EXPECT_EQ("/mnt/huge", config.hugePageMountDir);
  }
  {
    AllocatorT::Config config;
    config.enableSlabHugePages(huge, "/mnt/huge");
    EXPECT_TRUE(config.slabHugePageSize.isHugePage());
    EXPECT_FALSE(config.accessHugePageSize.isHugePage());
    config.enableAccessHugePages(huge);
    EXPECT_TRUE(config.accessHugePageSize.isHugePage());
    EXPECT_EQ("/mnt/huge", config.hugePageMountDir);
  }
  {
    AllocatorT::Config config;
    EXPECT_THROW(config.enableSlabHugePages(PageSize{}), std::invalid_argument);
    EXPECT_THROW(config.enableAccessHugePages(PageSize{}),
                 std::invalid_argument);
  }
}

TEST_F(CacheAllocatorConfigTest, HugePageAccessOnlyWithMemoryMonitor) {
  AllocatorT::Config config;
  config.setCacheSize(defaultTotalSize)
      .enableAccessHugePages(PageSize(Slab::kSize * 256))
      .enableMemoryMonitor(std::chrono::seconds{2}, advisingMonitorConfig());
  EXPECT_NO_THROW(config.validate());
}

TEST_F(CacheAllocatorConfigTest, HugePageSlabOnlyWithMemoryMonitor) {
  AllocatorT::Config config;
  config.setCacheSize(defaultTotalSize)
      .enableSlabHugePages(PageSize(Slab::kSize * 256))
      .enableMemoryMonitor(std::chrono::seconds{2}, advisingMonitorConfig());
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST_F(CacheAllocatorConfigTest, SerializeHugePageConfig) {
  if (!PageSize::supportedHugePageSizes().contains(
          PageSize::kHugePageSize2MB)) {
    GTEST_SKIP() << "2MB huge pages not supported by kernel";
  }
  AllocatorT::Config config;
  auto def = config.serialize();
  EXPECT_EQ(def["slabHugePageSize"], "0");
  EXPECT_EQ(def["accessHugePageSize"], "0");
  EXPECT_EQ(def["hugePageMountDir"], "");
  config.enableSlabHugePages(PageSize(PageSize::kHugePageSize2MB), "/mnt/huge");
  auto set = config.serialize();
  EXPECT_EQ(set["slabHugePageSize"],
            std::to_string(PageSize::kHugePageSize2MB));
  EXPECT_EQ(set["accessHugePageSize"], "0");
  EXPECT_EQ(set["hugePageMountDir"], "/mnt/huge");
}

} // namespace tests
} // namespace cachelib
} // namespace facebook
