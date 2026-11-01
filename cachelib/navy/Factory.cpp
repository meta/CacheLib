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

#include "cachelib/navy/Factory.h"

#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/logging/xlog.h>

#include <stdexcept>

#include "cachelib/navy/admission_policy/DynamicRandomAP.h"
#include "cachelib/navy/admission_policy/RejectRandomAP.h"
#include "cachelib/navy/bighash/BigHash.h"
#include "cachelib/navy/block_cache/BlockCache.h"
#include "cachelib/navy/block_cache/FifoPolicy.h"
#include "cachelib/navy/block_cache/LruPolicy.h"
#include "cachelib/navy/common/ChecksumOffload.h"
#include "cachelib/navy/common/Device.h"
#include "cachelib/navy/driver/Driver.h"

/* O_DIRECT not available on Mac OS */
#ifndef O_DIRECT
#define O_DIRECT 0
#endif

namespace facebook::cachelib::navy {
namespace {
// Returns true if checksum offload may be enabled. Logs and returns false
// (callers fall back to software checksums) when built without DTO support
// or when the runtime DSA-vs-CPU checksum parity self-check fails. The
// self-check protects against a mixed deployment where accelerator-computed
// checksums would not verify against CPU-computed ones.
bool verifyChecksumOffload(folly::StringPiece engine) {
  if (!checksumOffloadSupported()) {
    XLOGF(WARN,
          "{}: checksum offload requested, but this binary was built without "
          "DTO support. Falling back to software checksums.",
          engine);
    return false;
  }
  if (!checksumOffloadSelfCheck()) {
    XLOGF(ERR,
          "{}: DSA checksum offload self-check failed (no usable DSA work "
          "queue - descriptors did not complete on the device - or the "
          "accelerator checksum does not match the CPU checksum). Falling "
          "back to software checksums.",
          engine);
    return false;
  }
  return true;
}

class BlockCacheProtoImpl final : public BlockCacheProto {
 public:
  BlockCacheProtoImpl() = default;
  ~BlockCacheProtoImpl() override = default;

  void setLayout(uint64_t baseOffset,
                 uint64_t size,
                 uint32_t regionSize) override {
    if (size <= 0 || regionSize <= 0) {
      throw std::invalid_argument(fmt::format(
          "Invalid layout. size: {}, regionSize: {}.", size, regionSize));
    }
    config_.cacheBaseOffset = baseOffset;
    config_.cacheSize = size;
    config_.regionSize = regionSize;
  }

  void setChecksum(bool enable) override { config_.checksum = enable; }

  void setChecksumOffload(bool enable, uint32_t minSize) override {
    config_.checksumOffload = enable;
    config_.checksumOffloadMinSize = minSize;
  }

  void setChecksumOffloadReadMinSize(uint32_t minSize) override {
    config_.checksumOffloadReadMinSize = minSize;
  }

  void setChecksumOffloadCacheControl(bool enable) override {
    config_.checksumOffloadCacheControl = enable;
  }

  void setChecksumOffloadBatchReclaim(bool enable) override {
    config_.checksumOffloadBatchReclaim = enable;
  }

  void setChecksumOffloadBatch(bool enable) override {
    config_.checksumOffloadBatch = enable;
  }

  void setChecksumDeferSmall(bool enable) override {
    config_.checksumDeferSmall = enable;
  }

  void setSkipInMemValueVerify(bool enable) override {
    config_.skipInMemValueVerify = enable;
  }

  void setLruEvictionPolicy() override {
    if (!(config_.cacheSize > 0 && config_.regionSize > 0)) {
      throw std::logic_error("layout is not set");
    }
    auto numRegions = config_.getNumRegions();
    if (config_.evictionPolicy) {
      throw std::invalid_argument("There's already an eviction policy set");
    }
    config_.evictionPolicy = std::make_unique<LruPolicy>(numRegions);
  }

  void setFifoEvictionPolicy() override {
    if (config_.evictionPolicy) {
      throw std::invalid_argument("There's already an eviction policy set");
    }
    config_.evictionPolicy = std::make_unique<FifoPolicy>();
  }

  void setSegmentedFifoEvictionPolicy(
      std::vector<unsigned int> segmentRatio) override {
    if (config_.evictionPolicy) {
      throw std::invalid_argument("There's already an eviction policy set");
    }
    config_.evictionPolicy =
        std::make_unique<SegmentedFifoPolicy>(std::move(segmentRatio));
  }

  void setNumAllocatorsPerPriority(
      std::vector<uint32_t> allocatorsPerPriority) override {
    config_.allocatorsPerPriority = std::move(allocatorsPerPriority);
  }

  void setReadBufferSize(uint32_t size) override {
    config_.readBufferSize = size;
  }

  void setCleanRegionsPool(uint32_t cleanRegions,
                           uint32_t cleanRegionThreads) override {
    config_.cleanRegionsPool = cleanRegions;
    config_.cleanRegionThreads = cleanRegionThreads;
  }

  void setReinsertionConfig(
      const BlockCacheReinsertionConfig& reinsertionConfig) override {
    config_.reinsertionConfig = reinsertionConfig;
  }

  void setDevice(Device* device) { config_.device = device; }

  void setPersistParams(const NavyPersistParams& params) {
    config_.persistParams = params;
  }

  void setName(const std::string& name) { config_.name = name; }

  void setNumInMemBuffers(uint32_t numInMemBuffers) override {
    config_.numInMemBuffers = numInMemBuffers;
  }

  void setItemDestructorEnabled(bool itemDestructorEnabled) override {
    config_.itemDestructorEnabled = itemDestructorEnabled;
  }

  void setStackSize(uint32_t stackSize) override {
    config_.stackSize = stackSize;
  }

  void setPreciseRemove(bool preciseRemove) override {
    config_.preciseRemove = preciseRemove;
  }

  void setRegionManagerFlushAsync(bool asyn) override {
    config_.regionManagerFlushAsync = asyn;
  }

  void setRecoverEvictionPolicy(bool enable) override {
    config_.recoverEvictionPolicy = enable;
  }

  void setDirectFlush(bool enable) override { config_.directFlush = enable; }
  void setFlushCopyOffload(bool enable) override {
    config_.flushCopyOffload = enable;
  }

  void setUseCombinedEntryBlock(bool useCombinedEntryBlock) override {
    config_.useCombinedEntryBlock = useCombinedEntryBlock;
  }

  void setIndexConfig(const BlockCacheIndexConfig& indexConfig) override {
    config_.indexConfig = indexConfig;
  }

  void setLegacyEventTracker(
      const std::optional<std::reference_wrapper<LegacyEventTracker>>&
          legacyEventTracker) override {
    config_.legacyEventTracker = legacyEventTracker;
  }

  std::unique_ptr<Engine> create(ExpiredCheck checkExpired,
                                 DestructorCallback cb) && {
    config_.checkExpired = std::move(checkExpired);
    config_.destructorCb = std::move(cb);
    if (config_.checksumOffload && !verifyChecksumOffload("BlockCache")) {
      config_.checksumOffload = false;
    }
    return std::make_unique<BlockCache>(std::move(config_));
  }

 private:
  BlockCache::Config config_;
};

class BigHashProtoImpl final : public BigHashProto {
 public:
  BigHashProtoImpl() = default;
  ~BigHashProtoImpl() override = default;

  void setLayout(uint64_t baseOffset,
                 uint64_t size,
                 uint32_t bucketSize) override {
    config_.cacheBaseOffset = baseOffset;
    config_.cacheSize = size;
    config_.bucketSize = bucketSize;
  }

  // BigHash uses bloom filters (BF) to reduce number of IO. We want to maintain
  // BF for every bucket, because we want to rebuild it on every remove to keep
  // its filtering properties.
  void setBloomFilter(uint32_t numHashes, uint32_t hashTableBitSize) override {
    // Want to make @setLayout and Bloom filter setup independent.
    bloomFilterEnabled_ = true;
    numHashes_ = numHashes;
    hashTableBitSize_ = hashTableBitSize;
  }

  void setNumMutexesPower(uint8_t numMutexesPower) override {
    config_.numMutexesPower = numMutexesPower;
  }

  void setChecksumOffload(bool enable) override {
    config_.checksumOffload = enable;
  }

  void setDevice(Device* device) { config_.device = device; }

  void setDestructorCb(DestructorCallback cb) {
    config_.destructorCb = std::move(cb);
  }

  std::unique_ptr<Engine> create(ExpiredCheck checkExpired) && {
    config_.checkExpired = std::move(checkExpired);
    if (config_.checksumOffload && !verifyChecksumOffload("BigHash")) {
      config_.checksumOffload = false;
    }
    if (bloomFilterEnabled_) {
      if (config_.bucketSize == 0) {
        throw std::invalid_argument{"invalid bucket size"};
      }
      config_.bloomFilter = std::make_unique<BloomFilter>(
          config_.numBuckets(), numHashes_, hashTableBitSize_);
    }
    return std::make_unique<BigHash>(std::move(config_));
  }

 private:
  BigHash::Config config_;
  bool bloomFilterEnabled_{false};
  uint32_t numHashes_{};
  uint32_t hashTableBitSize_{};
};

class EnginePairProtoImpl final : public EnginePairProto {
 public:
  EnginePairProtoImpl() = default;
  ~EnginePairProtoImpl() override = default;

  void setBigHash(std::unique_ptr<BigHashProto> proto,
                  uint32_t smallItemMaxSize) override {
    bigHashProto_ = std::move(proto);
    smallItemMaxSize_ = smallItemMaxSize;
  }

  void setBlockCache(std::unique_ptr<BlockCacheProto> proto) override {
    blockCacheProto_ = std::move(proto);
  }

  EnginePair create(Device* device,
                    const NavyPersistParams& persistParams,
                    const ExpiredCheck& checkExpired,
                    const DestructorCallback& destructorCb,
                    JobScheduler& scheduler) {
    std::unique_ptr<Engine> bh;

    if (bigHashProto_) {
      auto bhProto = dynamic_cast<BigHashProtoImpl*>(bigHashProto_.get());
      if (bhProto != nullptr) {
        bhProto->setDevice(device);
        bhProto->setDestructorCb(destructorCb);
        bh = std::move(*bhProto).create(checkExpired);
      }
    }

    std::unique_ptr<Engine> bc;
    if (blockCacheProto_) {
      auto bcProto = dynamic_cast<BlockCacheProtoImpl*>(blockCacheProto_.get());
      if (bcProto != nullptr) {
        bcProto->setDevice(device);
        bcProto->setPersistParams(persistParams);
        bcProto->setName(name_);
        bc = std::move(*bcProto).create(checkExpired, destructorCb);
      }
    }

    return EnginePair{std::move(bh), std::move(bc), smallItemMaxSize_,
                      &scheduler, std::move(name_)};
  }

  void setName(const std::string& name) override { name_ = name; }

 private:
  std::unique_ptr<BigHashProto> bigHashProto_;
  std::unique_ptr<BlockCacheProto> blockCacheProto_;
  uint32_t smallItemMaxSize_;
  std::string name_;
};

class DriverProtoImpl final : public CacheProto {
 public:
  DriverProtoImpl() = default;
  ~DriverProtoImpl() override = default;

  void setMaxConcurrentInserts(uint32_t limit) override {
    config_.maxConcurrentInserts = limit;
  }

  void setMaxParcelMemory(uint64_t limit) override {
    config_.maxParcelMemory = limit;
  }

  void setUseEstimatedWriteSize(bool useEstimatedWriteSize) override {
    config_.useEstimatedWriteSize = useEstimatedWriteSize;
  }

  void setDevice(std::unique_ptr<Device> device) override {
    config_.device = std::move(device);
  }

  void setPersistParams(const NavyPersistParams& params) override {
    config_.persistParams = params;
  }

  void setMetadataSize(size_t size) override { config_.metadataSize = size; }

  void setMetadataStagingSize(size_t size) override {
    config_.metadataStagingSize = size;
  }

  void setMaxKeySize(uint32_t keySize) override {
    config_.maxKeySize = keySize;
  }

  void setExpiredCheck(ExpiredCheck checkExpired) override {
    checkExpired_ = std::move(checkExpired);
  }

  void setDestructorCallback(DestructorCallback cb) override {
    destructorCb_ = std::move(cb);
  }

  void addEnginePair(std::unique_ptr<EnginePairProto> proto) override {
    enginePairsProto_.push_back(std::move(proto));
  }

  void setEnginesSelector(NavyConfig::EnginesSelector selector) override {
    config_.selector = std::move(selector);
  }

  void setRejectRandomAdmissionPolicy(const RandomAPConfig& config) override {
    RejectRandomAP::Config apConfig;
    apConfig.probability = config.getAdmProbability();
    apConfig.seed = folly::Random::rand32();
    config_.admissionPolicy =
        std::make_unique<RejectRandomAP>(std::move(apConfig));
  }

  void setDynamicRandomAdmissionPolicy(
      const DynamicRandomAPConfig& config) override {
    DynamicRandomAP::Config apConfig;
    apConfig.targetRate = config.getAdmWriteRate();
    apConfig.fnBytesWritten = [device = config_.device.get()]() {
      return device->getBytesWritten();
    };
    apConfig.seed = folly::Random::rand32();
    apConfig.deterministicKeyHashSuffixLength = config.getAdmSuffixLength();
    uint32_t itemBaseSize = config.getAdmProbBaseSize();
    if (itemBaseSize > 0) {
      apConfig.baseSize = itemBaseSize;
    }
    uint64_t maxRate = config.getMaxWriteRate();
    if (maxRate > 0) {
      apConfig.maxRate = maxRate;
    }
    double probFactorLowerBound = config.getProbFactorLowerBound();
    double probFactorUpperBound = config.getProbFactorUpperBound();
    if (probFactorLowerBound > 0 && probFactorUpperBound > 0) {
      apConfig.probFactorLowerBound = probFactorLowerBound;
      apConfig.probFactorUpperBound = probFactorUpperBound;
    }
    apConfig.enableLogging = config.getEnableLogging();
    auto fnBypass = config.getFnBypass();
    if (fnBypass) {
      apConfig.fnBypass = std::move(fnBypass);
    }

    config_.admissionPolicy =
        std::make_unique<DynamicRandomAP>(std::move(apConfig));
  }

  void setJobScheduler(std::unique_ptr<JobScheduler> ex) override {
    config_.scheduler = std::move(ex);
  }

  std::unique_ptr<AbstractCache> create() && {
    if (config_.scheduler == nullptr) {
      throw std::invalid_argument("scheduler is not set");
    }

    for (auto& p : enginePairsProto_) {
      config_.enginePairs.push_back(
          dynamic_cast<EnginePairProtoImpl*>(p.get())->create(
              config_.device.get(), config_.persistParams, checkExpired_,
              destructorCb_, *config_.scheduler));
    }

    return std::make_unique<Driver>(std::move(config_));
  }

 private:
  ExpiredCheck checkExpired_;
  DestructorCallback destructorCb_;
  std::vector<std::unique_ptr<EnginePairProto>> enginePairsProto_;
  Driver::Config config_;
};
} // namespace

std::unique_ptr<BlockCacheProto> createBlockCacheProto() {
  return std::make_unique<BlockCacheProtoImpl>();
}

std::unique_ptr<BigHashProto> createBigHashProto() {
  return std::make_unique<BigHashProtoImpl>();
}

std::unique_ptr<EnginePairProto> createEnginePairProto() {
  return std::make_unique<EnginePairProtoImpl>();
}

std::unique_ptr<CacheProto> createCacheProto() {
  return std::make_unique<DriverProtoImpl>();
}

std::unique_ptr<AbstractCache> createCache(std::unique_ptr<CacheProto> proto) {
  return std::move(dynamic_cast<DriverProtoImpl&>(*proto)).create();
}
} // namespace facebook::cachelib::navy
