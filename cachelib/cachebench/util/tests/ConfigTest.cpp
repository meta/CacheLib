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

#include "cachelib/cachebench/util/Config.h"

namespace facebook::cachelib::cachebench {
namespace {

TEST(StressorConfigTest, DramIteratorDefaultsDisabled) {
  const StressorConfig config{folly::dynamic::object()};

  EXPECT_EQ(DramIteratorMode::kDisabled, config.dramIteratorMode);
  EXPECT_EQ(10000, config.dramIteratorIntervalMs);
  EXPECT_EQ(0, config.dramIteratorSleepMs);
  EXPECT_EQ(0, config.dramIteratorWorkMs);
  EXPECT_FALSE(config.usesDramIterator());
  EXPECT_FALSE(config.usesRegularDramIterator());
  EXPECT_FALSE(config.usesLockGroupDramIterator());
}

TEST(StressorConfigTest, ParsesRegularDramIteratorMode) {
  folly::dynamic configJson = folly::dynamic::object(
      "dramIteratorMode", "regular")("dramIteratorIntervalMs", 25)(
      "dramIteratorSleepMs", 3)("dramIteratorWorkMs", 7);

  const StressorConfig config{configJson};

  EXPECT_EQ(DramIteratorMode::kRegular, config.dramIteratorMode);
  EXPECT_TRUE(config.usesDramIterator());
  EXPECT_TRUE(config.usesRegularDramIterator());
  EXPECT_FALSE(config.usesLockGroupDramIterator());
  EXPECT_EQ(25, config.dramIteratorIntervalMs);
  EXPECT_EQ(3, config.dramIteratorSleepMs);
  EXPECT_EQ(7, config.dramIteratorWorkMs);
}

TEST(StressorConfigTest, ParsesLockGroupDramIteratorMode) {
  const StressorConfig config{
      folly::dynamic::object("dramIteratorMode", "lock_group")};

  EXPECT_EQ(DramIteratorMode::kLockGroup, config.dramIteratorMode);
  EXPECT_TRUE(config.usesDramIterator());
  EXPECT_FALSE(config.usesRegularDramIterator());
  EXPECT_TRUE(config.usesLockGroupDramIterator());
}

TEST(StressorConfigTest, RejectsInvalidDramIteratorMode) {
  EXPECT_THROW(StressorConfig{folly::dynamic::object("dramIteratorMode",
                                                     "not_an_iterator")},
               std::invalid_argument);
}

TEST(StressorConfigTest, RejectsZeroDramIteratorIntervalWhenEnabled) {
  EXPECT_THROW(StressorConfig{folly::dynamic::object(
                   "dramIteratorMode", "regular")("dramIteratorIntervalMs", 0)},
               std::invalid_argument);
}

TEST(StressorConfigTest, RejectsNegativeDramIteratorThrottleValues) {
  EXPECT_THROW(
      StressorConfig{folly::dynamic::object("dramIteratorSleepMs", -1)},
      std::invalid_argument);
  EXPECT_THROW(StressorConfig{folly::dynamic::object("dramIteratorWorkMs", -1)},
               std::invalid_argument);
}

TEST(StressorConfigTest, RejectsDramIteratorWorkWithoutSleep) {
  EXPECT_THROW(StressorConfig{folly::dynamic::object("dramIteratorWorkMs", 1)},
               std::invalid_argument);
}

TEST(CacheConfigTest, NavyChecksumDefaultsToCrc32) {
  const CacheConfig defaultConfig;
  const CacheConfig jsonConfig{folly::dynamic::object()};

  EXPECT_EQ("crc32", defaultConfig.navyChecksumType);
  EXPECT_EQ(navy::ChecksumType::Crc32, defaultConfig.getNavyChecksumType());
  EXPECT_EQ("crc32", jsonConfig.navyChecksumType);
  EXPECT_EQ(navy::ChecksumType::Crc32, jsonConfig.getNavyChecksumType());
}

TEST(CacheConfigTest, ParsesNavyChecksumType) {
  const CacheConfig legacyConfig{
      folly::dynamic::object("navyChecksumType", "crc32")};
  const CacheConfig crc32cConfig{
      folly::dynamic::object("navyChecksumType", "crc32c")};

  EXPECT_EQ("crc32", legacyConfig.navyChecksumType);
  EXPECT_EQ(navy::ChecksumType::Crc32, legacyConfig.getNavyChecksumType());
  EXPECT_EQ("crc32c", crc32cConfig.navyChecksumType);
  EXPECT_EQ(navy::ChecksumType::Crc32c, crc32cConfig.getNavyChecksumType());
}

TEST(CacheConfigTest, RejectsInvalidNavyChecksumType) {
  for (const auto* checksumType : {"", "CRC32C", "auto"}) {
    SCOPED_TRACE(checksumType);
    EXPECT_THROW(
        CacheConfig{folly::dynamic::object("navyChecksumType", checksumType)},
        std::invalid_argument);
  }
}

TEST(CacheConfigTest, NavyArenasDefaultToLegacySingleArena) {
  const CacheConfig config{folly::dynamic::object()};

  EXPECT_TRUE(config.navyArenas.empty());
  EXPECT_EQ(1, config.getNavyNumArenas());
}

TEST(CacheConfigTest, ParsesHitsPerSlabRebalanceMinDiff) {
  const CacheConfig config{
      folly::dynamic::object("poolRebalanceIntervalSec", 1)(
          "rebalanceStrategy", "hits")("rebalanceMinDiff", 123)};

  const auto strategy = config.getRebalanceStrategy();
  ASSERT_NE(nullptr, strategy);
  EXPECT_EQ("123", strategy->exportConfig().at("min_diff"));
}

TEST(CacheConfigTest, ParsesNavyArenas) {
  const auto arenas = folly::dynamic::array(
      folly::dynamic::object("name", "first")("sizePct", 40)("bigHashPct", 0),
      folly::dynamic::object("name", "second")("sizePct", 60)("bigHashPct",
                                                              50));
  const CacheConfig config{folly::dynamic::object("navyArenas", arenas)};

  ASSERT_EQ(2, config.navyArenas.size());
  EXPECT_EQ("first", config.navyArenas[0].name);
  EXPECT_EQ(40, config.navyArenas[0].sizePct);
  EXPECT_EQ(0, config.navyArenas[0].bigHashPct);
  EXPECT_EQ("second", config.navyArenas[1].name);
  EXPECT_EQ(60, config.navyArenas[1].sizePct);
  EXPECT_EQ(50, config.navyArenas[1].bigHashPct);
}

TEST(NavyArenaConfigTest, ConvertsBigHashSizeToRoundedDevicePercentage) {
  const NavyArenaConfig config{folly::dynamic::object("name", "arena")(
      "sizePct", 100)("bigHashPct", 50)};

  EXPECT_EQ(15, config.getBigHashDeviceSizePct(298, 1000));
  EXPECT_EQ(14, config.getBigHashDeviceSizePct(288, 1000));
}

TEST(CacheConfigTest, RejectsEmptyNavyArenas) {
  EXPECT_THROW(CacheConfig{folly::dynamic::object("navyArenas",
                                                  folly::dynamic::array())},
               std::invalid_argument);
}

TEST(CacheConfigTest, RejectsSingleNavyArena) {
  EXPECT_THROW(CacheConfig{folly::dynamic::object(
                   "navyArenas",
                   folly::dynamic::array(folly::dynamic::object("name", "only")(
                       "sizePct", 100)("bigHashPct", 50)))},
               std::invalid_argument);
}

TEST(CacheConfigTest, RejectsUnnamedNavyArena) {
  EXPECT_THROW(
      CacheConfig{folly::dynamic::object(
          "navyArenas",
          folly::dynamic::array(folly::dynamic::object("sizePct", 100)))},
      std::invalid_argument);
}

TEST(CacheConfigTest, RejectsDuplicateNavyArenaNames) {
  EXPECT_THROW(
      CacheConfig{folly::dynamic::object(
          "navyArenas",
          folly::dynamic::array(
              folly::dynamic::object("name", "duplicate")("sizePct", 50),
              folly::dynamic::object("name", "duplicate")("sizePct", 50)))},
      std::invalid_argument);
}

TEST(CacheConfigTest, RejectsNavyArenaSizesThatDoNotTotalOneHundred) {
  EXPECT_THROW(
      CacheConfig{folly::dynamic::object(
          "navyArenas",
          folly::dynamic::array(
              folly::dynamic::object("name", "first")("sizePct", 25),
              folly::dynamic::object("name", "second")("sizePct", 50)))},
      std::invalid_argument);
}

TEST(CacheConfigTest, RejectsNavyArenaWithoutBlockCache) {
  EXPECT_THROW(CacheConfig{folly::dynamic::object(
                   "navyArenas",
                   folly::dynamic::array(folly::dynamic::object(
                       "name", "arena")("sizePct", 100)("bigHashPct", 100)))},
               std::invalid_argument);
}

TEST(CacheConfigTest, ParsesNavyAllocatorsPerPriority) {
  const CacheConfig config{
      folly::dynamic::object("navyAllocatorsPerPriority", 3)};

  EXPECT_EQ(3, config.navyAllocatorsPerPriority);
  EXPECT_EQ((std::vector<uint32_t>{3}), config.getNavyAllocatorCounts());
}

TEST(CacheConfigTest, NavyAllocatorsPerPriorityDefaultsDisabled) {
  const CacheConfig config{folly::dynamic::object()};

  EXPECT_EQ(0, config.navyAllocatorsPerPriority);
  EXPECT_TRUE(config.getNavyAllocatorCounts().empty());
}

TEST(CacheConfigTest, ExpandsNavyAllocatorsAcrossPriorities) {
  const CacheConfig config{
      folly::dynamic::object("navyAllocatorsPerPriority", 3)(
          "navySegmentedFifoSegmentRatio", folly::dynamic::array(1, 2, 3))};

  EXPECT_EQ((std::vector<uint32_t>{3, 3, 3}), config.getNavyAllocatorCounts());
}

TEST(CacheConfigTest, ParsesNavyRegionManagerFlushAsync) {
  const CacheConfig config{
      folly::dynamic::object("navyRegionManagerFlushAsync", true)};

  EXPECT_TRUE(config.navyRegionManagerFlushAsync);
}

TEST(CacheConfigTest, NavyRegionManagerFlushAsyncDefaultsDisabled) {
  const CacheConfig config{folly::dynamic::object()};

  EXPECT_FALSE(config.navyRegionManagerFlushAsync);
}

TEST(CacheConfigTest, ParsesNavySparseMapIndexConfig) {
  const CacheConfig config{folly::dynamic::object(
      "navyNumSparseMapBuckets", 32 * 1024)("navyNumBucketsPerMutex", 128)};

  EXPECT_EQ(32 * 1024, config.navyNumSparseMapBuckets);
  EXPECT_EQ(128, config.navyNumBucketsPerMutex);
}

TEST(CacheConfigTest, SlabAsanPoisoningDefaultsDisabled) {
  const CacheConfig config{folly::dynamic::object()};

  EXPECT_FALSE(config.enableSlabAsanPoisoning);
}

TEST(CacheConfigTest, ParsesSlabAsanPoisoning) {
  const CacheConfig config{
      folly::dynamic::object("enableSlabAsanPoisoning", true)};

  EXPECT_TRUE(config.enableSlabAsanPoisoning);
}

TEST(CacheConfigTest, ParsesScriptedResidentMemoryMonitor) {
  const CacheConfig config{folly::dynamic::object(
      "memoryMonitorMode", "resident")("memoryMonitorIntervalMs", 25)(
      "memoryMonitorLowerLimitGB", 1)("memoryMonitorUpperLimitGB", 2)(
      "memoryMonitorMaxAdvisePercentPerIter",
      10)("memoryMonitorMaxReclaimPercentPerIter",
          15)("memoryMonitorMaxAdvisePercent",
              30)("memoryMonitorReclaimRateLimitWindowSecs",
                  4)("memoryMonitorScriptRepeat", true)(
      "memoryMonitorScript",
      folly::dynamic::array(
          folly::dynamic::object("valueGB", 1)("durationMs", 75),
          folly::dynamic::object("valueGB", 3)("durationMs", 125)))};

  const auto monitorConfig = config.getMemoryMonitorConfig();
  EXPECT_TRUE(config.memoryMonitorEnabled());
  EXPECT_EQ(MemoryMonitor::ResidentMemory, monitorConfig.mode);
  EXPECT_EQ(25, config.memoryMonitorIntervalMs);
  EXPECT_EQ(1, monitorConfig.lowerLimitGB);
  EXPECT_EQ(2, monitorConfig.upperLimitGB);
  EXPECT_EQ(10, monitorConfig.maxAdvisePercentPerIter);
  EXPECT_EQ(15, monitorConfig.maxReclaimPercentPerIter);
  EXPECT_EQ(30, monitorConfig.maxAdvisePercent);
  EXPECT_EQ(std::chrono::seconds{4}, monitorConfig.reclaimRateLimitWindowSecs);
  EXPECT_TRUE(config.memoryMonitorScriptRepeat);
  const std::vector<MemoryMonitorScriptPhase> expectedPhases{{1, 75}, {3, 125}};
  EXPECT_EQ(expectedPhases, config.memoryMonitorScript);
}

TEST(CacheConfigTest, ParsesFreeMemoryMonitorWithSystemReadings) {
  const CacheConfig config{folly::dynamic::object("memoryMonitorMode", "free")(
      "memoryMonitorIntervalMs", 100)};

  EXPECT_EQ(MemoryMonitor::FreeMemory, config.getMemoryMonitorConfig().mode);
  EXPECT_TRUE(config.memoryMonitorScript.empty());
}

TEST(CacheConfigTest, RejectsInvalidMemoryMonitorConfigurations) {
  EXPECT_THROW(CacheConfig{folly::dynamic::object("memoryMonitorMode", "bad")},
               std::invalid_argument);
  EXPECT_THROW(
      CacheConfig{folly::dynamic::object("memoryMonitorMode", "resident")(
          "memoryMonitorIntervalMs", 0)},
      std::invalid_argument);
  EXPECT_THROW(
      CacheConfig{folly::dynamic::object("memoryMonitorMode", "resident")(
          "memoryMonitorIntervalMs", 1)("memoryMonitorLowerLimitGB",
                                        2)("memoryMonitorUpperLimitGB", 2)},
      std::invalid_argument);
  EXPECT_THROW(CacheConfig{folly::dynamic::object(
                   "memoryMonitorScript",
                   folly::dynamic::array(
                       folly::dynamic::object("valueGB", 1)("durationMs", 1)))},
               std::invalid_argument);
  EXPECT_THROW(
      CacheConfig{folly::dynamic::object("memoryMonitorMode", "resident")(
          "memoryMonitorIntervalMs",
          1)("memoryMonitorScript",
             folly::dynamic::array(
                 folly::dynamic::object("valueGB", 0)("durationMs", 1)))},
      std::invalid_argument);
  EXPECT_THROW(
      CacheConfig{folly::dynamic::object("memoryMonitorMode", "resident")(
          "memoryMonitorIntervalMs",
          1)("memoryMonitorScript",
             folly::dynamic::array(
                 folly::dynamic::object("valueGB", 1)("durationMs", 0)))},
      std::invalid_argument);
  EXPECT_THROW(
      CacheConfig{folly::dynamic::object("memoryMonitorMode", "resident")(
          "memoryMonitorIntervalMs",
          2)("memoryMonitorScript",
             folly::dynamic::array(
                 folly::dynamic::object("valueGB", 1)("durationMs", 1)))},
      std::invalid_argument);
  EXPECT_THROW(
      CacheConfig{folly::dynamic::object("memoryMonitorScriptRepeat", true)},
      std::invalid_argument);
  EXPECT_THROW(CacheConfig{folly::dynamic::object("memoryMonitorScript",
                                                  folly::dynamic::array(1))},
               folly::TypeError);
  EXPECT_THROW(CacheConfig{folly::dynamic::object(
                   "memoryMonitorScript",
                   folly::dynamic::array(folly::dynamic::object("valueGB", 1.5)(
                       "durationMs", 1)))},
               folly::TypeError);
}

TEST(CacheConfigTest, AllowsMemoryMonitorWithTemporarySharedMemory) {
  const CacheConfig config{
      folly::dynamic::object("memoryMonitorMode", "resident")(
          "memoryMonitorIntervalMs", 1)("shmType", "tmp")};

  EXPECT_TRUE(config.memoryMonitorEnabled());
  EXPECT_EQ("tmp", config.shmType);
}

} // namespace
} // namespace facebook::cachelib::cachebench
