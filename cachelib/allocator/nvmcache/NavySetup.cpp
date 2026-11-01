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

#include "cachelib/allocator/nvmcache/NavySetup.h"

#include <fmt/format.h>
#include <folly/logging/xlog.h>
#include <gmock/gmock.h>

#include "cachelib/allocator/nvmcache/NavyConfig.h"
#include "cachelib/navy/Factory.h"
#include "cachelib/navy/common/Types.h"
#include "cachelib/navy/scheduler/JobScheduler.h"
#include "cachelib/navy/testing/MockDevice.h"

namespace facebook {
namespace cachelib {

namespace {

// Default value for (almost) 1TB flash device = 5GB reserved for metadata
constexpr double kDefaultMetadataPercent = 0.5;
uint64_t megabytesToBytes(uint64_t mb) { return mb << 20; }

// Return a number that's equal or smaller than @num and aligned on @alignment
uint64_t alignDown(uint64_t num, uint64_t alignment) {
  return num - num % alignment;
}

// Return a number that's equal or bigger than @num and aligned on @alignment
uint64_t alignUp(uint64_t num, uint64_t alignment) {
  return alignDown(num + alignment - 1, alignment);
}

uint64_t getRegionSize(const navy::NavyConfig& config) {
  const auto& configs = config.enginesConfigs();
  uint64_t regionSize = configs[0].blockCache().getRegionSize();
  for (size_t idx = 1; idx < configs.size(); idx++) {
    if (regionSize != configs[idx].blockCache().getRegionSize()) {
      throw std::invalid_argument(fmt::format(
          "Blockcache {} region size: {}, not equal to block cache 0: {}", idx,
          configs[idx].blockCache().getRegionSize(), regionSize));
    }
  }
  return regionSize;
}

uint64_t getDeviceFileSize(const navy::NavyConfig& config) {
  const auto fileSize = config.getFileSize();
  if (!config.usesRaidFiles()) {
    return fileSize;
  }

  const auto regionSize = getRegionSize(config);
  if (regionSize == 0) {
    throw std::invalid_argument("Navy region size must be greater than zero");
  }
  return alignDown(fileSize, regionSize);
}

uint64_t getMetadataSize(const navy::NavyConfig& config,
                         uint64_t deviceSize,
                         uint64_t alignment) {
  if (alignment == 0) {
    throw std::invalid_argument("Navy block size must be greater than zero");
  }

  auto metadataSize = config.getDeviceMetadataSize();
  if (metadataSize == 0) {
    XDCHECK(folly::isPowTwo(alignment));
    const auto mask = ~(alignment - 1);
    metadataSize =
        static_cast<uint64_t>(kDefaultMetadataPercent * deviceSize / 100) &
        mask;
  }
  return alignUp(metadataSize, alignment);
}

// Create a bighash that ends at bigHashEndOffset.
//
// @param bigHashConfig bighash config
// @param ioAlignSize alignment size
// @param bigHashReservedSize Size reserved for bighash. Actual big hash size
// could be different due to alignment to bucket size.
// @param bigHashEndOffset The offset where this bighash ends.
// @param bigHashStartOffsetLimit The start offset of this bighash can not be
// smaller than or equal to this limit.
// @param proto The proto of engine pair that this bighash will be set into.
//
// @return the starting offset of the setup bighash (inclusive)
uint64_t setupBigHash(const navy::BigHashConfig& bigHashConfig,
                      uint32_t ioAlignSize,
                      uint64_t bigHashReservedSize,
                      uint64_t bigHashEndOffset,
                      uint64_t bigHashStartOffsetLimit,
                      cachelib::navy::EnginePairProto& proto) {
  auto bucketSize = bigHashConfig.getBucketSize();
  if (bucketSize != alignUp(bucketSize, ioAlignSize)) {
    throw std::invalid_argument(
        fmt::format("Bucket size: {} is not aligned to ioAlignSize: {}",
                    bucketSize, ioAlignSize));
  }

  const uint64_t bigHashCacheOffset =
      alignUp(bigHashEndOffset - bigHashReservedSize, bucketSize);
  const uint64_t bigHashCacheSize =
      alignDown(bigHashEndOffset - bigHashCacheOffset, bucketSize);

  auto bigHash = cachelib::navy::createBigHashProto();
  bigHash->setLayout(bigHashCacheOffset, bigHashCacheSize, bucketSize);

  // Bucket Bloom filter size, bytes
  //
  // Experiments showed that if we have 16 bytes for BF with 25 entries,
  // then optimal number of hash functions is 4 and false positive rate
  // below 10%.
  if (bigHashConfig.isBloomFilterEnabled()) {
    // We set 4 hash function unconditionally. This seems to be the best
    // for our use case. If BF size to bucket size ratio gets lower, try
    // to reduce number of hashes.
    constexpr uint32_t kNumHashes = 4;
    const uint32_t bitsPerHash =
        bigHashConfig.getBucketBfSize() * 8 / kNumHashes;
    bigHash->setBloomFilter(kNumHashes, bitsPerHash);
  }

  // Set number of mutexes from config
  bigHash->setNumMutexesPower(bigHashConfig.getNumMutexesPower());

  bigHash->setChecksumOffload(bigHashConfig.getChecksumOffload());

  proto.setBigHash(std::move(bigHash), bigHashConfig.getSmallItemMaxSize());

  if (bigHashCacheOffset <= bigHashStartOffsetLimit) {
    throw std::invalid_argument("NVM cache size is not big enough!");
  }
  XLOG(INFO) << "bighashStartingLimit: " << bigHashStartOffsetLimit
             << " bigHashCacheOffset: " << bigHashCacheOffset
             << " bigHashCacheSize: " << bigHashCacheSize;
  return bigHashCacheOffset;
}

// Setup a block cache from blockCacheOffset.
//
// @param blockCacheConfig
// @param blockCacheSize size of this block cache.
// @param ioAlignSize alignment size
// @param blockCacheOffset this block cache starts from this address (inclusive)
// @param useRaidFiles if set to true, the device will setup using raid.
// @param itemDestructorEnabled
// @param stackSize size of the stack used by the region_manager thread
// @param proto
//
// @return The end offset (exclusive) of the setup blockcache.
uint64_t setupBlockCache(const navy::BlockCacheConfig& blockCacheConfig,
                         uint64_t blockCacheSize,
                         uint32_t ioAlignSize,
                         uint64_t blockCacheOffset,
                         bool usesRaidFiles,
                         bool itemDestructorEnabled,
                         uint32_t stackSize,
                         cachelib::navy::EnginePairProto& proto) {
  auto regionSize = blockCacheConfig.getRegionSize();
  if (regionSize != alignUp(regionSize, ioAlignSize)) {
    throw std::invalid_argument(
        fmt::format("Region size: {} is not aligned to ioAlignSize: {}",
                    regionSize, ioAlignSize));
  }

  // Adjust starting size of block cache to ensure it is aligned to region
  // size which is what we use for the stripe size when using RAID0Device.

  if (usesRaidFiles) {
    auto adjustedBlockCacheOffset = alignUp(blockCacheOffset, regionSize);
    auto cacheSizeAdjustment = adjustedBlockCacheOffset - blockCacheOffset;
    XDCHECK_LT(cacheSizeAdjustment, blockCacheSize);
    blockCacheSize -= cacheSizeAdjustment;
    blockCacheOffset = adjustedBlockCacheOffset;
  }
  blockCacheSize = alignDown(blockCacheSize, regionSize);

  XLOG(INFO) << "blockcache: starting offset: " << blockCacheOffset
             << ", block cache size: " << blockCacheSize;

  auto blockCache = cachelib::navy::createBlockCacheProto();
  blockCache->setLayout(blockCacheOffset, blockCacheSize, regionSize);
  blockCache->setChecksum(blockCacheConfig.getDataChecksum());
  blockCache->setChecksumOffload(blockCacheConfig.getChecksumOffload(),
                                 blockCacheConfig.getChecksumOffloadMinSize());
  blockCache->setChecksumOffloadReadMinSize(
      blockCacheConfig.getChecksumOffloadReadMinSize());
  blockCache->setChecksumOffloadCacheControl(
      blockCacheConfig.getChecksumOffloadCacheControl());
  blockCache->setChecksumOffloadBatchReclaim(
      blockCacheConfig.getChecksumOffloadBatchReclaim());
  blockCache->setChecksumOffloadBatch(blockCacheConfig.getChecksumOffloadBatch());
  blockCache->setChecksumDeferSmall(blockCacheConfig.getChecksumDeferSmall());
  blockCache->setSkipInMemValueVerify(
      blockCacheConfig.getSkipInMemValueVerify());

  // set eviction policy
  auto segmentRatio = blockCacheConfig.getSFifoSegmentRatio();
  if (!segmentRatio.empty()) {
    blockCache->setSegmentedFifoEvictionPolicy(std::move(segmentRatio));
  } else if (blockCacheConfig.isLruEnabled()) {
    blockCache->setLruEvictionPolicy();
  } else {
    blockCache->setFifoEvictionPolicy();
  }
  blockCache->setCleanRegionsPool(blockCacheConfig.getCleanRegions(),
                                  blockCacheConfig.getCleanRegionThreads());
  blockCache->setRegionManagerFlushAsync(
      blockCacheConfig.isRegionManagerFlushAsync());

  blockCache->setRecoverEvictionPolicy(
      blockCacheConfig.isRecoverEvictionPolicy());
  blockCache->setDirectFlush(blockCacheConfig.isDirectFlush());
  blockCache->setFlushCopyOffload(blockCacheConfig.isFlushCopyOffload());
  blockCache->setUseCombinedEntryBlock(
      blockCacheConfig.isCombinedEntryBlockEnabled());
  blockCache->setNumAllocatorsPerPriority(
      blockCacheConfig.getNumAllocatorsPerPriority());

  blockCache->setReinsertionConfig(blockCacheConfig.getReinsertionConfig());

  blockCache->setNumInMemBuffers(blockCacheConfig.getNumInMemBuffers());
  blockCache->setItemDestructorEnabled(itemDestructorEnabled);
  blockCache->setStackSize(stackSize);
  blockCache->setPreciseRemove(blockCacheConfig.isPreciseRemove());
  blockCache->setIndexConfig(blockCacheConfig.getIndexConfig());
  blockCache->setLegacyEventTracker(blockCacheConfig.getLegacyEventTracker());

  proto.setBlockCache(std::move(blockCache));
  return blockCacheOffset + blockCacheSize;
}

// Setup the CacheProto, includes BigHashProto and BlockCacheProto,
// which is the configuration interface from Navy engine, and can be used to
// create BigHash and BlockCache engines.
//
// @param config            the configured NavyConfig
// @param device            the flash device
// @param proto             the output CacheProto
//
// @throw std::invalid_argument if input arguments are invalid
// Below is an illustration on how the cache engines and metadata are laid out
// on the device address space.
// |--------------------------------- Device -------------------------------|
// |--- Metadata ---|--- BC-0 ---|--- BC-1 ---|...|--- BH-1 ---|--- BH-0 ---|

void setupCacheProtos(const navy::NavyConfig& config,
                      const navy::Device& device,
                      cachelib::navy::CacheProto& proto,
                      const bool itemDestructorEnabled) {
  if (config.enginesConfigs()[config.enginesConfigs().size() - 1]
          .blockCache()
          .getSize() != 0) {
    throw std::invalid_argument(
        "Last pair of engines must have its block cache use up all the space "
        "left on device");
  }

  auto ioAlignSize = device.getIOAlignmentSize();
  const uint64_t totalCacheSize = device.getSize();

  const auto metadataSize =
      getMetadataSize(config, totalCacheSize, ioAlignSize);
  if (metadataSize >= totalCacheSize) {
    throw std::invalid_argument{
        fmt::format("Invalid metadata size: {}. Cache size: {}",
                    metadataSize,
                    totalCacheSize)};
  }
  proto.setMetadataSize(metadataSize);
  proto.setMetadataStagingSize(config.getMetadataStagingSize());

  proto.setMaxKeySize(config.getMaxKeySize());

  // Start offsets are inclusive. End offsets are exclusive.
  // For each engine pair, bigHashStartOffset will be calculated by setting up
  // bighash from its end offset. blockCacheEndOffset will be calculated by
  // setting block cache from its start offset.
  uint64_t blockCacheStartOffset = metadataSize;
  uint64_t blockCacheEndOffset = 0;
  uint64_t bigHashEndOffset = totalCacheSize;
  uint64_t bigHashStartOffset = 0;

  XLOG(INFO) << "metadataSize: " << metadataSize;
  for (size_t idx = 0; idx < config.enginesConfigs().size(); idx++) {
    XLOG(INFO) << "Setting up engine pair " << idx;
    const auto& enginesConfig = config.enginesConfigs()[idx];
    uint64_t blockCacheSize = enginesConfig.blockCache().getSize();
    auto enginePairProto = cachelib::navy::createEnginePairProto();
    enginePairProto->setName(enginesConfig.getName());

    if (enginesConfig.isBigHashEnabled()) {
      uint64_t bigHashSize =
          totalCacheSize * enginesConfig.bigHash().getSizePct() / 100ul;
      bigHashStartOffset = setupBigHash(
          enginesConfig.bigHash(), ioAlignSize, bigHashSize, bigHashEndOffset,
          blockCacheStartOffset, *enginePairProto);
      blockCacheSize = blockCacheSize == 0
                           ? bigHashStartOffset - blockCacheStartOffset
                           : blockCacheSize;
      XLOG(INFO) << "blockCacheSize " << blockCacheSize;
    } else {
      bigHashStartOffset = bigHashEndOffset;
      blockCacheSize = blockCacheSize == 0
                           ? bigHashStartOffset - blockCacheStartOffset
                           : blockCacheSize;
      XLOG(INFO) << "-- No bighash. blockCacheSize " << blockCacheSize;
    }

    // Set up BlockCache if enabled
    if (blockCacheSize > 0) {
      blockCacheEndOffset = setupBlockCache(
          enginesConfig.blockCache(), blockCacheSize, ioAlignSize,
          blockCacheStartOffset, config.usesRaidFiles(), itemDestructorEnabled,
          config.getStackSize(), *enginePairProto);
    }
    if (blockCacheEndOffset > bigHashStartOffset) {
      throw std::invalid_argument(
          fmt::format("Invalid engine size configurations. block cache ends at "
                      "{}, big hash starts at {}.",
                      blockCacheEndOffset, bigHashStartOffset));
    }
    proto.addEnginePair(std::move(enginePairProto));
    bigHashEndOffset = bigHashStartOffset;
    blockCacheStartOffset = blockCacheEndOffset;
  }
  proto.setEnginesSelector(config.getEnginesSelector());
}

void setAdmissionPolicy(const cachelib::navy::NavyConfig& config,
                        cachelib::navy::CacheProto& proto) {
  const std::string& policyName = config.getAdmissionPolicy();
  if (policyName.empty()) {
    return;
  }
  if (policyName == navy::NavyConfig::kAdmPolicyRandom) {
    proto.setRejectRandomAdmissionPolicy(config.randomAdmPolicy());
  } else if (policyName == navy::NavyConfig::kAdmPolicyDynamicRandom) {
    proto.setDynamicRandomAdmissionPolicy(config.dynamicRandomAdmPolicy());
  } else {
    throw std::invalid_argument{
        fmt::format("invalid policy name {}", policyName)};
  }
}

std::unique_ptr<cachelib::navy::JobScheduler> createJobScheduler(
    const navy::NavyConfig& config) {
  auto readerThreads = config.getReaderThreads();
  auto writerThreads = config.getWriterThreads();
  auto maxNumReads = config.getMaxNumReads();
  auto maxNumWrites = config.getMaxNumWrites();
  auto stackSize = config.getStackSize();
  auto reqOrderShardsPower = config.getNavyReqOrderingShards();
  auto readerPriority = config.getReaderThreadsPriority();
  auto writerPriority = config.getWriterThreadsPriority();
  if (maxNumReads == 0 && maxNumWrites == 0) {
    return cachelib::navy::createOrderedThreadPoolJobScheduler(
        readerThreads, writerThreads, reqOrderShardsPower, readerPriority,
        writerPriority);
  }

  return cachelib::navy::createNavyRequestScheduler(readerThreads,
                                                    writerThreads,
                                                    maxNumReads,
                                                    maxNumWrites,
                                                    stackSize,
                                                    reqOrderShardsPower);
}
} // namespace

NavyCacheSizes getNavyCacheSizes(const navy::NavyConfig& config) {
  const auto fileSize = getDeviceFileSize(config);
  const auto deviceSize = config.usesRaidFiles()
                              ? fileSize * config.getRaidPaths().size()
                              : fileSize;
  return {deviceSize,
          getMetadataSize(config, deviceSize, config.getBlockSize())};
}

std::unique_ptr<navy::Device> createDevice(
    const navy::NavyConfig& config,
    std::shared_ptr<navy::DeviceEncryptor> encryptor) {
  auto blockSize = config.getBlockSize();
  auto maxDeviceWriteSize = config.getDeviceMaxWriteSize();
  if (config.usesRaidFiles() || config.usesSimpleFile()) {
    auto stripeSize = 0;
    const auto fileSize = getDeviceFileSize(config);
    std::vector<std::string> filePaths;
    if (config.usesSimpleFile()) {
      filePaths.emplace_back(config.getFileName());
    } else {
      stripeSize = getRegionSize(config);
      filePaths = config.getRaidPaths();
    }

    return navy::createFileDevice(
        filePaths,
        fileSize,
        config.getTruncateFile(),
        blockSize,
        stripeSize,
        maxDeviceWriteSize > 0 ? alignDown(maxDeviceWriteSize, blockSize) : 0,
        config.getIoEngine(),
        config.getQDepth(),
        config.isFDPEnabled(),
        std::move(encryptor),
        config.getExclusiveOwner());
  } else {
    return navy::createMemoryDevice(config.getFileSize(), std::move(encryptor),
                                    blockSize);
  }
}

std::unique_ptr<navy::AbstractCache> createNavyCache(
    const navy::NavyConfig& config,
    navy::ExpiredCheck checkExpired,
    navy::DestructorCallback destructorCb,
    bool truncate,
    std::shared_ptr<navy::DeviceEncryptor> encryptor,
    bool itemDestructorEnabled,
    const navy::NavyPersistParams& persistParams) {
  auto device = createDevice(config, std::move(encryptor));

  std::unique_ptr<navy::MockDevice> mockDevice;
  switch (config.hasBadDeviceForTesting()) {
  case navy::BadDeviceStatus::DataCorruption:
    // Use mock device. This is for testing
    mockDevice = std::make_unique<navy::MockDevice>(
        device->getSize(), device->getIOAlignmentSize());
    mockDevice->setRealDevice(std::move(device));
    ON_CALL(*mockDevice, readImpl(testing::_, testing::_, testing::_))
        .WillByDefault(
            testing::Invoke([d = &mockDevice->getRealDeviceRef()](
                                uint64_t offset, uint32_t size, void* buffer) {
              XDCHECK_EQ(size % d->getIOAlignmentSize(), 0u);
              XDCHECK_EQ(offset % d->getIOAlignmentSize(), 0u);
              auto res = d->read(offset, size, buffer);
              if (!res) {
                return res;
              }

              // Corrupt the first byte which will break checksum
              reinterpret_cast<char*>(buffer)[0] += 1;
              return res;
            }));

    device = std::move(mockDevice);
    break;
  case navy::BadDeviceStatus::IoReqFailure:
    // Use mock device. This is for testing
    mockDevice = std::make_unique<navy::MockDevice>(
        device->getSize(), device->getIOAlignmentSize());
    mockDevice->setRealDevice(std::move(device));
    ON_CALL(*mockDevice, readImpl(testing::_, testing::_, testing::_))
        .WillByDefault(testing::Return(false));

    device = std::move(mockDevice);
    break;
  default:
    break;
  }

  auto proto = navy::createCacheProto();
  auto* devicePtr = device.get();
  proto->setDevice(std::move(device));
  proto->setJobScheduler(createJobScheduler(config));
  proto->setMaxConcurrentInserts(config.getMaxConcurrentInserts());
  proto->setMaxParcelMemory(megabytesToBytes(config.getMaxParcelMemoryMB()));
  proto->setUseEstimatedWriteSize(config.getUseEstimatedWriteSize());
  setAdmissionPolicy(config, *proto);
  proto->setExpiredCheck(checkExpired);
  proto->setDestructorCallback(destructorCb);
  proto->setPersistParams(persistParams);

  setupCacheProtos(config, *devicePtr, *proto, itemDestructorEnabled);

  auto cache = createCache(std::move(proto));
  XDCHECK(cache != nullptr);

  if (truncate) {
    cache->reset();
    return cache;
  }

  if (!cache->recover()) {
    XLOG(WARN) << "No recovery data found. Continuing with clean cache.";
  }
  return cache;
}
} // namespace cachelib
} // namespace facebook
