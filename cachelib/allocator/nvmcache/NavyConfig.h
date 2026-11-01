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

#pragma once

#include <fmt/format.h>
#include <folly/json/dynamic.h>
#include <folly/logging/xlog.h>

#include <stdexcept>

#include "cachelib/allocator/nvmcache/BlockCacheReinsertionPolicy.h"
#include "cachelib/common/EventInterface.h"
#include "cachelib/common/Hash.h"

namespace facebook {
namespace cachelib {
namespace navy {

class Index;

/**
 * RandomAPConfig provides APIs for users to configure one of the admission
 * policy - "random". Admission policy is one part of NavyConfig.
 *
 * By this class, users can:
 * - set admission probability
 * - get the value of admission probability
 */
class RandomAPConfig {
 public:
  // Set admission probability for "random" policy.
  // @throw std::std::invalid_argument if the input value is not in the range
  //        of [0, 1].
  RandomAPConfig& setAdmProbability(double admProbability);

  double getAdmProbability() const { return admProbability_; }

 private:
  // Admission probability in decimal form.
  double admProbability_{};
};

/**
 * DynamicRandomAPConfig provides APIs for users to configure one of the
 * admission policy - "dynamic_random". Admission policy is one part of
 * NavyConfig.
 *
 *
 * By this class, users can:
 * - set admission target write rate
 * - set max write rate
 * - set admission suffix length
 * - set base size of baseProbability calculation
 * - get the values of the above parameters
 */
class DynamicRandomAPConfig {
 public:
  using FnBypass = std::function<bool(folly::StringPiece)>;
  // Set admission policy's target rate in bytes/s.
  // This target is enforced across a window in average. Default to be 0 if not
  // set, meaning no rate limiting.
  DynamicRandomAPConfig& setAdmWriteRate(uint64_t admWriteRate) noexcept {
    admWriteRate_ = admWriteRate;
    return *this;
  }

  // Set the max write rate to device in bytes/s.
  // This ensures write at any given second don't exceed this limit despite a
  // possibility of writing more to stay within the target rate above.
  DynamicRandomAPConfig& setMaxWriteRate(uint64_t maxWriteRate) noexcept {
    maxWriteRate_ = maxWriteRate;
    return *this;
  }

  // Set the length of suffix in key to be ignored when hashing for
  // probability.
  DynamicRandomAPConfig& setAdmSuffixLength(size_t admSuffixLen) noexcept {
    admSuffixLen_ = admSuffixLen;
    return *this;
  }

  // Set the Navy item base size for base probability calculation.
  // Set this closer to the mean size of objects. The probability is scaled for
  // other sizes by using this size as the pivot.
  DynamicRandomAPConfig& setAdmProbBaseSize(uint32_t admProbBaseSize) noexcept {
    admProbBaseSize_ = admProbBaseSize;
    return *this;
  }

  // Set the range for probability factor.
  // Non-positive values in either of the field would make
  // both values ignored and the default values from DynamicRandomAP::Config
  // will be used.
  DynamicRandomAPConfig& setProbFactorRange(double lowerBound,
                                            double upperBound) noexcept {
    probFactorLowerBound_ = lowerBound;
    probFactorUpperBound_ = upperBound;
    return *this;
  }

  DynamicRandomAPConfig& enableLogging(bool enable) noexcept {
    enableLogging_ = enable;
    return *this;
  }

  // Set a function to determine which items bypass the admission policy.
  DynamicRandomAPConfig& setFnBypass(FnBypass fn) {
    fnBypass_ = std::move(fn);
    return *this;
  }

  uint64_t getAdmWriteRate() const { return admWriteRate_; }

  uint64_t getMaxWriteRate() const { return maxWriteRate_; }

  size_t getAdmSuffixLength() const { return admSuffixLen_; }

  uint32_t getAdmProbBaseSize() const { return admProbBaseSize_; }

  double getProbFactorLowerBound() const { return probFactorLowerBound_; }

  double getProbFactorUpperBound() const { return probFactorUpperBound_; }

  bool getEnableLogging() const { return enableLogging_; }

  FnBypass getFnBypass() const { return fnBypass_; }

 private:
  // Admission policy target rate, bytes/s.
  // Zero means no rate limiting.
  uint64_t admWriteRate_{0};
  // The max write rate to device in bytes/s to stay within the device limit
  // of saturation to avoid latency increase.
  uint64_t maxWriteRate_{0};
  // Length of suffix in key to be ignored when hashing for probability.
  size_t admSuffixLen_{0};
  // Navy item base size of baseProbability calculation.
  size_t admProbBaseSize_{0};
  // Lower bound of the probability factor. Non-positive valu ewould be replaced
  // the default value from DynamicRandomAP::Config
  double probFactorLowerBound_{0};
  // Upper bound of the probability factor. Non-positive value would be
  // replaced the default value from DynamicRandomAP::Config
  double probFactorUpperBound_{0};
  // Whether to putting out logs for more information
  bool enableLogging_{false};
  // Bypass function to determine keys to bypass in admission policy.
  FnBypass fnBypass_;
};

/**
 * BlockCacheIndexConfig provides APIs for users to configure BlockCache index.
 * BlockCache index can be either fixed sized or sparse_map.
 *
 * With fixed sized index, users can configure the number of buckets and mutexes
 * it will maintain, and the overall memory footprint used for index will be
 * fixed depending on that configuration. If too small number of buckets are
 * populated, it will increase the chances of hash collision. Also too small
 * number of mutexes will increase the lock contention.
 *
 * With sparse_map index, it will dynamically adjust the number of buckets
 * depending on the number of entries stored and hash distribution to avoid hash
 * collision. However, it will keep rehashing at runtime, meaning that it will
 * increase resizing costs (accompanying memory allocations and copies) and also
 * memory footprint that it uses can't be controlled (Adding more entries will
 * consume more memory).
 *
 * Another side effect caused by sparse_map index implementation is that it may
 * consume much more memory per entries than fixed sized one even when the same
 * number of buckets are populated and stored.
 */
class BlockCacheIndexConfig {
 public:
  // These default constants are defined here for backward compatibility
  // Without changing any BlockCacheIndexConfig, it should give the same
  // config values as previously used in SparseMapIndex implementation.
  static constexpr uint32_t kDefaultNumSparseMapBuckets{64 * 1024};
  static constexpr uint64_t kDefaultNumBucketsPerMutex{64};

  BlockCacheIndexConfig& enableFixedSizeIndex(bool enable) {
    enableFixedSizeIndex_ = enable;
    return *this;
  }

  // TODO: For the config values for fixed size index, we need to add more
  // checks to see if it's a reasonable range with the block cache size given
  BlockCacheIndexConfig& setNumChunks(uint32_t numChunks) {
    if (numChunks == 0) {
      throw std::invalid_argument("numChunks must be > 0");
    }
    numChunks_ = numChunks;
    return *this;
  }

  BlockCacheIndexConfig& setNumBucketsPerChunkPower(
      uint8_t numBucketsPerChunkPower) {
    if (numBucketsPerChunkPower == 0 || numBucketsPerChunkPower >= 64) {
      throw std::invalid_argument(
          "numBucketsPerChunkPower is 0 or too large (>= 64).");
    }
    numBucketsPerChunkPower_ = numBucketsPerChunkPower;
    return *this;
  }

  BlockCacheIndexConfig& setNumBucketsPerMutex(uint64_t numBucketsPerMutex) {
    if (numBucketsPerMutex == 0) {
      throw std::invalid_argument("numBucketsPerMutex must be > 0");
    }
    numBucketsPerMutex_ = numBucketsPerMutex;
    return *this;
  }

  BlockCacheIndexConfig& setNumSparseMapBuckets(uint32_t numSparseMapBuckets) {
    if (numSparseMapBuckets == 0 || !folly::isPowTwo(numSparseMapBuckets)) {
      throw std::invalid_argument("numSparseMapBuckets must be power of two");
    }
    numSparseMapBuckets_ = numSparseMapBuckets;
    return *this;
  }

  BlockCacheIndexConfig& enableTrackItemHistory() {
    trackItemHistory_ = true;
    return *this;
  }

  BlockCacheIndexConfig& setPersistUsingShm(bool useShm) {
    persistUsingShm_ = useShm;
    return *this;
  }

  BlockCacheIndexConfig& validate() {
    if (enableFixedSizeIndex_) {
      // with FixedSizeIndex
      if (numChunks_ == 0 || numBucketsPerMutex_ == 0 ||
          numBucketsPerChunkPower_ == 0 || numBucketsPerChunkPower_ >= 64) {
        throw std::invalid_argument(
            "enableFixedSizeIndex is set, but numChunks, numBucketsPerMutex, "
            "or numBucketsPerChunkPower is not a reasonable value");
      }
      if ((uint64_t)numChunks_ * (1ull << numBucketsPerChunkPower_) <
          numBucketsPerMutex_) {
        throw std::invalid_argument(
            "enableFixedSizeIndex is set, numBucketsPerMutex is larger than "
            "total number of buckets");
      }
      if (!persistUsingShm_) {
        throw std::invalid_argument(
            "with FixedSizeIndex, persistence using flash is not supported "
            "yet");
      }
    } else {
      // with SparseMapIndex
      if (numSparseMapBuckets_ == 0 || !folly::isPowTwo(numSparseMapBuckets_)) {
        throw std::invalid_argument(
            "with SparseMapIndex, numSparseMapBuckets must be power of two");
      }
      if (numBucketsPerMutex_ == 0 || !folly::isPowTwo(numBucketsPerMutex_)) {
        throw std::invalid_argument(
            "with SparseMapIndex, numBucketsPerMutex must be power of two");
      }
      if (numBucketsPerMutex_ > numSparseMapBuckets_) {
        throw std::invalid_argument(
            "with SparseMapIndex, numBucketsPerMutex must be <= "
            "numSparseMapBuckets");
      }
      if (persistUsingShm_) {
        throw std::invalid_argument(
            "with SparseMapIndex, persistence using shm is not supported");
      }
    }
    return *this;
  }

  // getter functions
  bool isFixedSizeIndexEnabled() const { return enableFixedSizeIndex_; }

  uint32_t getNumChunks() const { return numChunks_; }
  uint8_t getNumBucketsPerChunkPower() const {
    return numBucketsPerChunkPower_;
  }
  uint64_t getNumBucketsPerMutex() const { return numBucketsPerMutex_; }
  uint32_t getNumSparseMapBuckets() const { return numSparseMapBuckets_; }
  bool isTrackItemHistoryEnabled() const { return trackItemHistory_; }
  bool useShmToPersist() const { return persistUsingShm_; }

 private:
  // Whether to enable fixed size index, true for enabling it.
  // If false, we will use sparse_map index which will dynamically adjust the
  // sizes and the number of buckets which is convenient but more expensive
  bool enableFixedSizeIndex_{false};
  // The number of buckets per mutex. Each mutex will cover a consecutive range
  // of buckets with the size of the given number here.
  uint64_t numBucketsPerMutex_{kDefaultNumBucketsPerMutex};

  // The number of buckets with SparseMapIndex
  // Each 'bucket' in SparseMapIndex is a sparse_map instance and will be
  // expanded to a hashtable with mem alloc and rehashing while more items
  // are populated
  uint32_t numSparseMapBuckets_{kDefaultNumSparseMapBuckets};

  // Whether to track item's hits history instead of total hits. Only applies to
  // SparseMapIndex. This is a compromise to keep index size low, enabling this
  // will make totalHits return undefined value.
  bool trackItemHistory_{false};

  // Use Shm to persist. This is only for FixedSizeIndex.
  // SparseMapIndex doesn't support persistence using Shm (and don't have a plan
  // to do so)
  // TODO: Currently, FixedSizeIndex only support persistence using Shm. We may
  // support persistenc using flash in the future.
  bool persistUsingShm_{false};

  // Below are parameters for the fixed size index, and when it's not enabled
  // they will be ignored

  // The number of total chunks. Fixed size index will be divided into chunks
  // and each chunk will maintain a fixed number of buckets with the fixed
  // number mutexes.
  uint32_t numChunks_{4};
  // The integer power for the number of buckets per chunk which should be a
  // power of 2.
  // The total number of buckets = numChunks_ * (2 ^ numBucketsPerChunkPower_)
  uint8_t numBucketsPerChunkPower_{27};
};

/**
 * BlockCacheReinsertionConfig provides APIs for users to configure BlockCache
 * reinsertion policy, which is a part of NavyConfig.
 *
 * By this class, users can enable one of the following reinsertion policies:
 * - hits-based
 * - probability-based
 * - custom
 */
class BlockCacheReinsertionConfig {
 public:
  BlockCacheReinsertionConfig& enableHitsBased(uint8_t hitsThreshold) {
    if (pctThreshold_ > 0 || makeCustomPolicy_) {
      throw std::invalid_argument(
          "already set reinsertion percentage threshold, should not set "
          "reinsertion hits threshold");
    }

    hitsThreshold_ = hitsThreshold;
    return *this;
  }

  BlockCacheReinsertionConfig& enablePctBased(unsigned int pctThreshold) {
    if (hitsThreshold_ > 0 || makeCustomPolicy_) {
      throw std::invalid_argument(
          "already set reinsertion hits threshold, should not set reinsertion "
          "probability threshold");
    }
    if (pctThreshold > 100) {
      throw std::invalid_argument(
          fmt::format("reinsertion percentage threshold should between 0 and "
                      "100, but {} is set",
                      pctThreshold));
    }
    pctThreshold_ = pctThreshold;
    return *this;
  }

  BlockCacheReinsertionConfig& enableCustom(
      std::function<std::shared_ptr<BlockCacheReinsertionPolicy>(const Index&)>
          makeCustomPolicy) {
    if (hitsThreshold_ > 0 || pctThreshold_ > 0) {
      throw std::invalid_argument(fmt::format(
          "Already set reinsertion hits threshold {}, or reinsertion "
          "probability threshold {} while trying to set a custom reinsertion "
          "policy.",
          hitsThreshold_, pctThreshold_));
    }
    makeCustomPolicy_ = makeCustomPolicy;
    return *this;
  }

  BlockCacheReinsertionConfig& validate() {
    if ((pctThreshold_ > 0) + (hitsThreshold_ > 0) +
            (makeCustomPolicy_ != nullptr) >
        1) {
      throw std::invalid_argument(fmt::format(
          "More than one configuration for reinsertion policy is specified: "
          "pctThreshold_ {}, hitsThreshold_ {}, custom_ {}",
          pctThreshold_, hitsThreshold_, makeCustomPolicy_ != nullptr));
    }
    return *this;
  }

  uint8_t getHitsThreshold() const { return hitsThreshold_; }

  unsigned int getPctThreshold() const { return pctThreshold_; }

  std::shared_ptr<BlockCacheReinsertionPolicy> getCustomPolicy(
      const Index& index) const {
    ensureCustomPolicy(index);
    return createdCustomPolicy_;
  }

 private:
  // Only one of the field below can be initialized.

  // Threshold of a hits based reinsertion policy with Navy BlockCache.
  // If an item had been accessed more than that threshold, it will be
  // eligible for reinsertion.
  uint8_t hitsThreshold_{0};
  // Threshold of a percentage based reinsertion policy with Navy BlockCache.
  // The percentage value is between 0 and 100 for reinsertion.
  unsigned int pctThreshold_{0};

  // A constructor for a custom reinsertion policy.
  std::function<std::shared_ptr<BlockCacheReinsertionPolicy>(const Index&)>
      makeCustomPolicy_{nullptr};
  std::shared_ptr<BlockCacheReinsertionPolicy> createdCustomPolicy_{nullptr};

  void ensureCustomPolicy(const Index& index) const {
    if (!createdCustomPolicy_ && makeCustomPolicy_) {
      const_cast<BlockCacheReinsertionConfig*>(this)->createdCustomPolicy_ =
          makeCustomPolicy_(index);
    }
  }
};

/**
 * BlockCacheConfig provides APIs for users to configure BlockCache engine,
 * which is one part of NavyConfig.
 *
 * By this class, users can:
 * - enable FIFO or segmented FIFO eviction policy (default is LRU)
 * - set number of clean regions
 * - enable in-mem buffer (once enabled, the number is 2 * clean regions)
 * - set size classes
 * - set region size
 * - set data checksum
 * - get the values of all the above parameters
 */
class BlockCacheConfig {
 public:
  // Enable FIFO eviction policy (LRU will be disabled).
  BlockCacheConfig& enableFifo() noexcept {
    lru_ = false;
    return *this;
  }

  // Enable segmented FIFO eviction policy (LRU will be disabled)
  // @param sFifoSegmentRatio maps to segments in the order from
  //        least-important to most-important.
  //        e.g. {1, 1, 1} gives equal share in each of the 3 segments;
  //             {1, 2, 3} gives the 1/6th of the items in the first segment (P0
  //             least important), 2/6th of the items in the second segment
  //             (P1), and finally 3/6th of the items in the third segment (P2).
  // @param Number of allocators for each priority. If not set, each priority
  // would have one allocator.
  BlockCacheConfig& enableSegmentedFifo(
      std::vector<unsigned int> sFifoSegmentRatio,
      std::vector<uint32_t> allocatorCounts = {}) noexcept {
    sFifoSegmentRatio_ = std::move(sFifoSegmentRatio);
    if (allocatorCounts.size() > 0) {
      XDCHECK_EQ(sFifoSegmentRatio_.size(), allocatorCounts.size());
      allocatorsPerPriority_ = std::move(allocatorCounts);
    }
    lru_ = false;
    return *this;
  }

  // Enable hit-based reinsertion policy.
  // When evicting regions, items that exceed this threshold of access will be
  // preserved by reinserting them internally.
  // @throw std::invalid_argument if any other reinsertion policy has been
  // enabled.
  BlockCacheConfig& enableHitsBasedReinsertion(uint8_t hitsThreshold);

  // Enable percentage based reinsertion policy.
  // This is used for testing where a certain fraction of evicted items
  // (governed by the percentage) are always reinserted.
  // @throw std::invalid_argument if any other reinsertion policy has
  //        been enabled or the input value is not in the range of 0~100.
  BlockCacheConfig& enablePctBasedReinsertion(unsigned int pctThreshold);

  // Enable a customized reinsertion policy created by the user.
  // @throw std::invalid_argument if any other reinsertion policy has been
  // enabled.
  BlockCacheConfig& enableCustomReinsertion(
      std::shared_ptr<BlockCacheReinsertionPolicy> policy);

  // Enable a customized reinsertion policy created by the user.
  // This version calls a user-provided function to construct the policy
  // and the policy will have access to the BlockCache Index.
  // @throw std::invalid_argument if any other reinsertion policy has been
  // enabled.
  BlockCacheConfig& enableCustomReinsertion(
      std::function<std::shared_ptr<BlockCacheReinsertionPolicy>(const Index&)>
          makeCustomPolicy);

  // Set number of clean regions that are maintained for incoming write and
  // whether the writes are buffered in-memory.
  // Navy needs to maintain sufficient buffers for each clean region that is
  // reserved. This ensures each time we obtain a new in-mem buffer, we have a
  // clean region to flush it to flash once it's ready.
  BlockCacheConfig& setCleanRegions(uint32_t cleanRegions,
                                    uint32_t cleanRegionThreads = 1);

  BlockCacheConfig& setRegionSize(uint32_t regionSize) noexcept {
    regionSize_ = regionSize;
    return *this;
  }

  BlockCacheConfig& setLegacyEventTracker(
      LegacyEventTracker& legacyEventTracker) {
    legacyEventTracker_ =
        std::reference_wrapper<LegacyEventTracker>(legacyEventTracker);
    return *this;
  }

  const std::optional<std::reference_wrapper<LegacyEventTracker>>&
  getLegacyEventTracker() const {
    return legacyEventTracker_;
  }

  BlockCacheConfig& setDataChecksum(bool dataChecksum) noexcept {
    dataChecksum_ = dataChecksum;
    return *this;
  }

  // Offload data checksumming (fused with the value copy on the write path)
  // to Intel DSA via the DTO library. Requires data checksum to be enabled
  // and CacheLib built with BUILD_WITH_DTO. @minSize is the minimum value
  // size to use the offloaded path; smaller values are checksummed in
  // software, since submitting and polling a descriptor costs about what the
  // CPU needs to CRC 16-32 KiB (16 KiB measured as the break-even).
  BlockCacheConfig& setChecksumOffload(bool checksumOffload,
                                       uint32_t minSize = 16384) noexcept {
    checksumOffload_ = checksumOffload;
    checksumOffloadMinSize_ = minSize;
    return *this;
  }

  // Separate minimum size for offloading checksum VERIFICATION on the read,
  // reclaim and reinsertion paths; 0 = same as the write-path gate. Verifying
  // has no CPU work to overlap with the accelerator, so its break-even is
  // higher than the fused write.
  BlockCacheConfig& setChecksumOffloadReadMinSize(uint32_t minSize) noexcept {
    checksumOffloadReadMinSize_ = minSize;
    return *this;
  }

  // Cache-control hint on the fused write-path copy (default true): steer the
  // value bytes toward the CPU cache. Turn off when the next reader of the
  // region buffer is the device (directFlush / flush copy offload).
  BlockCacheConfig& setChecksumOffloadCacheControl(bool enable) noexcept {
    checksumOffloadCacheControl_ = enable;
    return *this;
  }

  // Region reclaim/cleanup verify values as DSA Batch descriptors (<= 64 per
  // submission) instead of one descriptor each. Default false; needs the
  // checksum offload. See BlockCache::Config::checksumOffloadBatchReclaim.
  BlockCacheConfig& setChecksumOffloadBatchReclaim(bool enable) noexcept {
    checksumOffloadBatchReclaim_ = enable;
    return *this;
  }

  // Coalesce write-path fused copy+CRC descriptors across the fibers of a
  // NavyThread (yielding only when siblings are runnable). Default false;
  // measured neutral to slightly negative. See BlockCache::Config.
  BlockCacheConfig& setChecksumOffloadBatch(bool enable) noexcept {
    checksumOffloadBatch_ = enable;
    return *this;
  }

  // Defer the checksum of values below the offload gate to region flush,
  // where they are batched on DSA (implies skipInMemValueVerify). Default
  // false. See BlockCache::Config::checksumDeferSmall.
  BlockCacheConfig& setChecksumDeferSmall(bool enable) noexcept {
    checksumDeferSmall_ = enable;
    return *this;
  }

  // Lookups served from the in-memory region buffer skip the value checksum
  // (the bytes never left DRAM; the header checksum still covers the
  // descriptor). Default false. See BlockCache::Config::skipInMemValueVerify.
  BlockCacheConfig& setSkipInMemValueVerify(bool enable) noexcept {
    skipInMemValueVerify_ = enable;
    return *this;
  }

  BlockCacheConfig& setPreciseRemove(bool preciseRemove) noexcept {
    preciseRemove_ = preciseRemove;
    return *this;
  }

  BlockCacheConfig& setSize(uint64_t size) noexcept {
    size_ = size;
    return *this;
  }

  BlockCacheConfig& setRegionManagerFlushAsync(bool async) noexcept {
    regionManagerFlushAsync_ = async;
    return *this;
  }

  BlockCacheConfig& setRecoverEvictionPolicy(bool enable) noexcept {
    recoverEvictionPolicy_ = enable;
    return *this;
  }

  BlockCacheConfig& setDirectFlush(bool enable) noexcept {
    directFlush_ = enable;
    return *this;
  }

  // When directFlush is off: copy the region buffer into the flush write
  // buffer with one Intel DSA Memory Move instead of memcpy. Requires
  // CacheLib built with BUILD_WITH_DTO; falls back to memcpy if DSA is
  // unusable. The write buffers are pooled, so after the first flush per
  // buffer their pages are resident; a work queue with block-on-fault covers
  // the first touch.
  BlockCacheConfig& setFlushCopyOffload(bool enable) noexcept {
    flushCopyOffload_ = enable;
    return *this;
  }

  BlockCacheConfig& setAllocatorCount(uint32_t numAllocators) noexcept {
    allocatorsPerPriority_ = {numAllocators};
    return *this;
  }

  BlockCacheConfig& useCombinedEntryBlock(bool enable) noexcept {
    useCombinedEntryBlock_ = enable;
    return *this;
  }

  // TO enable Sparse Map Index with the configurable parameters. Without
  // calling this explicitly, default index will be still SparseMapIndex
  // with the default parameters.
  // Call enableSparseMapIndex() explicitly when parameters should be changed.
  // numSparseMapBuckets will determine the number of sparse map instances.
  // numBucketsPerMutes will be used to determine the number of sparse map
  // instances covered by each mutex.
  BlockCacheConfig& enableSparseMapIndex(uint32_t numSparseMapBuckets,
                                         uint32_t numBucketsPerMutex);
  BlockCacheConfig& enableSparseMapIndex(uint32_t numSparseMapBuckets,
                                         uint32_t numBucketsPerMutex,
                                         bool trackItemHistory);
  // To enable fixed size Index for BC. All the parameter should be valid.
  // Without calling this explicitly, default index will be SparseMapIndex
  // with the default parameters.
  //
  // With the fixed size index, the number of buckets that it can hold is
  // decided by the configured numbers.
  // The table is managed by a number of chunks (numChunks), and each chunk will
  // maintain a number of buckets (2 ^ numBucketsPerChunkPower). For example, if
  // numChunks is 4 and numBucketsPerChunkPower is 27, the total number of
  // buckets is 4 * (2 ^ 27) = 512M. Those numbers should be decided considering
  // the total entries populated with the cache and the memory footprint that it
  // could consume.
  // numBucketsPerMutex is for how many buckets will be covered by each mutex
  // and total number mutexes will be decided by this number.
  BlockCacheConfig& enableFixedSizeIndex(uint32_t numChunks,
                                         uint8_t numBucketsPerChunkPower,
                                         uint64_t numBucketsPerMutex);

  bool isLruEnabled() const { return lru_; }

  const std::vector<unsigned int>& getSFifoSegmentRatio() const {
    return sFifoSegmentRatio_;
  }

  uint32_t getCleanRegions() const { return cleanRegions_; }

  uint32_t getCleanRegionThreads() const { return cleanRegionThreads_; }

  uint32_t getNumInMemBuffers() const { return numInMemBuffers_; }

  uint32_t getRegionSize() const { return regionSize_; }

  bool getDataChecksum() const { return dataChecksum_; }

  bool getChecksumOffload() const { return checksumOffload_; }

  uint32_t getChecksumOffloadMinSize() const { return checksumOffloadMinSize_; }

  uint32_t getChecksumOffloadReadMinSize() const {
    return checksumOffloadReadMinSize_;
  }

  bool getChecksumOffloadCacheControl() const {
    return checksumOffloadCacheControl_;
  }

  bool getChecksumOffloadBatchReclaim() const {
    return checksumOffloadBatchReclaim_;
  }
  bool getChecksumOffloadBatch() const { return checksumOffloadBatch_; }
  bool getChecksumDeferSmall() const { return checksumDeferSmall_; }
  bool getSkipInMemValueVerify() const { return skipInMemValueVerify_; }

  uint64_t getSize() const { return size_; }

  bool isRegionManagerFlushAsync() const { return regionManagerFlushAsync_; }

  bool isRecoverEvictionPolicy() const { return recoverEvictionPolicy_; }

  bool isDirectFlush() const { return directFlush_; }
  bool isFlushCopyOffload() const { return flushCopyOffload_; }

  bool isCombinedEntryBlockEnabled() const { return useCombinedEntryBlock_; }

  const BlockCacheReinsertionConfig& getReinsertionConfig() const {
    return reinsertionConfig_;
  }

  const BlockCacheIndexConfig& getIndexConfig() const { return indexConfig_; }

  bool isPreciseRemove() const { return preciseRemove_; }

  const std::vector<uint32_t>& getNumAllocatorsPerPriority() const {
    return allocatorsPerPriority_;
  }

 private:
  // Whether Navy BlockCache will use region-based LRU eviction policy.
  bool lru_{true};
  // The ratio of segments for segmented FIFO eviction policy.
  // Once segmented FIFO is enabled, lru_ will be false.
  std::vector<unsigned int> sFifoSegmentRatio_;
  // Config for constructing reinsertion policy.
  BlockCacheReinsertionConfig reinsertionConfig_;
  // Buffer of clean regions to maintain for eviction.
  uint32_t cleanRegions_{1};
  // Number of RegionManager threads to run reclaim and flush.
  // We expect one thread is enough for most of use cases, but can be
  // configured to more threads if needed
  unsigned int cleanRegionThreads_{1};
  // Number of Navy BlockCache in-memory buffers.
  uint32_t numInMemBuffers_{2};
  // Size for a region for Navy BlockCache (must be multiple of
  // blockSize_).
  uint32_t regionSize_{16 * 1024 * 1024};
  // Whether enabling data checksum for Navy BlockCache.
  bool dataChecksum_{true};
  // Whether to offload data checksumming to Intel DSA (fused with the value
  // copy on the write path), and the minimum value size to do so.
  bool checksumOffload_{false};
  uint32_t checksumOffloadMinSize_{16384};
  uint32_t checksumOffloadReadMinSize_{0};
  bool checksumOffloadCacheControl_{true};
  bool checksumOffloadBatchReclaim_{false};
  bool checksumOffloadBatch_{false};
  bool checksumDeferSmall_{false};
  bool skipInMemValueVerify_{false};
  // Whether to remove an item by checking the key (true) or only the hash value
  // (false).
  bool preciseRemove_{false};

  // Intended size of the block cache.
  // If 0, this block cache takes all the space left on the device.
  uint64_t size_{0};

  // Whether the region manager workers flushes asynchronously.
  bool regionManagerFlushAsync_{false};

  // Whether to persist and recover eviction policy ordering across restarts.
  bool recoverEvictionPolicy_{false};

  // Whether to write region buffer directly without intermediate copy.
  bool directFlush_{false};

  // Whether to do the flush copy (when not directFlush) on Intel DSA.
  bool flushCopyOffload_{false};

  // Whether to use Combined entry block (For index entries and small sized
  // items).
  // Only FixedSizeIndex will support this and it doesn't work with
  // SparseMapIndex
  //
  // TODO: For now, only index entries will be handled with Combined entry block
  bool useCombinedEntryBlock_{false};

  // Number of allocators per priority.
  // Do not set this directly. This should be configured by setAllocatorCount
  // for FIFO and LRU, and enableSegmentedFifo for segmented FIFO.
  std::vector<uint32_t> allocatorsPerPriority_{1};

  // Index related config. If not specified, SparseMapIndex will be used
  BlockCacheIndexConfig indexConfig_;

  std::optional<std::reference_wrapper<LegacyEventTracker>> legacyEventTracker_;

  friend class NavyConfig;
};

/**
 * BigHashConfig provides APIs for users to configure BigHash engine, which is
 * one part of NavyConfig.
 *
 * By this class, users can:
 * - enable BigHash by setting sizePct > 0
 * - set maximum item size
 * - set bucket size
 * - set bloom filter size (0 to disable bloom filter)
 * - get the values of all the above parameters
 */
class BigHashConfig {
 public:
  // Set BigHash device percentage and maximum item size(in bytes) to enable
  // BigHash engine. Default value of sizePct and smallItemMaxSize is 0,
  // meaning BigHash is not enabled.
  // @throw std::invalid_argument if sizePct is not in the range of
  //        [0, 100].
  BigHashConfig& setSizePctAndMaxItemSize(unsigned int sizePct,
                                          uint64_t smallItemMaxSize);

  // Set the bucket size in bytes for BigHash engine.
  // Default value is 4096.
  BigHashConfig& setBucketSize(uint32_t bucketSize) noexcept {
    bucketSize_ = bucketSize;
    return *this;
  }

  // Set bloom filter size per bucket in bytes for BigHash engine.
  // 0 means bloom filter will not be applied. Default value is 8.
  BigHashConfig& setBucketBfSize(uint64_t bucketBfSize) noexcept {
    bucketBfSize_ = bucketBfSize;
    return *this;
  }

  // Set number of mutexes for bucket locking in BigHash engine.
  BigHashConfig& setNumMutexesPower(uint8_t numMutexesPower) noexcept {
    numMutexesPower_ = numMutexesPower;
    return *this;
  }

  // Offload bucket checksumming to Intel DSA via the DTO library. Most
  // beneficial with large buckets (16KB+). Requires CacheLib built with
  // BUILD_WITH_DTO.
  BigHashConfig& setChecksumOffload(bool checksumOffload) noexcept {
    checksumOffload_ = checksumOffload;
    return *this;
  }

  bool isBloomFilterEnabled() const { return bucketBfSize_ > 0; }

  unsigned int getSizePct() const { return sizePct_; }

  uint32_t getBucketSize() const { return bucketSize_; }

  uint64_t getBucketBfSize() const { return bucketBfSize_; }

  uint64_t getSmallItemMaxSize() const { return smallItemMaxSize_; }

  uint8_t getNumMutexesPower() const { return numMutexesPower_; }

  bool getChecksumOffload() const { return checksumOffload_; }

 private:
  // Percentage of how much of the device out of all is given to BigHash
  // engine in Navy, e.g. 50.
  unsigned int sizePct_{0};
  // Navy BigHash engine's bucket size (must be multiple of the minimum
  // device io block size).
  // This size determines how big each bucket is and what is the physical
  // write granularity onto the device.
  uint32_t bucketSize_{4096};
  // The bloom filter size per bucket in bytes for Navy BigHash engine
  uint64_t bucketBfSize_{8};
  // The maximum item size to put into Navy BigHash engine.
  uint64_t smallItemMaxSize_{};
  // numMutexes = 1 << numMutexesPower_.
  uint8_t numMutexesPower_{14};
  // Whether to offload bucket checksumming to Intel DSA.
  bool checksumOffload_{false};
};

// Config for a pair of small,large engines.
class EnginesConfig {
 public:
  const BigHashConfig& bigHash() const { return bigHashConfig_; }

  const BlockCacheConfig& blockCache() const { return blockCacheConfig_; }

  BigHashConfig& bigHash() { return bigHashConfig_; }

  BlockCacheConfig& blockCache() { return blockCacheConfig_; }

  std::map<std::string, std::string> serialize() const;

  bool isBigHashEnabled() const { return bigHashConfig_.getSizePct() > 0; }

  const std::string& getName() const { return name_; }

  void setName(std::string&& name) { name_ = std::move(name); }

 private:
  BlockCacheConfig blockCacheConfig_;
  BigHashConfig bigHashConfig_;
  std::string name_;
};

enum class IoEngine : uint8_t { IoUring, LibAio, Sync };

inline const folly::StringPiece getIoEngineName(IoEngine e) {
  switch (e) {
  case IoEngine::IoUring:
    return "io_uring";
  case IoEngine::LibAio:
    return "libaio";
  case IoEngine::Sync:
    return "sync";
  }
  XDCHECK(false);
  return "invalid";
}

/**
 * For testing, we can simulate bad device by setting this enum value
 * with NavyConfig::setBadDeviceForTesting()
 */
enum class BadDeviceStatus : uint8_t {
  None,           // No bad device
  DataCorruption, // Data corruption (Doesn't necessarily mean device's fault,
                  // will yield checksum error)
  IoReqFailure    // Device that fails with IO requests
};

/**
 * NavyConfig provides APIs for users to set up Navy related settings for
 * NvmCache.
 *
 * Notes: the reason why these settings cannot be directly passed to Navy
 * internal config navy/Factory.h and setup there is
 * because we have logic in "NavySetup.cpp" that translates this input config
 * into CacheProto. Therefore, we need this intermediary config in NvmCache
 * Config.
 *
 */
class NavyConfig {
 public:
  using EnginesSelector = std::function<size_t(HashedKey)>;

  static constexpr folly::StringPiece kAdmPolicyRandom{"random"};
  static constexpr folly::StringPiece kAdmPolicyDynamicRandom{"dynamic_random"};
  static constexpr uint32_t kDefaultNumReaderThreads{32};
  static constexpr uint32_t kDefaultNumWriterThreads{32};

  bool usesSimpleFile() const noexcept { return !fileName_.empty(); }
  bool usesRaidFiles() const noexcept { return raidPaths_.size() > 0; }
  bool isBigHashEnabled() const {
    return enginesConfigs_[0].bigHash().getSizePct() > 0;
  }
  bool isFDPEnabled() const { return enableFDP_; }

  std::map<std::string, std::string> serialize() const;

  // Getters:
  // ============ Admission Policy =============
  const std::string& getAdmissionPolicy() const { return admissionPolicy_; }

  // Get a const DynamicRandomAPConfig to read values of its parameters.
  const DynamicRandomAPConfig& dynamicRandomAdmPolicy() const {
    return dynamicRandomAPConfig_;
  }

  // Get a const RandomAPConfig to read values of its parameters.
  const RandomAPConfig& randomAdmPolicy() const { return randomAPConfig_; }

  // ============ Device settings =============
  uint32_t getBlockSize() const { return blockSize_; }
  bool getExclusiveOwner() const { return isExclusiveOwner_; }
  const std::string& getFileName() const;
  const std::vector<std::string>& getRaidPaths() const;
  uint64_t getDeviceMetadataSize() const { return deviceMetadataSize_; }
  uint32_t getMaxKeySize() const { return maxKeySize_; }
  uint64_t getFileSize() const { return fileSize_; }
  bool getTruncateFile() const { return truncateFile_; }
  uint32_t getDeviceMaxWriteSize() const { return deviceMaxWriteSize_; }
  uint32_t getMetadataStagingSize() const { return metadataStagingSize_; }
  IoEngine getIoEngine() const { return ioEngine_; }
  uint32_t getQDepth() const { return qDepth_; }
  BadDeviceStatus hasBadDeviceForTesting() const { return testingBadDevice_; }

  // Return a const BigHashConfig to read values of its parameters.
  const BigHashConfig& bigHash() const {
    XDCHECK(enginesConfigs_.size() > 0);
    return enginesConfigs_[0].bigHash();
  }

  // Return a const BlockCacheConfig to read values of its parameters.
  const BlockCacheConfig& blockCache() const {
    XDCHECK(enginesConfigs_.size() > 0);
    return enginesConfigs_[0].blockCache();
  }

  // ============ Job scheduler settings =============
  uint32_t getReaderThreads() const {
    // return default value if it's not explicitly set
    return (readerThreads_ == 0) ? kDefaultNumReaderThreads : readerThreads_;
  }
  uint32_t getWriterThreads() const {
    // return default value if it's not explicitly set
    return (writerThreads_ == 0) ? kDefaultNumWriterThreads : writerThreads_;
  }
  uint64_t getNavyReqOrderingShards() const { return navyReqOrderingShards_; }

  int getReaderThreadsPriority() const { return readerThreadsPriority_; }
  int getWriterThreadsPriority() const { return writerThreadsPriority_; }

  uint32_t getMaxNumReads() const { return maxNumReads_; }
  uint32_t getMaxNumWrites() const { return maxNumWrites_; }
  uint32_t getStackSize() const { return stackSize_; }
  // ============ other settings =============
  uint32_t getMaxConcurrentInserts() const { return maxConcurrentInserts_; }
  uint64_t getMaxParcelMemoryMB() const { return maxParcelMemoryMB_; }
  bool getUseEstimatedWriteSize() const { return useEstimatedWriteSize_; }
  size_t getNumShards() const { return numShards_; }
  bool getEnableAccessTimeMap() const { return enableAccessTimeMap_; }
  size_t getAccessTimeMapMaxSize() const { return accessTimeMapMaxSize_; }

  // Setters:
  // Enable "dynamic_random" admission policy.
  // @return DynamicRandomAPConfig (for configuration)
  // @throw  invalid_argument if admissionPolicy_ is not empty
  DynamicRandomAPConfig& enableDynamicRandomAdmPolicy();

  // Enable "random" admission policy.
  // @return RandomAPConfig (for configuration)
  // @throw invalid_argument if admissionPolicy_ is not empty
  RandomAPConfig& enableRandomAdmPolicy();

  // ============ Device settings =============
  // Set the device block size, i.e., minimum unit of IO
  void setBlockSize(uint32_t blockSize) noexcept { blockSize_ = blockSize; }
  // Set the NVMe FDP Device data placement mode in the Cachelib
  void setEnableFDP(bool enable) noexcept { enableFDP_ = enable; }
  // If true, Navy will only start if it's the sole owner of the file.
  // This only applies to non-memory-backed files.
  void setExclusiveOwner(bool isExclusiveOwner) noexcept {
    isExclusiveOwner_ = isExclusiveOwner;
  }
  // Set the parameters for a simple file.
  // @throw std::invalid_argument if RAID files have been already set.
  void setSimpleFile(const std::string& fileName,
                     uint64_t fileSize,
                     bool truncateFile = false);
  // Set the parameters for RAID files.
  // @throw std::invalid_argument if a simple file has been already set
  //        or there is only one or fewer RAID paths.
  void setRaidFiles(std::vector<std::string> raidPaths,
                    uint64_t fileSize,
                    bool truncateFile = false);
  // Set the parameter for a in-memory file.
  // This function is only for cachebench and unit tests to create
  // a MemoryDevice when no file path is set.
  void setMemoryFile(uint64_t fileSize) noexcept { fileSize_ = fileSize; }
  // Set up a bad device backed by the existing device that user has configured.
  // This requires the user to have also set up a real device (one of the
  // above).
  // @param badStatus: whether the device will return bad behavior defined in
  //                   BadDeviceStatus enum.
  // TODO: user can add more options to tune the bad device's behavior. E.g. add
  //       probability for introducting data corruption. Add probability to
  //       return error on read or write IOs.
  void setBadDeviceForTesting(BadDeviceStatus badStatus) {
    testingBadDevice_ = badStatus;
  }

  // Configure the size of the metadata partition reserved for Navy
  void setDeviceMetadataSize(uint64_t deviceMetadataSize) noexcept {
    deviceMetadataSize_ = deviceMetadataSize;
  }
  void setDeviceMaxWriteSize(uint32_t deviceMaxWriteSize) noexcept {
    deviceMaxWriteSize_ = deviceMaxWriteSize;
  }

  // Configure how much metadata is staged in memory per device IO, on both
  // the persist and the recover side. 0 means one block.
  void setMetadataStagingSize(uint32_t metadataStagingSize) noexcept {
    metadataStagingSize_ = metadataStagingSize;
  }

  // Configure the max key size for Navy
  void setMaxKeySize(uint32_t maxKeySize) noexcept { maxKeySize_ = maxKeySize; }

  // Enable AsyncIo
  // If enabled already via job config settings, this will override
  // the qDepth_ or enableIoUring_.
  // If qDepth is 0, existing qDepth_ will be used
  // * CAUTION: This version of enableAsyncIo() is DEPRECATED.
  //          Please use the version with four arguments below
  void enableAsyncIo(unsigned int qDepth, bool enableIoUring);

  // Enable async IO (IO to the device) - either libaio or io_uring
  //  - maxNumReads : the number of concurrent reads issued to the queues and in
  //  process
  //  - maxNumWrites: the number of concurrent writes issued to the queues
  //  and in process
  //  - useIoUring : true to use io_uring
  //  - stackSizeKB : size of the stack for each fibers with the async job
  //  scheduler. 0 for default stack size
  void enableAsyncIo(uint32_t maxNumReads,
                     uint32_t maxNumWrites,
                     bool useIoUring,
                     uint32_t stackSizeKB = 0);

  // ============ BlockCache settings =============
  // Return BlockCacheConfig for configuration.
  BlockCacheConfig& blockCache() noexcept {
    return enginesConfigs_[0].blockCache();
  }

  // ============ BigHash settings =============
  // Return BigHashConfig for configuration.
  BigHashConfig& bigHash() noexcept { return enginesConfigs_[0].bigHash(); }

  void addEnginePair(EnginesConfig config) {
    enginesConfigs_.push_back(std::move(config));
  }
  void setEnginesSelector(EnginesSelector selector) {
    selector_ = std::move(selector);
  }

  // ============ Job scheduler settings =============
  // Set the number of reader threads and writer threads.
  // If maxNumReads and maxNumWrites are all 0, sync IO will be used
  // * CAUTION: This version of setReaderAndWriterThreads() is DEPRECATED.
  //          Please use the version with two arguments below.
  void setReaderAndWriterThreads(unsigned int readerThreads,
                                 unsigned int writerThreads,
                                 unsigned int maxNumReads,
                                 unsigned int maxNumWrites,
                                 unsigned int stackSizeKB = 0);

  // ============ Job scheduler settings =============
  // Set the number of reader threads and writer threads.
  void setReaderAndWriterThreads(uint32_t readerThreads,
                                 uint32_t writerThreads);

  // Set Navy request ordering shards (expressed as power of two).
  // @throw std::invalid_argument if the input value is 0.
  void setNavyReqOrderingShards(uint64_t navyReqOrderingShards);

  // Set the nice value (priority) for reader threads.
  // Valid range is -20 (highest priority) to 19 (lowest priority).
  // 0 means use the default nice value (no change).
  void setReaderThreadsPriority(int priority) noexcept {
    readerThreadsPriority_ = priority;
  }

  // Set the nice value (priority) for writer threads.
  // Valid range is -20 (highest priority) to 19 (lowest priority).
  // 0 means use the default nice value (no change).
  void setWriterThreadsPriority(int priority) noexcept {
    writerThreadsPriority_ = priority;
  }

  // ============ Other settings =============
  void setMaxConcurrentInserts(uint32_t maxConcurrentInserts) noexcept {
    maxConcurrentInserts_ = maxConcurrentInserts;
  }
  void setMaxParcelMemoryMB(uint64_t maxParcelMemoryMB) noexcept {
    maxParcelMemoryMB_ = maxParcelMemoryMB;
  }
  void setUseEstimatedWriteSize(bool useEstimatedWriteSize) noexcept {
    useEstimatedWriteSize_ = useEstimatedWriteSize;
  }
  void setNumShards(size_t numShards) noexcept { numShards_ = numShards; }
  void setEnableAccessTimeMap(bool enable) noexcept {
    enableAccessTimeMap_ = enable;
  }
  void setAccessTimeMapMaxSize(size_t maxSize) noexcept {
    accessTimeMapMaxSize_ = maxSize;
  }

  const std::vector<EnginesConfig>& enginesConfigs() const {
    return enginesConfigs_;
  }

  EnginesSelector getEnginesSelector() const { return selector_; }

 private:
  // ============ AP settings =============
  // Name of the admission policy.
  // This could only be "dynamic_random" or "random" (or empty).
  std::string admissionPolicy_;
  DynamicRandomAPConfig dynamicRandomAPConfig_{};
  RandomAPConfig randomAPConfig_{};

  // ============ Device settings =============
  // Navy specific device block size in bytes.
  uint32_t blockSize_{4096};
  // The maximum key size (in bytes) for items cached in Navy.
  uint32_t maxKeySize_{255};
  // If true, Navy will only start if it's the sole owner of the file.
  bool isExclusiveOwner_{false};
  // The file name/path for caching.
  std::string fileName_;
  // An array of Navy RAID device file paths.
  std::vector<std::string> raidPaths_;
  // The size of the metadata partition on the Navy device.
  uint64_t deviceMetadataSize_{};
  // The size of the file that Navy should use.
  // 0 means to use the whole device.
  uint64_t fileSize_{};
  // Whether ask Navy to truncate the file it uses.
  bool truncateFile_{false};
  // This controls granularity of the writes when we flush the region.
  // This is only used when in-mem buffer is enabled.
  uint32_t deviceMaxWriteSize_{};
  // Bytes staged in memory per metadata IO. Distinct from deviceMetadataSize,
  // which reserves the partition on the device: this changes the IO
  // granularity, not how much metadata is written. 0 means one block.
  uint32_t metadataStagingSize_{0};
  // This controls if device is in bad status (for testing).
  BadDeviceStatus testingBadDevice_{BadDeviceStatus::None};

  // IoEngine type used for IO
  IoEngine ioEngine_{IoEngine::Sync};

  // Number of queue depth per thread for async IO.
  // 0 for Sync io engine and >1 for libaio and io_uring
  uint32_t qDepth_{0};

  // ============ Engines settings =============
  std::vector<EnginesConfig> enginesConfigs_{1};
  // Function to map each item to a pair of engine.
  EnginesSelector selector_{};

  // ============ Job scheduler settings =============
  // Number of asynchronous worker thread for read operation.
  uint32_t readerThreads_{0};
  // Number of asynchronous worker thread for write operation.
  uint32_t writerThreads_{0};
  // Number of shards expressed as power of two for native request ordering in
  // Navy.
  // This value needs to be non-zero.
  uint64_t navyReqOrderingShards_{20};

  // Nice value (priority) for reader threads.
  // Valid range is -20 (highest priority) to 19 (lowest priority).
  // 0 means use the default nice value (no change).
  int readerThreadsPriority_{0};
  // Nice value (priority) for writer threads.
  // Valid range is -20 (highest priority) to 19 (lowest priority).
  // 0 means use the default nice value (no change).
  int writerThreadsPriority_{0};

  // Max number of concurrent reads/writes in whole Navy.
  // This needs to be a multiple of the number of readers and writers.
  // Setting this to non-0 will enable async IO where fibers are used
  // for Navy operations including device IO
  uint32_t maxNumReads_{0};
  uint32_t maxNumWrites_{0};

  // Stack size of fibers when async-io is enabled. 0 for default
  uint32_t stackSize_{0};

  // ============ Other settings =============
  // Maximum number of concurrent inserts we allow globally for Navy.
  // 0 rejects all inserts. Default of 1'000'000 is effectively no limit.
  uint32_t maxConcurrentInserts_{1'000'000};
  // Total memory limit for in-flight parcels.
  // Once this is reached, requests will be rejected until the parcel
  // memory usage gets under the limit.
  uint64_t maxParcelMemoryMB_{256};
  // Whether to use write size (instead of parcel size) for Navy admission
  // policy.
  bool useEstimatedWriteSize_{false};
  // Whether Navy support the NVMe FDP data placement(TP4146) directives or not.
  // Reference: https://nvmexpress.org/nvmeflexible-data-placement-fdp-blog/
  bool enableFDP_{false};
  // Number of nvm lock shards
  size_t numShards_{8192};
  // Whether to enable the AccessTimeMap for tracking NVM item access times.
  bool enableAccessTimeMap_{false};
  // Maximum number of entries in the AccessTimeMap. 0 means unbounded.
  size_t accessTimeMapMaxSize_{0};
};
} // namespace navy
} // namespace cachelib
} // namespace facebook
