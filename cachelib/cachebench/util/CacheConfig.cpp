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

#include "cachelib/cachebench/util/CacheConfig.h"

#include <algorithm>

#include "cachelib/allocator/HitsPerSlabStrategy.h"
#include "cachelib/allocator/LruTailAgeStrategy.h"
#include "cachelib/allocator/RandomStrategy.h"

namespace facebook {
namespace cachelib {
namespace cachebench {
CacheConfig::CacheConfig(const folly::dynamic& configJson) {
  JSONSetVal(configJson, allocator);
  JSONSetVal(configJson, cacheDir);
  JSONSetVal(configJson, cacheSizeMB);
  JSONSetVal(configJson, poolRebalanceIntervalSec);
  JSONSetVal(configJson, moveOnSlabRelease);
  JSONSetVal(configJson, rebalanceStrategy);
  JSONSetVal(configJson, rebalanceMinSlabs);
  JSONSetVal(configJson, rebalanceMinDiff);
  JSONSetVal(configJson, rebalanceDiffRatio);

  JSONSetVal(configJson, htBucketPower);
  JSONSetVal(configJson, htLockPower);

  JSONSetVal(configJson, lruRefreshSec);
  JSONSetVal(configJson, lruRefreshRatio);
  JSONSetVal(configJson, mmReconfigureIntervalSecs);
  JSONSetVal(configJson, lruUpdateOnWrite);
  JSONSetVal(configJson, lruUpdateOnRead);
  JSONSetVal(configJson, tryLockUpdate);
  JSONSetVal(configJson, lruIpSpec);
  JSONSetVal(configJson, useCombinedLockForIterators);

  JSONSetVal(configJson, lru2qHotPct);
  JSONSetVal(configJson, lru2qColdPct);

  JSONSetVal(configJson, allocFactor);
  JSONSetVal(configJson, maxAllocSize);
  JSONSetVal(configJson, minAllocSize);
  JSONSetVal(configJson, allocSizes);

  JSONSetVal(configJson, numPools);
  JSONSetVal(configJson, poolSizes);

  JSONSetVal(configJson, nvmCacheSizeMB);
  JSONSetVal(configJson, nvmCacheMetadataSizeMB);
  JSONSetVal(configJson, nvmCachePaths);
  JSONSetVal(configJson, writeAmpDeviceList);

  JSONSetVal(configJson, navyBlockSize);
  JSONSetVal(configJson, navyRegionSizeMB);
  if (const auto* arenas = configJson.get_ptr("navyArenas")) {
    if (!arenas->isArray()) {
      folly::throw_exception<folly::TypeError>("array", arenas->type());
    }
    if (arenas->size() < 2) {
      throw std::invalid_argument(
          "navyArenas must contain at least two arenas");
    }
    uint64_t totalSizePct = 0;
    navyArenas.reserve(arenas->size());
    for (const auto& arena : *arenas) {
      NavyArenaConfig arenaConfig{arena};
      for (const auto& configuredArena : navyArenas) {
        if (configuredArena.name == arenaConfig.name) {
          throw std::invalid_argument(fmt::format(
              "Navy arena name '{}' is duplicated", arenaConfig.name));
        }
      }
      totalSizePct += arenaConfig.sizePct;
      navyArenas.push_back(arenaConfig);
    }
    if (totalSizePct != 100) {
      throw std::invalid_argument(fmt::format(
          "Navy arena size percentages must total 100, but total {}",
          totalSizePct));
    }
  }
  JSONSetVal(configJson, navySegmentedFifoSegmentRatio);
  JSONSetVal(configJson, navyAllocatorsPerPriority);
  JSONSetVal(configJson, navyReqOrderShardsPower);
  JSONSetVal(configJson, navyBigHashSizePct);
  JSONSetVal(configJson, navyBigHashBucketSize);
  JSONSetVal(configJson, navyBloomFilterPerBucketSize);
  JSONSetVal(configJson, navySmallItemMaxSize);
  JSONSetVal(configJson, navyParcelMemoryMB);
  JSONSetVal(configJson, navyHitsReinsertionThreshold);
  JSONSetVal(configJson, navyProbabilityReinsertionThreshold);
  JSONSetVal(configJson, navyReaderThreads);
  JSONSetVal(configJson, navyWriterThreads);
  JSONSetVal(configJson, navyMaxNumReads);
  JSONSetVal(configJson, navyMaxNumWrites);
  JSONSetVal(configJson, navyStackSizeKB);
  JSONSetVal(configJson, navyQDepth);
  JSONSetVal(configJson, navyEnableIoUring);
  JSONSetVal(configJson, navyCleanRegions);
  JSONSetVal(configJson, navyCleanRegionThreads);
  JSONSetVal(configJson, navyRegionManagerFlushAsync);
  JSONSetVal(configJson, navyAdmissionWriteRateMB);
  JSONSetVal(configJson, navyMaxConcurrentInserts);
  JSONSetVal(configJson, navyDataChecksum);
  JSONSetVal(configJson, navyChecksumOffload);
  JSONSetVal(configJson, navyBlockCacheDirectFlush);
  JSONSetVal(configJson, navyBlockCacheFlushCopyOffload);
  JSONSetVal(configJson, navyChecksumOffloadMinSize);
  JSONSetVal(configJson, navyChecksumOffloadReadMinSize);
  JSONSetVal(configJson, navyChecksumOffloadCacheControl);
  JSONSetVal(configJson, navyChecksumOffloadBatchReclaim);
  JSONSetVal(configJson, navyChecksumOffloadBatch);
  JSONSetVal(configJson, navyChecksumDeferSmall);
  JSONSetVal(configJson, navySkipInMemValueVerify);
  JSONSetVal(configJson, navyBigHashChecksumOffload);
  JSONSetVal(configJson, truncateItemToOriginalAllocSizeInNvm);
  JSONSetVal(configJson, navyEncryption);
  JSONSetVal(configJson, deviceMaxWriteSize);
  JSONSetVal(configJson, deviceEnableFDP);
  JSONSetVal(configJson, navyEnableAccessTimeMap);
  JSONSetVal(configJson, navyAccessTimeMapMaxSize);

  JSONSetVal(configJson, memoryOnlyTTL);

  JSONSetVal(configJson, usePosixShm);
  JSONSetVal(configJson, shmType);
  JSONSetVal(configJson, hugePageSize);
  JSONSetVal(configJson, slabHugePageSize);
  JSONSetVal(configJson, accessHugePageSize);
  JSONSetVal(configJson, hugePageMountDir);
  JSONSetVal(configJson, lockMemory);
  JSONSetVal(configJson, enableSlabAsanPoisoning);
  JSONSetVal(configJson, memoryMonitorMode);
  JSONSetVal(configJson, memoryMonitorIntervalMs);
  JSONSetVal(configJson, memoryMonitorLowerLimitGB);
  JSONSetVal(configJson, memoryMonitorUpperLimitGB);
  JSONSetVal(configJson, memoryMonitorMaxAdvisePercentPerIter);
  JSONSetVal(configJson, memoryMonitorMaxReclaimPercentPerIter);
  JSONSetVal(configJson, memoryMonitorMaxAdvisePercent);
  JSONSetVal(configJson, memoryMonitorReclaimRateLimitWindowSecs);
  JSONSetVal(configJson, memoryMonitorScriptRepeat);
  if (const auto* script = configJson.get_ptr("memoryMonitorScript")) {
    if (!script->isArray()) {
      folly::throw_exception<folly::TypeError>("array", script->type());
    }
    for (const auto& phase : *script) {
      if (!phase.isObject()) {
        folly::throw_exception<folly::TypeError>("object", phase.type());
      }
      const auto* value = phase.get_ptr("valueGB");
      const auto* duration = phase.get_ptr("durationMs");
      if (value == nullptr || duration == nullptr) {
        throw std::invalid_argument(
            "each memoryMonitorScript phase requires valueGB and durationMs");
      }
      const auto valueGB = value->getInt();
      const auto durationMs = duration->getInt();
      if (valueGB <= 0 || durationMs <= 0) {
        throw std::invalid_argument(
            "memoryMonitorScript valueGB and durationMs "
            "must be greater than zero");
      }
      memoryMonitorScript.push_back(MemoryMonitorScriptPhase{
          static_cast<uint64_t>(valueGB), static_cast<uint64_t>(durationMs)});
    }
  }
  if (configJson.count("memoryTiers")) {
    for (auto& it : configJson["memoryTiers"]) {
      memoryTierConfigs.push_back(
          MemoryTierConfig(it).getMemoryTierCacheConfig());
    }
  }

  JSONSetVal(configJson, useTraceTimeStamp);
  JSONSetVal(configJson, printNvmCounters);
  JSONSetVal(configJson, tickerSynchingSeconds);
  JSONSetVal(configJson, enableItemDestructorCheck);
  JSONSetVal(configJson, enableItemDestructor);
  JSONSetVal(configJson, nvmAdmissionRetentionTimeThreshold);
  JSONSetVal(configJson, eventTrackerFilePath);
  JSONSetVal(configJson, eventTrackerSamplingRate);
  JSONSetVal(configJson, eventTrackerQueueSize);

  JSONSetVal(configJson, fccCoroFiberAdapterNumThreads);
  JSONSetVal(configJson, fccCoroFiberAdapterFibersPerThread);
  JSONSetVal(configJson, fccCoroFiberAdapterStackSizeKB);

  JSONSetVal(configJson, customConfigJson);
  JSONSetVal(configJson, navyEnableItemHistoryTracking);
  JSONSetVal(configJson, navyNumSparseMapBuckets);
  JSONSetVal(configJson, navyNumBucketsPerMutex);
  // if you added new fields to the configuration, update the JSONSetVal
  // to make them available for the json configs and increment the size
  // below
  checkCorrectSize<CacheConfig, 1144>();

  if (numPools != poolSizes.size()) {
    throw std::invalid_argument(fmt::format(
        "number of pools must be the same as the pool size distribution. "
        "numPools: {}, poolSizes.size(): {}",
        numPools, poolSizes.size()));
  }
  if (memoryMonitorMode != "disabled" && memoryMonitorMode != "resident" &&
      memoryMonitorMode != "free") {
    throw std::invalid_argument(
        fmt::format("unsupported memoryMonitorMode: {}", memoryMonitorMode));
  }
  if (memoryMonitorEnabled() && memoryMonitorIntervalMs == 0) {
    throw std::invalid_argument(
        "memoryMonitorIntervalMs must be greater than zero when enabled");
  }
  if (!memoryMonitorEnabled() && !memoryMonitorScript.empty()) {
    throw std::invalid_argument(
        "memoryMonitorScript requires memory monitoring to be enabled");
  }
  if (memoryMonitorScriptRepeat && memoryMonitorScript.empty()) {
    throw std::invalid_argument(
        "memoryMonitorScriptRepeat requires a non-empty script");
  }
  if (memoryMonitorEnabled() &&
      memoryMonitorLowerLimitGB >= memoryMonitorUpperLimitGB) {
    throw std::invalid_argument(
        "memoryMonitorLowerLimitGB must be less than "
        "memoryMonitorUpperLimitGB");
  }
  for (const auto& phase : memoryMonitorScript) {
    if (phase.durationMs < memoryMonitorIntervalMs) {
      throw std::invalid_argument(
          "memoryMonitorScript durationMs must be at least "
          "memoryMonitorIntervalMs");
    }
  }
  if (memoryMonitorMaxAdvisePercentPerIter > 100 ||
      memoryMonitorMaxReclaimPercentPerIter > 100 ||
      memoryMonitorMaxAdvisePercent > 100) {
    throw std::invalid_argument(
        "memory monitor percentage values must not exceed 100");
  }
}

NavyArenaConfig::NavyArenaConfig(const folly::dynamic& configJson) {
  JSONSetVal(configJson, name);
  JSONSetVal(configJson, sizePct);
  JSONSetVal(configJson, bigHashPct);

  if (name.empty()) {
    throw std::invalid_argument("Navy arena name must not be empty");
  }
  if (sizePct == 0 || sizePct > 100) {
    throw std::invalid_argument(fmt::format(
        "Navy arena '{}' size percentage must be in the range [1, 100]", name));
  }
  if (bigHashPct >= 100) {
    throw std::invalid_argument(fmt::format(
        "Navy arena '{}' BigHash percentage must be in the range [0, 100)",
        name));
  }
}

unsigned int NavyArenaConfig::getBigHashDeviceSizePct(
    uint64_t arenaSize, uint64_t deviceSize) const {
  if (deviceSize == 0) {
    throw std::invalid_argument("NVM device size must be greater than zero");
  }
  const auto requestedSize = arenaSize * bigHashPct / 100;
  // Add half the divisor so integer division rounds to the nearest percentage.
  return static_cast<unsigned int>((requestedSize * 100 + deviceSize / 2) /
                                   deviceSize);
}

std::shared_ptr<RebalanceStrategy> CacheConfig::getRebalanceStrategy() const {
  if (poolRebalanceIntervalSec == 0) {
    return nullptr;
  }

  if (rebalanceStrategy == "tail-age") {
    auto config = LruTailAgeStrategy::Config{
        rebalanceDiffRatio, static_cast<unsigned int>(rebalanceMinSlabs)};
    return std::make_shared<LruTailAgeStrategy>(config);
  } else if (rebalanceStrategy == "hits") {
    auto config = HitsPerSlabStrategy::Config{
        rebalanceDiffRatio, static_cast<unsigned int>(rebalanceMinSlabs)};
    config.minDiff = static_cast<unsigned int>(rebalanceMinDiff);
    return std::make_shared<HitsPerSlabStrategy>(config);
  } else {
    // use random strategy to just trigger some slab release.
    return std::make_shared<RandomStrategy>(
        RandomStrategy::Config{static_cast<unsigned int>(rebalanceMinSlabs)});
  }
}

std::vector<uint32_t> CacheConfig::getNavyAllocatorCounts() const {
  if (navyAllocatorsPerPriority == 0) {
    return {};
  }

  const auto numPriorities =
      std::max<size_t>(1, navySegmentedFifoSegmentRatio.size());
  return std::vector<uint32_t>(numPriorities, navyAllocatorsPerPriority);
}

bool CacheConfig::memoryMonitorEnabled() const {
  return memoryMonitorMode != "disabled";
}

MemoryMonitor::Config CacheConfig::getMemoryMonitorConfig() const {
  MemoryMonitor::Config config;
  if (memoryMonitorMode == "resident") {
    config.mode = MemoryMonitor::ResidentMemory;
  } else if (memoryMonitorMode == "free") {
    config.mode = MemoryMonitor::FreeMemory;
  }
  config.lowerLimitGB = memoryMonitorLowerLimitGB;
  config.upperLimitGB = memoryMonitorUpperLimitGB;
  config.maxAdvisePercentPerIter = memoryMonitorMaxAdvisePercentPerIter;
  config.maxReclaimPercentPerIter = memoryMonitorMaxReclaimPercentPerIter;
  config.maxAdvisePercent = memoryMonitorMaxAdvisePercent;
  config.reclaimRateLimitWindowSecs =
      std::chrono::seconds{memoryMonitorReclaimRateLimitWindowSecs};
  return config;
}

MemoryTierConfig::MemoryTierConfig(const folly::dynamic& configJson) {
  JSONSetVal(configJson, ratio);
  JSONSetVal(configJson, memBindNodes);

  checkCorrectSize<MemoryTierConfig, 40>();
}
} // namespace cachebench
} // namespace cachelib
} // namespace facebook
