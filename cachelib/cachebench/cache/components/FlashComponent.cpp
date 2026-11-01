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

#include <fmt/core.h>
#include <folly/system/HardwareConcurrency.h>

#include "cachelib/cachebench/cache/components/Components.h"
#include "cachelib/interface/components/FlashCacheComponent.h"
#include "cachelib/interface/utils/CoroFiberAdapter.h"
#include "cachelib/navy/block_cache/FifoPolicy.h"

using namespace facebook::cachelib::interface;

namespace facebook::cachelib::cachebench {
void validate(const CacheConfig& config) {
  if (config.enableItemDestructorCheck) {
    throw std::invalid_argument(
        "enableItemDestructorCheck is not yet supported for "
        "FlashCacheComponent in cachebench");
  } else if (!config.eventTrackerFilePath.empty()) {
    throw std::invalid_argument(
        "eventTracker is not yet supported for FlashCacheComponent in "
        "cachebench");
  } else if (config.navyAdmissionWriteRateMB ||
             config.nvmAdmissionRetentionTimeThreshold ||
             config.nvmAdmissionPolicyFactory) {
    throw std::invalid_argument(
        "admission policies are not supported for FlashCacheComponent in "
        "cachebench");
  }
}

std::unique_ptr<CacheComponent> createFlashCacheComponent(
    const CacheConfig& config) {
  validate(config);

  uint64_t cacheSize = config.cacheSizeMB * MB;
  std::shared_ptr<navy::DeviceEncryptor> encryptor = nullptr;
  if (config.navyEncryption && config.createEncryptor) {
    encryptor = config.createEncryptor();
  }

  std::string nvmCacheFilePath;
  std::unique_ptr<navy::Device> device;
  if (config.nvmCachePaths.empty()) {
    device = navy::createMemoryDevice(
        cacheSize, encryptor, config.navyBlockSize,
        folly::available_concurrency() /* numInitThreads */);
  } else {
    // Set up async options
    auto ioEngine = navy::IoEngine::Sync;
    size_t qDepth = 0;
    if (config.navyMaxNumReads || config.navyMaxNumWrites ||
        config.navyQDepth) {
      qDepth =
          config.navyQDepth > 0
              ? config.navyQDepth
              : std::max(config.navyMaxNumReads / config.navyReaderThreads,
                         config.navyMaxNumWrites / config.navyWriterThreads);
      ioEngine = config.navyEnableIoUring ? navy::IoEngine::IoUring
                                          : navy::IoEngine::LibAio;
    }

    // Set up NVM cache file options. If we get a directory, create a file. we
    // will clean it up. If we already have a file, user provided it. We will
    // also keep it around after the tests.
    std::vector<std::string> devicePaths = config.nvmCachePaths;
    bool truncate{false}; // should be false for RAID
    if (devicePaths.size() == 1) {
      auto path = devicePaths[0];
      bool isDir;
      bool isBlk = false;
      try {
        isDir = cachelib::util::isDir(path);
        if (!isDir) {
          isBlk = cachelib::util::isBlk(path);
        }
      } catch (const std::system_error&) {
        XLOGF(INFO, "nvmCachePath {} does not exist", path);
        isDir = false;
      }

      if (isDir) {
        const auto uniqueSuffix =
            fmt::format("nvmcache_{}_{}", ::getpid(), folly::Random::rand32());
        path = path + "/" + uniqueSuffix;
        util::makeDir(path);
        devicePaths[0] = path + "/navy_cache";
        nvmCacheFilePath = std::move(path);
        truncate = true;
      } else {
        truncate = !isBlk;
      }
    } else {
      XDCHECK_EQ(cacheSize % devicePaths.size(), 0UL)
          << "Cache size should be evenly divisible by number of files";
      cacheSize /= devicePaths.size();
    }
    auto stripeSize = config.navyRegionSizeMB * MB;

    device = navy::createFileDevice(devicePaths,
                                    cacheSize,
                                    truncate,
                                    config.navyBlockSize,
                                    stripeSize,
                                    config.deviceMaxWriteSize,
                                    ioEngine,
                                    qDepth,
                                    config.deviceEnableFDP,
                                    encryptor,
                                    false /* isExclusiveOwner */);
  }

  navy::BlockCache::Config bcConfig;
  bcConfig.name = "cachebench." + config.allocator;
  bcConfig.device = device.get();
  bcConfig.cacheBaseOffset = 0; // only adjust for metadata if we're persisting
  bcConfig.cacheSize = cacheSize;
  bcConfig.regionSize = config.navyRegionSizeMB * MB;
  bcConfig.cleanRegionsPool = config.navyCleanRegions;
  bcConfig.cleanRegionThreads = config.navyCleanRegionThreads;
  bcConfig.regionManagerFlushAsync = config.navyRegionManagerFlushAsync;
  bcConfig.stackSize = config.navyStackSizeKB * KB;
  // Use heuristic based on clean region count similarly to
  // BlockCacheConfig::setCleanRegions() in NavyConfig.cpp
  bcConfig.numInMemBuffers = 2 * config.navyCleanRegions;
  bcConfig.checksum = config.navyDataChecksum;
  bcConfig.indexConfig.setNumSparseMapBuckets(config.navyNumSparseMapBuckets)
      .setNumBucketsPerMutex(config.navyNumBucketsPerMutex);
  if (config.navyEnableItemHistoryTracking) {
    bcConfig.indexConfig.enableTrackItemHistory();
  }
  bcConfig.indexConfig.validate();
  bcConfig.checksumOffload = config.navyChecksumOffload;
  bcConfig.checksumOffloadMinSize = config.navyChecksumOffloadMinSize;
  bcConfig.checksumOffloadReadMinSize = config.navyChecksumOffloadReadMinSize;
  bcConfig.checksumOffloadCacheControl =
      config.navyChecksumOffloadCacheControl;
  bcConfig.checksumOffloadBatchReclaim = config.navyChecksumOffloadBatchReclaim;
  bcConfig.checksumOffloadBatch = config.navyChecksumOffloadBatch;
  bcConfig.checksumDeferSmall = config.navyChecksumDeferSmall;
  bcConfig.skipInMemValueVerify = config.navySkipInMemValueVerify;
  bcConfig.directFlush = config.navyBlockCacheDirectFlush;
  bcConfig.flushCopyOffload = config.navyBlockCacheFlushCopyOffload;
  // Note: LRU is not yet supported
  if (config.navySegmentedFifoSegmentRatio.empty() ||
      config.navySegmentedFifoSegmentRatio.size() == 1) {
    bcConfig.evictionPolicy = std::make_unique<navy::FifoPolicy>();
  } else {
    bcConfig.evictionPolicy = std::make_unique<navy::SegmentedFifoPolicy>(
        config.navySegmentedFifoSegmentRatio);
  }
  auto navyAllocatorCounts = config.getNavyAllocatorCounts();
  if (!navyAllocatorCounts.empty()) {
    bcConfig.allocatorsPerPriority = std::move(navyAllocatorCounts);
  }
  // Note: enableItemDestructorCheck is not yet supported
  if (config.enableItemDestructor) {
    bcConfig.itemDestructorEnabled = true;
    bcConfig.destructorCb = [](HashedKey /* hk */,
                               navy::BufferView /* value */,
                               navy::DestructorEvent /* event */) {};
  }

  if (config.navyHitsReinsertionThreshold) {
    bcConfig.reinsertionConfig.enableHitsBased(
        static_cast<uint8_t>(config.navyHitsReinsertionThreshold));
  } else if (config.navyProbabilityReinsertionThreshold) {
    bcConfig.reinsertionConfig.enablePctBased(
        config.navyProbabilityReinsertionThreshold);
  }

  // Note: unsupported BlockCache::Config params (not exposed for cachebench)
  //  persistParams
  //  cacheBaseOffset
  //  readBufferSize
  //  legacyEventTracker
  //  eventTracker
  //  inMemBufFlushRetryLimit
  //  preciseRemove

  utils::CoroFiberAdapter::Config executorConfig{
      .numThreads = config.fccCoroFiberAdapterNumThreads,
      .fibersPerThread = config.fccCoroFiberAdapterFibersPerThread,
      .stackSize =
          static_cast<uint32_t>(config.fccCoroFiberAdapterStackSizeKB * KB),
  };

  if (config.allocator.starts_with("consistent")) {
    auto hasher = std::make_unique<MurmurHash2>();
    auto cache = ConsistentFlashCacheComponent::create(
        "cachebench_consistent_flash", std::move(bcConfig), std::move(device),
        std::move(hasher), static_cast<uint8_t>(config.navyReqOrderShardsPower),
        executorConfig);
    XCHECK(cache.hasValue())
        << "Error creating ConsistentFlashCacheComponent: " << cache.error();
    return std::make_unique<ConsistentFlashCacheComponent>(
        std::move(cache).value());
  } else {
    auto cache =
        FlashCacheComponent::create("cachebench_flash", std::move(bcConfig),
                                    std::move(device), executorConfig);
    XCHECK(cache.hasValue())
        << "Error creating FlashCacheComponent: " << cache.error();
    return std::make_unique<FlashCacheComponent>(std::move(cache).value());
  }
}

} // namespace facebook::cachelib::cachebench
