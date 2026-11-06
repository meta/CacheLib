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

#include <sstream>
#include <stdexcept>

#include "cachelib/allocator/nvmcache/BlockCacheReinsertionPolicy.h"
#include "cachelib/allocator/nvmcache/NavyConfig.h"
#include "cachelib/navy/block_cache/SparseMapIndex.h"

namespace facebook {
namespace cachelib {
namespace tests {
using NavyConfig = navy::NavyConfig;
namespace {
// AP settings
const std::string admissionPolicy = "dynamic_random";
const uint64_t admissionWriteRate = 100;
const uint64_t maxWriteRate = 160;
const size_t admissionSuffixLen = 1;
const uint64_t admissionProbBaseSize = 1024;

// device settings
const uint64_t blockSize = 1024;
const std::string fileName = "test";
const std::vector<std::string> raidPaths = {"test1", "test2"};
const std::vector<std::string> raidPathsInvalid = {"test1"};
const uint64_t deviceMetadataSize = 1024 * 1024 * 1024;
const uint64_t fileSize = 10 * 1024 * 1024;
const bool truncateFile = false;
const uint32_t deviceMaxWriteSize = 4 * 1024 * 1024;
const navy::IoEngine ioEngine = navy::IoEngine::IoUring;
const unsigned int qDepth = 64;

// BlockCache settings
const uint32_t blockCacheRegionSize = 16 * 1024 * 1024;
const uint8_t blockCacheReinsertionHitsThreshold = 111;
const uint32_t blockCacheCleanRegions = 4;
const bool blockCacheDataChecksum = true;
const std::vector<unsigned int> blockCacheSegmentedFifoSegmentRatio = {111, 222,
                                                                       333};

class DummyReinsertionPolicy : public BlockCacheReinsertionPolicy {
 public:
  bool shouldReinsert(folly::StringPiece /* key */,
                      folly::StringPiece /* value */) override {
    return true;
  }

  void getCounters(const util::CounterVisitor& /* visitor */) const override {}
};

// BigCache settings
const unsigned int bigHashSizePct = 50;
const uint32_t bigHashBucketSize = 1024;
const uint64_t bigHashBucketBfSize = 4;
const uint64_t bigHashSmallItemMaxSize = 512;

const uint32_t maxConcurrentInserts = 50000;
const uint64_t maxParcelMemoryMB = 512;

// Job scheduler settings
const unsigned int readerThreads = 40;
const unsigned int writerThreads = 40;
const uint64_t navyReqOrderingShards = 30;

void setAdmissionPolicyTestSettings(NavyConfig& config) {
  config.enableDynamicRandomAdmPolicy()
      .setAdmWriteRate(admissionWriteRate)
      .setMaxWriteRate(maxWriteRate)
      .setAdmSuffixLength(admissionSuffixLen)
      .setAdmProbBaseSize(admissionProbBaseSize)
      .setProbFactorRange(0.001, 2.0);
}

void setDeviceTestSettings(NavyConfig& config) {
  config.setBlockSize(blockSize);
  config.setRaidFiles(raidPaths, fileSize, truncateFile);
  config.setDeviceMetadataSize(deviceMetadataSize);
  config.setDeviceMaxWriteSize(deviceMaxWriteSize);
  config.enableAsyncIo(qDepth, ioEngine == navy::IoEngine::IoUring);
}

void setBlockCacheTestSettings(NavyConfig& config) {
  config.blockCache()
      .enableSegmentedFifo(blockCacheSegmentedFifoSegmentRatio)
      .enableHitsBasedReinsertion(blockCacheReinsertionHitsThreshold)
      .setCleanRegions(blockCacheCleanRegions)
      .setRegionSize(blockCacheRegionSize)
      .setDataChecksum(blockCacheDataChecksum);
}

void setBigHashTestSettings(NavyConfig& config) {
  config.bigHash()
      .setSizePctAndMaxItemSize(bigHashSizePct, bigHashSmallItemMaxSize)
      .setBucketSize(bigHashBucketSize)
      .setBucketBfSize(bigHashBucketBfSize);
}

void setJobSchedulerTestSettings(NavyConfig& config) {
  config.setReaderAndWriterThreads(readerThreads, writerThreads);
  config.setNavyReqOrderingShards(navyReqOrderingShards);
}

void setTestNavyConfig(NavyConfig& config) {
  setAdmissionPolicyTestSettings(config);
  setDeviceTestSettings(config);
  setBlockCacheTestSettings(config);
  setBigHashTestSettings(config);
  setJobSchedulerTestSettings(config);
  config.setMaxConcurrentInserts(maxConcurrentInserts);
  config.setMaxParcelMemoryMB(maxParcelMemoryMB);
}
} // namespace
TEST(NavyConfigTest, DefaultVal) {
  NavyConfig config{};
  EXPECT_EQ(config.getChecksumType(), navy::ChecksumType::Crc32);

  const auto dynamicRandomConfig = config.dynamicRandomAdmPolicy();
  EXPECT_EQ(config.getAdmissionPolicy(), "");
  EXPECT_EQ(config.randomAdmPolicy().getAdmProbability(), 0);
  EXPECT_EQ(dynamicRandomConfig.getAdmSuffixLength(), 0);
  EXPECT_EQ(dynamicRandomConfig.getProbFactorLowerBound(), 0);
  EXPECT_EQ(dynamicRandomConfig.getProbFactorUpperBound(), 0);
  EXPECT_EQ(dynamicRandomConfig.getAdmWriteRate(), 0);

  const auto& blockCacheConfig = config.blockCache();
  EXPECT_EQ(blockCacheConfig.isLruEnabled(), true);
  EXPECT_EQ(blockCacheConfig.getRegionSize(), 16 * 1024 * 1024);
  EXPECT_EQ(blockCacheConfig.getCleanRegions(), 1);
  EXPECT_EQ(blockCacheConfig.getCleanRegionThreads(), 1);
  EXPECT_TRUE(blockCacheConfig.getSFifoSegmentRatio().empty());
  EXPECT_EQ(blockCacheConfig.getDataChecksum(), true);
  EXPECT_EQ(blockCacheConfig.getNumInMemBuffers(), 2);

  const auto& bigHashConfig = config.bigHash();
  EXPECT_EQ(bigHashConfig.getBucketSize(), 4096);
  EXPECT_EQ(bigHashConfig.getBucketBfSize(), 8);
  EXPECT_EQ(bigHashConfig.getSmallItemMaxSize(), 0);

  EXPECT_EQ(config.getMaxConcurrentInserts(), 1'000'000);
  EXPECT_EQ(config.getMaxParcelMemoryMB(), 256);

  EXPECT_EQ(config.getReaderThreads(), 32);
  EXPECT_EQ(config.getWriterThreads(), 32);
  EXPECT_EQ(config.getNavyReqOrderingShards(), 20);

  EXPECT_EQ(config.getBlockSize(), 4096);
  EXPECT_EQ(config.getTruncateFile(), false);
  EXPECT_EQ(config.getDeviceMetadataSize(), 0);
  EXPECT_EQ(config.getFileSize(), 0);
  EXPECT_EQ(config.getDeviceMaxWriteSize(), 0);

  EXPECT_EQ(config.usesSimpleFile(), false);
  EXPECT_EQ(config.usesRaidFiles(), false);
}

TEST(NavyConfigTest, Serialization) {
  NavyConfig config{};
  setTestNavyConfig(config);
  std::map<std::string, std::string> configMap = config.serialize();

  auto expectedConfigMap = std::map<std::string, std::string>();
  expectedConfigMap["navyConfig::checksumType"] = "crc32";
  expectedConfigMap["navyConfig::admissionPolicy"] = "dynamic_random";
  expectedConfigMap["navyConfig::admissionProbability"] = "0";
  expectedConfigMap["navyConfig::admissionWriteRate"] = "100";
  expectedConfigMap["navyConfig::maxWriteRate"] = "160";
  expectedConfigMap["navyConfig::admissionSuffixLen"] = "1";
  expectedConfigMap["navyConfig::admissionProbBaseSize"] = "1024";
  expectedConfigMap["navyConfig::admissionProbFactorLowerBound"] = "0.001";
  expectedConfigMap["navyConfig::admissionProbFactorUpperBound"] = "2";

  expectedConfigMap["navyConfig::blockSize"] = "1024";
  expectedConfigMap["navyConfig::fileName"] = "";
  expectedConfigMap["navyConfig::raidPaths"] = "test1,test2";
  expectedConfigMap["navyConfig::deviceMetadataSize"] = "1073741824";
  expectedConfigMap["navyConfig::maxKeySize"] = "255";
  expectedConfigMap["navyConfig::fileSize"] = "10485760";
  expectedConfigMap["navyConfig::truncateFile"] = "false";
  expectedConfigMap["navyConfig::deviceMaxWriteSize"] = "4194304";
  expectedConfigMap["navyConfig::metadataStagingSize"] = "0";
  expectedConfigMap["navyConfig::ioEngine"] = "io_uring";
  expectedConfigMap["navyConfig::QDepth"] = "64";
  expectedConfigMap["navyConfig::enableFDP"] = "0";

  expectedConfigMap["navyConfig::blockCacheLru"] = "false";
  expectedConfigMap["navyConfig::blockCacheRegionSize"] = "16777216";
  expectedConfigMap["navyConfig::blockCacheCleanRegions"] = "4";
  expectedConfigMap["navyConfig::blockCacheCleanRegionThreads"] = "1";
  expectedConfigMap["navyConfig::blockCacheReinsertionHitsThreshold"] = "111";
  expectedConfigMap["navyConfig::blockCacheReinsertionPctThreshold"] = "0";
  expectedConfigMap["navyConfig::blockCacheNumInMemBuffers"] = "8";
  expectedConfigMap["navyConfig::blockCacheDataChecksum"] = "true";
  expectedConfigMap["navyConfig::blockCacheSegmentedFifoSegmentRatio"] =
      "111,222,333";

  expectedConfigMap["navyConfig::bigHashSizePct"] = "50";
  expectedConfigMap["navyConfig::bigHashBucketSize"] = "1024";
  expectedConfigMap["navyConfig::bigHashBucketBfSize"] = "4";
  expectedConfigMap["navyConfig::bigHashSmallItemMaxSize"] = "512";

  expectedConfigMap["navyConfig::maxConcurrentInserts"] = "50000";
  expectedConfigMap["navyConfig::maxParcelMemoryMB"] = "512";

  expectedConfigMap["navyConfig::readerThreads"] = "40";
  expectedConfigMap["navyConfig::writerThreads"] = "40";
  expectedConfigMap["navyConfig::navyReqOrderingShards"] = "30";
  expectedConfigMap["navyConfig::maxNumReads"] = "0";
  expectedConfigMap["navyConfig::maxNumWrites"] = "0";
  expectedConfigMap["navyConfig::stackSize"] = "0";

  EXPECT_EQ(configMap, expectedConfigMap);
}

TEST(NavyConfigTest, AdmissionPolicy) {
  // set random admission policy
  NavyConfig config{};
  EXPECT_THROW(config.enableRandomAdmPolicy().setAdmProbability(2),
               std::invalid_argument);
  config = NavyConfig{};
  EXPECT_NO_THROW(config.enableRandomAdmPolicy().setAdmProbability(0.5));
  EXPECT_EQ(config.getAdmissionPolicy(), NavyConfig::kAdmPolicyRandom);
  EXPECT_EQ(config.randomAdmPolicy().getAdmProbability(), 0.5);
  // cannot set dynamic_random parameters
  EXPECT_THROW(config.enableDynamicRandomAdmPolicy(), std::invalid_argument);

  // set dynamic_random policy
  config = NavyConfig{};
  EXPECT_NO_THROW(config.enableDynamicRandomAdmPolicy()
                      .setAdmWriteRate(admissionWriteRate)
                      .setMaxWriteRate(maxWriteRate)
                      .setAdmSuffixLength(admissionSuffixLen)
                      .setAdmProbBaseSize(admissionProbBaseSize)
                      .setProbFactorRange(0.1, 10));
  auto dynamicRandomConfig = config.dynamicRandomAdmPolicy();
  EXPECT_EQ(config.getAdmissionPolicy(), NavyConfig::kAdmPolicyDynamicRandom);
  EXPECT_EQ(dynamicRandomConfig.getAdmWriteRate(), admissionWriteRate);
  EXPECT_EQ(dynamicRandomConfig.getMaxWriteRate(), maxWriteRate);
  EXPECT_EQ(dynamicRandomConfig.getAdmSuffixLength(), admissionSuffixLen);
  EXPECT_EQ(dynamicRandomConfig.getAdmProbBaseSize(), admissionProbBaseSize);
  EXPECT_EQ(dynamicRandomConfig.getProbFactorLowerBound(), 0.1);
  EXPECT_EQ(dynamicRandomConfig.getProbFactorUpperBound(), 10);
  // cannot set random parameters
  EXPECT_THROW(config.enableRandomAdmPolicy(), std::invalid_argument);
}

TEST(NavyConfigTest, Device) {
  {
    NavyConfig config{};
    config.setBlockSize(blockSize);
    config.setDeviceMetadataSize(deviceMetadataSize);
    EXPECT_EQ(config.getBlockSize(), blockSize);
    EXPECT_EQ(config.getDeviceMetadataSize(), deviceMetadataSize);

    // set simple file
    config.setSimpleFile(fileName, fileSize, truncateFile);
    EXPECT_EQ(config.getFileName(), fileName);
    EXPECT_EQ(config.getFileSize(), fileSize);
    EXPECT_EQ(config.getTruncateFile(), truncateFile);
    EXPECT_THROW(config.setRaidFiles(raidPaths, fileSize, truncateFile),
                 std::invalid_argument);
  }
  {
    // set RAID files
    NavyConfig config{};
    EXPECT_THROW(config.setRaidFiles(raidPathsInvalid, fileSize, truncateFile),
                 std::invalid_argument);
    config.setRaidFiles(raidPaths, fileSize, truncateFile);
    EXPECT_EQ(config.getRaidPaths(), raidPaths);
    EXPECT_EQ(config.getFileSize(), fileSize);
    EXPECT_EQ(config.getTruncateFile(), truncateFile);
    EXPECT_THROW(config.setSimpleFile(fileName, fileSize, truncateFile),
                 std::invalid_argument);
  }
  {
    // set io engines
    NavyConfig config{};
    EXPECT_EQ(config.getIoEngine(), navy::IoEngine::Sync);
    EXPECT_EQ(config.getQDepth(), 0);
    config.enableAsyncIo(1, false);
    EXPECT_EQ(config.getIoEngine(), navy::IoEngine::LibAio);
    EXPECT_EQ(config.getQDepth(), 1);
    config.enableAsyncIo(64, true);
    EXPECT_EQ(config.getIoEngine(), navy::IoEngine::IoUring);
    EXPECT_EQ(config.getQDepth(), 64);
  }
  {
    // set async io via job scheduler settings
    NavyConfig config{};
    config.setReaderAndWriterThreads(4, 4);
    EXPECT_EQ(config.getIoEngine(), navy::IoEngine::Sync);
    EXPECT_EQ(config.getQDepth(), 0);
    config.setReaderAndWriterThreads(4, 4, 4, 4);
    EXPECT_EQ(config.getIoEngine(), navy::IoEngine::LibAio);
    EXPECT_EQ(config.getQDepth(), 1);
    config.enableAsyncIo(64, true);
    EXPECT_EQ(config.getIoEngine(), navy::IoEngine::IoUring);
    EXPECT_EQ(config.getQDepth(), 64);
  }
}

TEST(NavyConfigTest, BlockCache) {
  NavyConfig config{};
  // test general settings
  config.blockCache()
      .setRegionSize(blockCacheRegionSize)
      .setCleanRegions(blockCacheCleanRegions)
      .setDataChecksum(blockCacheDataChecksum);
  EXPECT_EQ(config.blockCache().getRegionSize(), blockCacheRegionSize);
  EXPECT_EQ(config.blockCache().getCleanRegions(), blockCacheCleanRegions);
  EXPECT_EQ(config.blockCache().getNumInMemBuffers(),
            blockCacheCleanRegions * 2);
  EXPECT_EQ(config.blockCache().getDataChecksum(), blockCacheDataChecksum);

  // test FIFO eviction policy
  config.blockCache().enableFifo();
  EXPECT_EQ(config.blockCache().isLruEnabled(), false);
  EXPECT_TRUE(config.blockCache().getSFifoSegmentRatio().empty());
  // test segmented FIFO eviction policy
  config.blockCache().enableSegmentedFifo(blockCacheSegmentedFifoSegmentRatio);
  EXPECT_EQ(config.blockCache().isLruEnabled(), false);
  EXPECT_EQ(config.blockCache().getSFifoSegmentRatio(),
            blockCacheSegmentedFifoSegmentRatio);

  auto customPolicy = std::make_shared<DummyReinsertionPolicy>();

  navy::SparseMapIndex index;

  // test cannot enable both hits-based and probability-based reinsertion policy
  config = NavyConfig{};
  EXPECT_THROW(
      config.blockCache()
          .enableHitsBasedReinsertion(blockCacheReinsertionHitsThreshold)
          .enablePctBasedReinsertion(50),
      std::invalid_argument);
  EXPECT_THROW(config.blockCache().enableCustomReinsertion(customPolicy),
               std::invalid_argument);
  EXPECT_EQ(config.blockCache().getReinsertionConfig().getHitsThreshold(),
            blockCacheReinsertionHitsThreshold);
  EXPECT_EQ(config.blockCache().getReinsertionConfig().getPctThreshold(), 0);
  EXPECT_EQ(config.blockCache().getReinsertionConfig().getCustomPolicy(index),
            nullptr);

  config = NavyConfig{};
  EXPECT_THROW(
      config.blockCache()
          .enablePctBasedReinsertion(50)
          .enableHitsBasedReinsertion(blockCacheReinsertionHitsThreshold),
      std::invalid_argument);
  EXPECT_THROW(config.blockCache().enableCustomReinsertion(customPolicy),
               std::invalid_argument);
  EXPECT_EQ(config.blockCache().getReinsertionConfig().getPctThreshold(), 50);
  EXPECT_EQ(config.blockCache().getReinsertionConfig().getHitsThreshold(), 0);
  EXPECT_EQ(config.blockCache().getReinsertionConfig().getCustomPolicy(index),
            nullptr);

  // test invalid input for percentage based reinsertion policy
  config = NavyConfig{};
  EXPECT_THROW(config.blockCache().enablePctBasedReinsertion(200),
               std::invalid_argument);

  config = NavyConfig{};
  EXPECT_THROW(config.blockCache()
                   .enableCustomReinsertion(customPolicy)
                   .enablePctBasedReinsertion(50),
               std::invalid_argument);
  EXPECT_THROW(config.blockCache().enableHitsBasedReinsertion(
                   blockCacheReinsertionHitsThreshold),
               std::invalid_argument);
  EXPECT_EQ(config.blockCache().getReinsertionConfig().getPctThreshold(), 0);
  EXPECT_EQ(config.blockCache().getReinsertionConfig().getHitsThreshold(), 0);
  EXPECT_EQ(config.blockCache().getReinsertionConfig().getCustomPolicy(index),
            customPolicy);

  config = NavyConfig{};
  // Default is sparse map index with the default config parameters that are
  // previously hard coded with SparseMapIndex
  EXPECT_FALSE(config.blockCache().getIndexConfig().isFixedSizeIndexEnabled());
  EXPECT_EQ(config.blockCache().getIndexConfig().getNumSparseMapBuckets(),
            navy::BlockCacheIndexConfig::kDefaultNumSparseMapBuckets);
  EXPECT_EQ(config.blockCache().getIndexConfig().getNumBucketsPerMutex(),
            navy::BlockCacheIndexConfig::kDefaultNumBucketsPerMutex);

  // Enable FixedSizeIndex with invalid argument
  EXPECT_THROW(config.blockCache().enableFixedSizeIndex(6, 0, 1024),
               std::invalid_argument);
  EXPECT_THROW(config.blockCache().enableFixedSizeIndex(6, 100, 3000),
               std::invalid_argument);

  // Enable FixedSizeIndex with the valid arguments
  config.blockCache().enableFixedSizeIndex(8, 27, 64 * 1024);
  EXPECT_TRUE(config.blockCache().getIndexConfig().isFixedSizeIndexEnabled());
  EXPECT_EQ(config.blockCache().getIndexConfig().getNumChunks(), 8);
  EXPECT_EQ(config.blockCache().getIndexConfig().getNumBucketsPerChunkPower(),
            27);
  EXPECT_EQ(config.blockCache().getIndexConfig().getNumBucketsPerMutex(),
            64 * 1024);

  // Enable SparseMap with the different parameters
  config.blockCache().enableSparseMapIndex(32 * 1024, 128);
  EXPECT_FALSE(config.blockCache().getIndexConfig().isFixedSizeIndexEnabled());
  EXPECT_EQ(config.blockCache().getIndexConfig().getNumSparseMapBuckets(),
            32 * 1024);
  EXPECT_EQ(config.blockCache().getIndexConfig().getNumBucketsPerMutex(), 128);
  EXPECT_EQ(config.blockCache().getIndexConfig().isTrackItemHistoryEnabled(),
            false);
  // Enable SparseMap with the alternative parameters
  config.blockCache().enableSparseMapIndex(32 * 1024, 128, false);
  EXPECT_FALSE(config.blockCache().getIndexConfig().isFixedSizeIndexEnabled());
  EXPECT_EQ(config.blockCache().getIndexConfig().getNumSparseMapBuckets(),
            32 * 1024);
  EXPECT_EQ(config.blockCache().getIndexConfig().getNumBucketsPerMutex(), 128);
  EXPECT_EQ(config.blockCache().getIndexConfig().isTrackItemHistoryEnabled(),
            false);
  // Enable SparseMap with the alternative parameters
  config.blockCache().enableSparseMapIndex(32 * 1024, 128, true);
  EXPECT_FALSE(config.blockCache().getIndexConfig().isFixedSizeIndexEnabled());
  EXPECT_EQ(config.blockCache().getIndexConfig().getNumSparseMapBuckets(),
            32 * 1024);
  EXPECT_EQ(config.blockCache().getIndexConfig().getNumBucketsPerMutex(), 128);
  EXPECT_EQ(config.blockCache().getIndexConfig().isTrackItemHistoryEnabled(),
            true);

  // Try with invalid arguments
  EXPECT_THROW(config.blockCache().enableSparseMapIndex(0, 128),
               std::invalid_argument);
  EXPECT_THROW(config.blockCache().enableSparseMapIndex(32 * 1024, 100, false),
               std::invalid_argument);
  EXPECT_THROW(
      config.blockCache().enableSparseMapIndex(32 * 1024, 64 * 1024, true),
      std::invalid_argument);
}

TEST(NavyConfigTest, BigHash) {
  NavyConfig config{};
  EXPECT_THROW(
      config.bigHash().setSizePctAndMaxItemSize(200, bigHashSmallItemMaxSize),
      std::invalid_argument);
  config.bigHash()
      .setSizePctAndMaxItemSize(bigHashSizePct, bigHashSmallItemMaxSize)
      .setBucketSize(bigHashBucketSize)
      .setBucketBfSize(bigHashBucketBfSize);

  EXPECT_EQ(config.bigHash().getSizePct(), bigHashSizePct);
  EXPECT_EQ(config.bigHash().getBucketSize(), bigHashBucketSize);
  EXPECT_EQ(config.bigHash().getBucketBfSize(), bigHashBucketBfSize);
  EXPECT_EQ(config.bigHash().getSmallItemMaxSize(), bigHashSmallItemMaxSize);
}

TEST(NavyConfigTest, JobScheduler) {
  NavyConfig config{};
  config.setReaderAndWriterThreads(readerThreads, writerThreads);
  ASSERT_THROW(config.setNavyReqOrderingShards(0), std::invalid_argument);
  config.setNavyReqOrderingShards(navyReqOrderingShards);
  EXPECT_EQ(config.getReaderThreads(), readerThreads);
  EXPECT_EQ(config.getWriterThreads(), writerThreads);
  EXPECT_EQ(config.getNavyReqOrderingShards(), navyReqOrderingShards);
}

TEST(NavyConfigTest, ChecksumType) {
  NavyConfig config;
  config.setChecksumType(navy::ChecksumType::Crc32c);
  EXPECT_EQ(config.getChecksumType(), navy::ChecksumType::Crc32c);
  EXPECT_EQ(config.serialize().at("navyConfig::checksumType"), "crc32c");
}

TEST(NavyConfigTest, OtherSettings) {
  NavyConfig config{};
  config.setMaxConcurrentInserts(maxConcurrentInserts);
  config.setMaxParcelMemoryMB(maxParcelMemoryMB);
  EXPECT_EQ(config.getMaxConcurrentInserts(), maxConcurrentInserts);
  EXPECT_EQ(config.getMaxParcelMemoryMB(), maxParcelMemoryMB);
}
} // namespace tests
} // namespace cachelib
} // namespace facebook
