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

#include <fmt/core.h>
#include <folly/Benchmark.h>
#include <folly/container/F14Map.h>
#include <folly/hash/Hash.h>
#include <folly/json/DynamicConverter.h>
#include <folly/json/json.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/allocator/HitsPerSlabStrategy.h"
#include "cachelib/allocator/LruTailAgeStrategy.h"
#include "cachelib/allocator/RandomStrategy.h"
#include "cachelib/allocator/Util.h"
#include "cachelib/allocator/nvmcache/NavyConfig.h"
#include "cachelib/allocator/nvmcache/NavySetup.h"
#include "cachelib/cachebench/cache/CacheStats.h"
#include "cachelib/cachebench/cache/CacheValue.h"
#include "cachelib/cachebench/cache/ItemRecords.h"
#include "cachelib/cachebench/cache/TimeStampTicker.h"
#include "cachelib/cachebench/consistency/LogEventStream.h"
#include "cachelib/cachebench/consistency/ValueTracker.h"
#include "cachelib/cachebench/util/CacheConfig.h"
#include "cachelib/cachebench/util/MemoryMonitorScript.h"
#include "cachelib/cachebench/util/NandWrites.h"
#include "cachelib/common/EventTracker.h"
#include "cachelib/common/Throttler.h"

DECLARE_bool(report_api_latency);
DECLARE_string(report_ac_memory_usage_stats);

namespace facebook::cachelib::cachebench {
constexpr folly::StringPiece kCachebenchCacheName = "cachebench";

// An admission policy that rejects items that was last accessed more than
// X seconds ago. This is useful to simulate workloads where we provide a
// retention (soft) guarantee.
template <typename Cache>
class RetentionAP final : public NvmAdmissionPolicy<Cache> {
 public:
  using Item = typename Cache::Item;
  using ChainedItemIter = typename Cache::ChainedItemIter;

  // @param retentionThreshold    reject items with eviction age above
  RetentionAP(uint32_t retentionThreshold)
      : retentionThreshold_{retentionThreshold} {}

 protected:
  bool acceptImpl(const Item& it,
                  folly::Range<ChainedItemIter>) final override {
    auto lastAccessTime = it.getLastAccessTime();
    auto evictionAge = util::getCurrentTimeSec() - lastAccessTime;
    return evictionAge <= retentionThreshold_;
  }

  bool acceptImpl(typename Item::Key /* key */) final override {
    // We don't know eviction age so always return true
    return true;
  }

  virtual void getCountersImpl(const util::CounterVisitor&) final override {}

 private:
  const uint32_t retentionThreshold_{0};
};

// A specialized Cache for cachebench use, backed by Cachelib.
// Items value in this cache follows CacheValue schema, which
// contains a few integers for sanity checks use. So it is invalid
// to use item.getMemory and item.getSize APIs directly and caller must use
// getMemory() through this cache instance.
template <typename Allocator>
class Cache {
 public:
  using Item = typename Allocator::Item;
  using ReadHandle = typename Allocator::ReadHandle;
  using WriteHandle = typename Allocator::WriteHandle;
  using Key = typename Item::Key;
  using RemoveRes = typename Allocator::RemoveRes;
  using SyncObj = typename Allocator::SyncObj;
  using ChainedItemMovingSync = typename Allocator::ChainedItemMovingSync;
  using ChainedAllocs = typename Allocator::ChainedAllocs;

  template <typename U>
  using TypedHandle = LruAllocator::TypedHandle<U>;

  struct DramIteratorSweepResult {
    uint64_t items{0};
    uint64_t keyBytes{0};
    uint64_t valueBytes{0};
  };

  // instantiate a cachelib cache instance.
  //
  // @param config        the configuration for the cache.
  // @param movingSync    workload specific moving sync for constructing the
  //                      cache.
  // @param cacheDir      optional directory for the cache to enable
  //                      persistence across restarts.
  // @param touchValue    read entire value on find
  explicit Cache(const CacheConfig& config,
                 ChainedItemMovingSync movingSync = {},
                 std::string cacheDir = "",
                 bool touchValue = false);

  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;
  Cache(Cache&&) = delete;
  Cache& operator=(Cache&&) = delete;
  ~Cache();

  // allocate an item from the cache.
  //
  // @param pid     the pool id to allocate from
  // @param key     the key for the item
  // @param size    size of the item
  // @param ttlSecs ttl for the item in seconds (optional)
  WriteHandle allocate(PoolId pid,
                       folly::StringPiece key,
                       size_t size,
                       uint32_t ttlSecs = 0);

  // inserts the item into the cache and tracks it.
  WriteHandle insertOrReplace(WriteHandle& handle);

  // inserts the handle into cache and returns true if the insert was
  // successful, false otherwise. Insert operation can not be performed when
  // consistency checking is enabled.
  bool insert(WriteHandle& handle);

  // perform a probalistic existence check in cachelib. False means the key
  // definitely does NOT exist. True means the key likely exists, but a
  // subsequent lookup could still return empty.
  bool couldExist(Key key);

  // perform lookup in the cache and if consistency checking is enabled,
  // ensure that the lookup result is consistent with the past actions and
  // concurrent actions. If NVM is enabled, waits for the Item to become ready
  // before returning. If key is found to be inconsistent, it is marked as
  // invalid.
  //
  // @param key   the key for lookup
  //
  // @return read handle for the item if present or null handle.
  ReadHandle find(Key key);

  // perform lookup in the cache asynchronously. The handle will be returned
  // directly without waiting. Caller needs to handle the consistency check and
  // simulate performance impact when the handle is ready. This is also used to
  // findToWrite asynchronously. The caller can then turn the read handle into a
  // write handle.
  //
  // @param key   the key for lookup
  //
  // @return a semifuture of a read handle for the caller to check if it is
  // ready.
  folly::SemiFuture<ReadHandle> asyncFind(Key key);

  // perform lookup then mutation in the cache and if consistency checking is
  // enabled, ensure that the lookup result is consistent with the past actions
  // and concurrent actions. If NVM is enabled, waits for the Item to become
  // ready before returning. If key is found to be inconsistent, it is marked as
  // invalid.
  //
  // @param key   the key for lookup
  //
  // @return write handle for the item if present or null handle.
  WriteHandle findToWrite(Key key);

  // removes the key from the cache and returns corresponding cache result.
  // Tracks the operation for checking consistency if enabled.
  RemoveRes remove(Key key);
  // same as above, but takes an item handle.
  RemoveRes remove(const ReadHandle& it);

  // allocates a chained item for this parent.
  //
  // @param parent   the parent for the chain
  // @param size     the size of the allocation
  //
  // @return item handle allocated.
  WriteHandle allocateChainedItem(const ReadHandle& parent, size_t size);

  // replace the oldItem belonging to the parent with a new one.
  // @param oldItem         the item being replaced
  // @param newItemHandle   handle to the new item
  // @param parent          the parent item
  //
  // @return handle to the item that was replaced.
  WriteHandle replaceChainedItem(Item& oldItem,
                                 WriteHandle newItemHandle,
                                 Item& parent);

  // Adds a chained item to the parent.
  // @param  parent   the parent item's handle
  // @param child     handle to the child
  void addChainedItem(WriteHandle& parent, WriteHandle child);

  template <typename... Params>
  auto viewAsChainedAllocs(Params&&... args) {
    return cache_->viewAsChainedAllocs(std::forward<Params>(args)...);
  }

  template <typename... Params>
  auto viewAsWritableChainedAllocs(Params&&... args) {
    return cache_->viewAsWritableChainedAllocs(std::forward<Params>(args)...);
  }

  // Item specific accessors for the cache. This is needed since cachebench's
  // cache adds some overheads on top of Cache::Item.

  // Return the readonly memory
  const void* getMemory(const ReadHandle& handle) const noexcept {
    return handle == nullptr ? nullptr : getMemory(*handle);
  }

  // Return the writable memory
  void* getMemory(WriteHandle& handle) noexcept {
    return handle == nullptr ? nullptr : getMemory(*handle);
  }

  // Return the readonly memory
  const void* getMemory(const Item& item) const noexcept {
    return item.template getMemoryAs<CacheValue>()->getData();
  }

  // Return the writable memory
  void* getMemory(Item& item) noexcept {
    return item.template getMemoryAs<CacheValue>()->getData();
  }

  // return the allocation size for the item.
  uint32_t getSize(const ReadHandle& item) const noexcept {
    return getSize(item.get());
  }

  // read entire value on find.
  void touchValue(const ReadHandle& it) const;

  // returns the size of the item, taking into account ItemRecords could be
  // enabled.
  uint32_t getSize(const Item* item) const noexcept;

  // set's a random value for the item when consistency check is enabled.
  void setUint64ToItem(WriteHandle& handle, uint64_t num) const;

  // For chained items, tracks the state of the chain by using the combination
  // of unique checksum value per item in chain into one value.
  void trackChainChecksum(const ReadHandle& handle);

  // set's the string value to the item, stripping the tail if the input
  // string is longer than the item's storage space.
  //
  // @param handle   the handle for the item
  // @param str      the string value to be set.
  void setStringItem(WriteHandle& handle, const std::string& str);

  // when item records are enabled, updates the version for the item and
  // correspondingly invalidates the nvm cache.
  void updateItemRecordVersion(WriteHandle& it);

  // the following three are helper functions to support the cachelib map
  // integration stress tests. These expose the same interface as
  // CacheAllocator. TODO (sathya) expose the underlying cache instance
  // instead of forwarding the apis.
  template <typename... Params>
  auto getUsableSize(Params&&... args) {
    return cache_->getUsableSize(std::forward<Params>(args)...);
  }

  template <typename... Params>
  auto getAllocInfo(Params&&... args) {
    return cache_->getAllocInfo(std::forward<Params>(args)...);
  }

  template <typename... Params>
  const auto& getPool(Params&&... args) {
    return cache_->getPool(std::forward<Params>(args)...);
  }

  // true if the cache only uses DRAM. false if the cache is configured to
  // have NVM as well (even if it is mocked by DRAM underneath).
  bool isRamOnly() const { return config_.nvmCacheSizeMB == 0; }

  // if NVM is being used, checks if there were any errors that could have
  // disable the NVM engine.
  bool isNvmCacheDisabled() const {
    return !isRamOnly() && !cache_->isNvmCacheEnabled();
  }

  double getNvmEvictionRate() const;

  // enables consistency checking for the cache. This should be done before
  // any find/insert/remove is called.
  //
  // @param keys  list of keys that the stressor uses for the workload.
  void enableConsistencyCheck(const std::vector<std::string>& keys);

  // returns true if the consistency checking is enabled.
  bool consistencyCheckEnabled() const { return valueTracker_ != nullptr; }

  // returns true if touching value is enabled.
  bool touchValueEnabled() const { return touchValue_; }

  // return true if the key was previously detected to be inconsistent. This
  // is useful only when consistency checking is enabled by calling
  // enableConsistencyCheck()
  bool isInvalidKey(const std::string_view key) {
    return invalidKeys_.find(key)->second.load(std::memory_order_relaxed);
  }

  // Get overall stats on the whole cache allocator
  std::unique_ptr<StatsBase> getStats() const;

  // Get number of bytes written to NVM.
  double getNvmBytesWritten() const;

  DramIteratorSweepResult runRegularDramIteratorSweep(
      const std::optional<util::Throttler::Config>& throttlerConfig =
          std::nullopt);
  DramIteratorSweepResult runLockGroupDramIteratorSweep(
      const std::optional<util::Throttler::Config>& throttlerConfig =
          std::nullopt);

  // return the stats for the pool.
  PoolStats getPoolStats(PoolId pid) const { return cache_->getPoolStats(pid); }

  // return the total number of inconsistent operations detected since start.
  unsigned int getInconsistencyCount() const {
    return inconsistencyCount_.load(std::memory_order_relaxed);
  }

  // return the number of times Item destructor was called inconsistently.
  uint64_t getInvalidDestructorCount() { return invalidDestructor_; }

  // return the number of pools the cache was configured with.
  uint64_t numPools() const noexcept { return config_.numPools; }

  // reports the number of handles held by the thread calling this api.
  int64_t getHandleCountForThread() const {
    return cache_->getHandleCountForThread();
  }

  // TODO(sathya) clean up this api by making this part of the find api
  // implementation.
  void recordAccess(folly::StringPiece key) {
    if (nvmAdmissionPolicy_) {
      nvmAdmissionPolicy_->trackAccess(key);
    }
  }

  // returns the initialized size of the cache.
  // TODO (sathya) deprecate this after cleaning up FastShutdownStressor
  size_t getCacheSize() const {
    return cache_->getCacheMemoryStats().ramCacheSize;
  }

  // empties the cache entries by removing the keys, this will schedule the
  // destructor call backs to be executed.
  void clearCache(uint64_t errorLimit);

  // shuts down the cache for persistence. User shall not access the instance
  // until the cache is re-attached using reAttach() below.
  void shutDown();

  // reinitialize the cache. This assumes the cache was shutdown using
  // shutDown() api.
  //
  // @throw   std::invalid_argument if the cache can not be re-attached.
  void reAttach();

  // cleanup the cache resources if the cache was persistent one, initialized
  // with a cache directory. TODO (sathya) merge this with shutDown()
  void cleanupSharedMem();

 private:
  // checks for the consistency of the operation for the item
  //
  // @param opId    the operation id
  // @param it      the item to check
  bool checkGet(ValueTracker::Index opId, const ReadHandle& it);

  // fetches the value stored in the item for consistency tracking purposes.
  // Only called if consistency checking is enabled.
  uint64_t getUint64FromItem(const Item& item) const {
    auto ptr = item.template getMemoryAs<CacheValue>();
    return ptr->getConsistencyNum();
  }

  // generates a hash corresponding to the handle and its's chain, based on
  // each item's hash value. Used for consistency tracking purposes.
  uint64_t genHashForChain(const ReadHandle& handle) const;

  // get the nand writes for the SSD device if enabled.
  uint64_t fetchNandWrites() const;

  template <typename Iterator>
  DramIteratorSweepResult runDramIteratorSweep(Iterator iter,
                                               const Iterator& end);

  // original input config for the cache used to derive the
  // CacheAllocatorConfig.
  const CacheConfig config_;

  // config for the currently initialized cache. This is also used to
  // re-attach after shutdown()
  typename Allocator::Config allocatorConfig_;

  // tracks if the cache instance should cleanup the NVM cache. This is done
  // if the resources were allocated by the cache instance in the constructor
  // instead of being provided by the user through the config as a
  // file/device. If this is an empty string, no cleanup is performed.
  std::string nvmCacheFilePath_;

  // The admission policy that tracks the accesses.
  std::shared_ptr<NvmAdmissionPolicy<Allocator>> nvmAdmissionPolicy_;

  // instance of the cache.
  std::unique_ptr<Allocator> cache_;

  // Process-wide memory-reading override used by scripted monitor runs.
  std::unique_ptr<MemoryMonitorScript> memoryMonitorScript_;

  // the monitor for the cache. This is a facebook specific functionality to
  // pull stats from the cachebench directly into facebook monitoring systems.
  std::unique_ptr<CacheMonitor> monitor_;

  // list of pools present in the cache.
  std::vector<PoolId> pools_;

  // this is used to check if the key has been previously reported to have
  // gotten into a inconsistent state and avoiding it for future requests.
  // Since this can be accessed from multiple threads, the map is initialized
  // during start up and only the value is updated by flipping the bit
  // atomically.
  folly::F14NodeMap<std::string, std::atomic<bool>> invalidKeys_;

  // number of inconsistency detected so far with the operations
  std::atomic<unsigned int> inconsistencyCount_{0};

  // tracker for consistency monitoring.
  std::unique_ptr<ValueTracker> valueTracker_;

  // read entire value on find.
  bool touchValue_{false};

  // reading of the nand bytes written for the benchmark if enabled.
  const uint64_t nandBytesBegin_{0};

  // latency stats of cachelib APIs inside cachebench
  mutable util::PercentileStats cacheFindLatency_;

  // when enabled, tracks the keys in the cache independently to validate
  // features like ItemDestructor.
  ItemRecords<Allocator> itemRecords_;

  // number of times we detected an in-correct invocation of the item
  // destructor.
  std::atomic<uint64_t> invalidDestructor_{0};

  // number of times the item destructor was called in the cache, if enabled.
  std::atomic<int64_t> totalDestructor_{0};
};

// Specializations are required for each MMType
template <typename MMConfigType>
MMConfigType makeMMConfig(CacheConfig const&);

// LRU
template <>
inline typename LruAllocator::MMConfig makeMMConfig(CacheConfig const& config) {
  return LruAllocator::MMConfig(config.lruRefreshSec,
                                config.lruRefreshRatio,
                                config.lruUpdateOnWrite,
                                config.lruUpdateOnRead,
                                config.tryLockUpdate,
                                static_cast<uint8_t>(config.lruIpSpec),
                                0,
                                config.useCombinedLockForIterators);
}

// LRU
template <>
inline typename Lru2QAllocator::MMConfig makeMMConfig(
    CacheConfig const& config) {
  return Lru2QAllocator::MMConfig(config.lruRefreshSec,
                                  config.lruRefreshRatio,
                                  config.lruUpdateOnWrite,
                                  config.lruUpdateOnRead,
                                  config.tryLockUpdate,
                                  false,
                                  config.lru2qHotPct,
                                  config.lru2qColdPct,
                                  0,
                                  config.useCombinedLockForIterators);
}

template <typename Allocator>
uint64_t Cache<Allocator>::fetchNandWrites() const {
  size_t total = 0;
  for (const auto& d : config_.writeAmpDeviceList) {
    try {
      total += facebook::hw::nandWriteBytes(d);
    } catch (const std::exception& e) {
      XLOGF(ERR, "Exception fetching nand writes for {}. Msg: {}", d, e.what());
      return 0;
    }
  }
  return total;
}

template <typename Allocator>
Cache<Allocator>::Cache(const CacheConfig& config,
                        ChainedItemMovingSync movingSync,
                        std::string cacheDir,
                        bool touchValue)
    : config_(config),
      touchValue_(touchValue),
      nandBytesBegin_{fetchNandWrites()},
      itemRecords_(config_.enableItemDestructorCheck) {
  constexpr size_t MB = 1024ULL * 1024ULL;

  allocatorConfig_.enablePoolRebalancing(
      config_.getRebalanceStrategy(),
      std::chrono::seconds(config_.poolRebalanceIntervalSec));

  if (config_.moveOnSlabRelease && movingSync != nullptr) {
    allocatorConfig_.enableMovingOnSlabRelease(
        [](Item& oldItem, Item& newItem, Item* parentPtr) {
          XDCHECK(oldItem.isChainedItem() == (parentPtr != nullptr));
          std::memcpy(newItem.getMemory(), oldItem.getMemory(),
                      oldItem.getSize());
        },
        movingSync);
  }

  if (config_.allocSizes.empty()) {
    XDCHECK(config_.minAllocSize >= sizeof(CacheValue));
    allocatorConfig_.setDefaultAllocSizes(util::generateAllocSizes(
        config_.allocFactor, config_.maxAllocSize, config_.minAllocSize, true));
  } else {
    std::set<uint32_t> allocSizes;
    for (uint64_t s : config_.allocSizes) {
      XDCHECK(s >= sizeof(CacheValue));
      allocSizes.insert(s);
    }
    allocatorConfig_.setDefaultAllocSizes(std::move(allocSizes));
  }

  // Set hash table config
  allocatorConfig_.setAccessConfig(typename Allocator::AccessConfig{
      static_cast<uint32_t>(config_.htBucketPower),
      static_cast<uint32_t>(config_.htLockPower)});

  allocatorConfig_.configureChainedItems(typename Allocator::AccessConfig{
      static_cast<uint32_t>(config_.chainedItemHtBucketPower),
      static_cast<uint32_t>(config_.chainedItemHtLockPower)});

  allocatorConfig_.setCacheSize(config_.cacheSizeMB * (MB));

  if (config_.memoryMonitorEnabled()) {
    auto memoryMonitorConfig = config_.getMemoryMonitorConfig();
    if (!config_.memoryMonitorScript.empty()) {
      allocatorConfig_.setDelayCacheWorkersStart();
    }
    allocatorConfig_.enableMemoryMonitor(
        std::chrono::milliseconds{config_.memoryMonitorIntervalMs},
        std::move(memoryMonitorConfig));
  }

  if (!cacheDir.empty()) {
    allocatorConfig_.cacheDir = cacheDir;
  }

  if (!config_.eventTrackerFilePath.empty()) {
    if (config_.eventTrackerQueueSize == 0) {
      throw std::invalid_argument(
          "eventTrackerQueueSize must be greater than 0");
    }

    const auto eventTrackerFilePath = config_.eventTrackerFilePath;
    const auto eventTrackerSamplingRate = config_.eventTrackerSamplingRate;
    const auto eventTrackerQueueSize = config_.eventTrackerQueueSize;
    allocatorConfig_.setEventTrackerConfigFactory([eventTrackerFilePath,
                                                   eventTrackerSamplingRate,
                                                   eventTrackerQueueSize] {
      EventTracker::Config trackerConfig;
      trackerConfig.queueSize = eventTrackerQueueSize;
      trackerConfig.sampler =
          std::make_unique<FurcHashSampler>(eventTrackerSamplingRate);
      trackerConfig.eventSink =
          std::make_unique<FileEventSink>(eventTrackerFilePath);
      return trackerConfig;
    });
  }

  // Select the memory backing. When shmType is empty we keep the legacy
  // behavior (shared memory iff a cacheDir is set, POSIX iff usePosixShm);
  // otherwise shmType is authoritative and overrides usePosixShm.
  const bool useTempShm = config_.shmType == "tmp";
  if (config_.shmType.empty()) {
    if (config_.usePosixShm) {
      allocatorConfig_.usePosixForShm();
    }
  } else if (config_.shmType == "none" || useTempShm) {
    // Both heap and temp shm require an empty cacheDir; temp shm is then
    // distinguished by enabling memory monitoring below.
    allocatorConfig_.cacheDir.clear();
  } else if (config_.shmType == "sysv" || config_.shmType == "posix") {
    if (allocatorConfig_.cacheDir.empty()) {
      throw std::invalid_argument(fmt::format(
          "shmType '{}' requires a non-empty cacheDir", config_.shmType));
    }
    if (config_.shmType == "posix") {
      allocatorConfig_.usePosixForShm();
    }
  } else {
    throw std::invalid_argument(
        fmt::format("Unknown shmType '{}'", config_.shmType));
  }

  const size_t slabHugePageSize = config_.slabHugePageSize != 0
                                      ? config_.slabHugePageSize
                                      : config_.hugePageSize;
  const size_t accessHugePageSize = config_.accessHugePageSize != 0
                                        ? config_.accessHugePageSize
                                        : config_.hugePageSize;
  if (slabHugePageSize != 0 || accessHugePageSize != 0) {
    if (allocatorConfig_.isUsingPosixShm() &&
        config_.hugePageMountDir.empty()) {
      throw std::invalid_argument(
          "Using POSIX for shared memory & huge pages, but didn't specify a "
          "hugetlbfs mount");
    }
    if (slabHugePageSize != 0) {
      allocatorConfig_.enableSlabHugePages(PageSize(slabHugePageSize),
                                           config_.hugePageMountDir);
    }
    if (accessHugePageSize != 0) {
      allocatorConfig_.enableAccessHugePages(PageSize(accessHugePageSize),
                                             config_.hugePageMountDir);
    }
  }

  if (useTempShm && !config_.memoryMonitorEnabled()) {
    // Temp shm is only allocated when memory monitoring is enabled (see
    // CacheAllocator's isOnShm_). Configure a resident-memory monitor with an
    // unreachable upper limit so it never advises away slabs -- its sole
    // purpose is to route allocation through TempShmMapping.
    MemoryMonitor::Config monitorConfig;
    monitorConfig.mode = MemoryMonitor::ResidentMemory;
    monitorConfig.maxAdvisePercent = 0;
    monitorConfig.lowerLimitGB = 0;
    monitorConfig.upperLimitGB = size_t{1} << 20;
    allocatorConfig_.enableMemoryMonitor(std::chrono::seconds{1},
                                         monitorConfig);
  }

  allocatorConfig_.setMemoryLocking(config_.lockMemory);

#if !FOLLY_SANITIZE_ADDRESS
  if (config_.enableSlabAsanPoisoning) {
    throw std::invalid_argument(
        "enableSlabAsanPoisoning requires an ASAN build");
  }
#endif
  allocatorConfig_.setSlabAsanPoisoning(config_.enableSlabAsanPoisoning);

  if (!config_.memoryTierConfigs.empty()) {
    allocatorConfig_.configureMemoryTiers(config_.memoryTierConfigs);
  }

  auto cleanupGuard = folly::makeGuard([&] {
    if (!nvmCacheFilePath_.empty()) {
      util::removePath(nvmCacheFilePath_);
    }
  });

  if (config_.enableItemDestructorCheck) {
    auto removeCB = [&](const typename Allocator::DestructorData& data) {
      if (!itemRecords_.validate(data)) {
        ++invalidDestructor_;
      }
      // in the end of test, total destructor invocations must be equal to total
      // size of itemRecords_ (also is the number of new allocations)
      ++totalDestructor_;
    };
    allocatorConfig_.setItemDestructor(removeCB);
  } else if (config_.enableItemDestructor) {
    auto removeCB = [&](const typename Allocator::DestructorData&) {};
    allocatorConfig_.setItemDestructor(removeCB);
  }

  // Set up Navy
  if (!isRamOnly()) {
    typename Allocator::NvmCacheConfig nvmConfig;

    if (config_.nvmCachePaths.size() == 1) {
      // if we get a directory, create a file. we will clean it up. If we
      // already have a file, user provided it. We will also keep it around
      // after the tests.
      auto path = config_.nvmCachePaths[0];
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
        nvmCacheFilePath_ = path;
        XLOGF(INFO, "Configuring NVM cache: directory {} size {} MB", path,
              config_.nvmCacheSizeMB);
        nvmConfig.navyConfig.setSimpleFile(path + "/navy_cache",
                                           config_.nvmCacheSizeMB * MB,
                                           true /*truncateFile*/);
      } else {
        XLOGF(INFO, "Configuring NVM cache: simple file {} size {} MB", path,
              config_.nvmCacheSizeMB);
        nvmConfig.navyConfig.setSimpleFile(path, config_.nvmCacheSizeMB * MB,
                                           !isBlk /* truncateFile */);
      }
    } else if (config_.nvmCachePaths.size() > 1) {
      XLOGF(INFO, "Configuring NVM cache: RAID-0 ({} devices) size {} MB",
            config_.nvmCachePaths.size(), config_.nvmCacheSizeMB);
      // set up a software raid-0 across each nvm cache path.
      nvmConfig.navyConfig.setRaidFiles(config_.nvmCachePaths,
                                        config_.nvmCacheSizeMB * MB);
    } else {
      // use memory to mock NVM.
      XLOGF(INFO, "Configuring NVM cache: memory file size {} MB",
            config_.nvmCacheSizeMB);
      nvmConfig.navyConfig.setMemoryFile(config_.nvmCacheSizeMB * MB);
    }
    nvmConfig.navyConfig.setDeviceMetadataSize(config_.nvmCacheMetadataSizeMB *
                                               MB);

    if (config_.navyReqOrderShardsPower != 0) {
      nvmConfig.navyConfig.setNavyReqOrderingShards(
          config_.navyReqOrderShardsPower);
    }
    nvmConfig.navyConfig.setBlockSize(config_.navyBlockSize);
    nvmConfig.navyConfig.setEnableFDP(config_.deviceEnableFDP);

    // configure BlockCache
    auto& bcConfig =
        nvmConfig.navyConfig.blockCache()
            .setDataChecksum(config_.navyDataChecksum)
            .setChecksumOffload(config_.navyChecksumOffload,
                                config_.navyChecksumOffloadMinSize)
            .setChecksumOffloadReadMinSize(
                config_.navyChecksumOffloadReadMinSize)
            .setChecksumOffloadCacheControl(
                config_.navyChecksumOffloadCacheControl)
            .setChecksumOffloadBatchReclaim(
                config_.navyChecksumOffloadBatchReclaim)
            .setChecksumOffloadBatch(config_.navyChecksumOffloadBatch)
            .setChecksumDeferSmall(config_.navyChecksumDeferSmall)
            .setSkipInMemValueVerify(config_.navySkipInMemValueVerify)
            .setDirectFlush(config_.navyBlockCacheDirectFlush)
            .setFlushCopyOffload(config_.navyBlockCacheFlushCopyOffload)
            .setCleanRegions(config_.navyCleanRegions,
                             config_.navyCleanRegionThreads)
            .setRegionSize(config_.navyRegionSizeMB * MB)
            .setRegionManagerFlushAsync(config_.navyRegionManagerFlushAsync);

    // by default lru. if more than one fifo ratio is present, we use
    // segmented fifo. otherwise, simple fifo.
    const auto navyAllocatorCounts = config_.getNavyAllocatorCounts();
    if (!config_.navySegmentedFifoSegmentRatio.empty()) {
      if (config.navySegmentedFifoSegmentRatio.size() == 1) {
        bcConfig.enableFifo();
      } else {
        bcConfig.enableSegmentedFifo(config_.navySegmentedFifoSegmentRatio,
                                     navyAllocatorCounts);
      }
    }
    if (navyAllocatorCounts.size() == 1) {
      bcConfig.setAllocatorCount(navyAllocatorCounts.front());
    }

    bcConfig.enableSparseMapIndex(config_.navyNumSparseMapBuckets,
                                  config_.navyNumBucketsPerMutex,
                                  config_.navyEnableItemHistoryTracking);

    if (config_.customReinsertionPolicyFactory) {
      bcConfig.enableCustomReinsertion(config_.customReinsertionPolicyFactory);
    } else if (config_.navyHitsReinsertionThreshold > 0) {
      bcConfig.enableHitsBasedReinsertion(
          static_cast<uint8_t>(config_.navyHitsReinsertionThreshold));
    }
    if (config_.navyProbabilityReinsertionThreshold > 0) {
      bcConfig.enablePctBasedReinsertion(
          config_.navyProbabilityReinsertionThreshold);
    }

    auto& bigHashConfig = nvmConfig.navyConfig.bigHash();
    if (config_.navyArenas.empty() && config_.navyBigHashSizePct > 0) {
      bigHashConfig
          .setSizePctAndMaxItemSize(config_.navyBigHashSizePct,
                                    config_.navySmallItemMaxSize)
          .setBucketSize(config_.navyBigHashBucketSize)
          .setBucketBfSize(config_.navyBloomFilterPerBucketSize)
          .setChecksumOffload(config_.navyBigHashChecksumOffload);
    }

    const auto numArenas = config_.getNavyNumArenas();
    if (!config_.navyArenas.empty()) {
      const auto regionSize = config_.navyRegionSizeMB * MB;
      if (regionSize == 0) {
        throw std::invalid_argument(
            "navyRegionSizeMB must be greater than zero");
      }
      const auto [deviceSize, metadataSize] =
          getNavyCacheSizes(nvmConfig.navyConfig);
      if (metadataSize >= deviceSize) {
        throw std::invalid_argument(fmt::format(
            "NVM metadata size {} must be smaller than device size {}",
            metadataSize,
            deviceSize));
      }

      std::vector<unsigned int> bigHashDeviceSizePcts;
      std::vector<uint64_t> requestedBlockCacheSizes;
      std::vector<uint64_t> blockCacheSizes;
      bigHashDeviceSizePcts.reserve(numArenas);
      requestedBlockCacheSizes.reserve(numArenas);
      blockCacheSizes.reserve(numArenas);

      const auto usableSize = deviceSize - metadataSize;
      uint64_t assignedSize = 0;
      uint64_t totalBigHashSize = 0;
      for (size_t i = 0; i < numArenas; ++i) {
        const auto& arena = config_.navyArenas[i];
        const auto arenaSize = i + 1 == numArenas
                                   ? usableSize - assignedSize
                                   : usableSize * arena.sizePct / 100;
        assignedSize += arenaSize;

        const auto bigHashDeviceSizePct =
            arena.getBigHashDeviceSizePct(arenaSize, deviceSize);
        if (arena.bigHashPct > 0 && bigHashDeviceSizePct == 0) {
          throw std::invalid_argument(fmt::format(
              "Navy arena '{}' is too small to configure BigHash", arena.name));
        }

        const auto bigHashSize = deviceSize * bigHashDeviceSizePct / 100;
        if (bigHashSize >= arenaSize) {
          throw std::invalid_argument(fmt::format(
              "Navy arena '{}' is too small for its requested BigHash split",
              arena.name));
        }

        bigHashDeviceSizePcts.push_back(bigHashDeviceSizePct);
        requestedBlockCacheSizes.push_back(arenaSize - bigHashSize);
        totalBigHashSize += bigHashSize;
      }

      const auto totalBlockCacheRegions =
          (usableSize - totalBigHashSize) / regionSize;
      if (totalBlockCacheRegions < numArenas) {
        throw std::invalid_argument(
            fmt::format("NVM cache has {} BlockCache regions for {} arenas",
                        totalBlockCacheRegions,
                        numArenas));
      }

      // Round each BlockCache down to whole regions, then distribute leftover
      // regions to arenas with the largest fractional remainders.
      uint64_t assignedBlockCacheRegions = 0;
      std::vector<std::pair<uint64_t, size_t>> regionRemainders;
      regionRemainders.reserve(numArenas);
      for (size_t i = 0; i < numArenas; ++i) {
        const auto regions = requestedBlockCacheSizes[i] / regionSize;
        blockCacheSizes.push_back(regions * regionSize);
        assignedBlockCacheRegions += regions;
        regionRemainders.emplace_back(requestedBlockCacheSizes[i] % regionSize,
                                      i);
      }
      std::sort(regionRemainders.begin(),
                regionRemainders.end(),
                [](const auto& lhs, const auto& rhs) {
                  return lhs.first != rhs.first ? lhs.first > rhs.first
                                                : lhs.second < rhs.second;
                });
      for (size_t i = 0; i < totalBlockCacheRegions - assignedBlockCacheRegions;
           ++i) {
        blockCacheSizes[regionRemainders[i].second] += regionSize;
      }
      for (size_t i = 0; i < numArenas; ++i) {
        if (blockCacheSizes[i] == 0) {
          throw std::invalid_argument(
              fmt::format("Navy arena '{}' is too small for region size {}",
                          config_.navyArenas[i].name,
                          regionSize));
        }
      }

      auto commonBigHashConfig = bigHashConfig;
      commonBigHashConfig.setBucketSize(config_.navyBigHashBucketSize)
          .setBucketBfSize(config_.navyBloomFilterPerBucketSize);

      // NavyConfig owns the first engine pair, so configure it in place.
      if (bigHashDeviceSizePcts[0] > 0) {
        bigHashConfig = commonBigHashConfig;
        bigHashConfig.setSizePctAndMaxItemSize(bigHashDeviceSizePcts[0],
                                               config_.navySmallItemMaxSize);
      }
      const auto commonBlockCacheConfig = bcConfig;
      bcConfig.setSize(blockCacheSizes[0]);
      for (size_t i = 1; i < numArenas; ++i) {
        navy::EnginesConfig enginesConfig;
        enginesConfig.blockCache() = commonBlockCacheConfig;
        // A zero size tells Navy's final BlockCache to consume remaining space.
        enginesConfig.blockCache().setSize(
            i + 1 == numArenas ? 0 : blockCacheSizes[i]);
        enginesConfig.bigHash() = commonBigHashConfig;
        enginesConfig.bigHash().setSizePctAndMaxItemSize(
            bigHashDeviceSizePcts[i], config_.navySmallItemMaxSize);
        nvmConfig.navyConfig.addEnginePair(std::move(enginesConfig));
      }
      nvmConfig.navyConfig.setEnginesSelector(
          [numArenas](HashedKey key) { return key.keyHash() % numArenas; });
    }

    nvmConfig.navyConfig.setMaxParcelMemoryMB(config_.navyParcelMemoryMB);

    nvmConfig.navyConfig.setReaderAndWriterThreads(config_.navyReaderThreads,
                                                   config_.navyWriterThreads,
                                                   config_.navyMaxNumReads,
                                                   config_.navyMaxNumWrites,
                                                   config_.navyStackSizeKB);

    // Set enableIoUring (and override qDepth) if async io is enabled
    if (config_.navyMaxNumReads || config_.navyMaxNumWrites ||
        config_.navyQDepth) {
      nvmConfig.navyConfig.enableAsyncIo(config_.navyQDepth,
                                         config_.navyEnableIoUring);
    }

    if (config_.navyAdmissionWriteRateMB > 0) {
      nvmConfig.navyConfig.enableDynamicRandomAdmPolicy().setAdmWriteRate(
          config_.navyAdmissionWriteRateMB * MB);
    }
    nvmConfig.navyConfig.setMaxConcurrentInserts(
        config_.navyMaxConcurrentInserts);
    nvmConfig.navyConfig.setEnableAccessTimeMap(
        config_.navyEnableAccessTimeMap);
    nvmConfig.navyConfig.setAccessTimeMapMaxSize(
        config_.navyAccessTimeMapMaxSize);

    nvmConfig.truncateItemToOriginalAllocSizeInNvm =
        config_.truncateItemToOriginalAllocSizeInNvm;

    nvmConfig.navyConfig.setDeviceMaxWriteSize(config_.deviceMaxWriteSize);

    XLOG(INFO) << "Using the following nvm config"
               << folly::toPrettyJson(
                      folly::toDynamic(nvmConfig.navyConfig.serialize()));
    allocatorConfig_.enableNvmCache(nvmConfig);

    if (config_.navyEncryption && config_.createEncryptor) {
      allocatorConfig_.enableNvmCacheBlockEncryption(config_.createEncryptor());
    }
    if (config_.nvmAdmissionPolicyFactory) {
      try {
        nvmAdmissionPolicy_ =
            std::any_cast<std::shared_ptr<NvmAdmissionPolicy<Allocator>>>(
                config_.nvmAdmissionPolicyFactory(config_));
        allocatorConfig_.setNvmCacheAdmissionPolicy(nvmAdmissionPolicy_);
      } catch (const std::bad_any_cast& e) {
        XLOG(ERR) << "CAST ERROR " << e.what();
        throw;
      }
    } else if (config_.nvmAdmissionRetentionTimeThreshold > 0) {
      nvmAdmissionPolicy_ = std::make_shared<RetentionAP<Allocator>>(
          config_.nvmAdmissionRetentionTimeThreshold);
      allocatorConfig_.setNvmCacheAdmissionPolicy(nvmAdmissionPolicy_);
    }

    allocatorConfig_.setNvmAdmissionMinTTL(config_.memoryOnlyTTL);
  }

  allocatorConfig_.cacheName = kCachebenchCacheName;

  bool isRecovered = false;
  if (!allocatorConfig_.cacheDir.empty()) {
    try {
      cache_ = std::make_unique<Allocator>(Allocator::SharedMemAttach,
                                           allocatorConfig_);
      XLOG(INFO,
           fmt::format("Successfully attached to existing cache. Cache dir: {}",
                       cacheDir));
      isRecovered = true;
    } catch (const std::exception& ex) {
      XLOG(INFO, fmt::format("Failed to attach for reason: {}", ex.what()));
      cache_ = std::make_unique<Allocator>(Allocator::SharedMemNew,
                                           allocatorConfig_);
    }
  } else {
    cache_ = std::make_unique<Allocator>(allocatorConfig_);
  }

  if (isRecovered) {
    auto poolIds = cache_->getPoolIds();
    for (auto poolId : poolIds) {
      pools_.push_back(poolId);
    }
  } else {
    const size_t numBytes = cache_->getCacheMemoryStats().ramCacheSize;
    for (uint64_t i = 0; i < config_.numPools; ++i) {
      const double& ratio = config_.poolSizes[i];
      const size_t poolSize = static_cast<size_t>(numBytes * ratio);
      typename Allocator::MMConfig mmConfig =
          makeMMConfig<typename Allocator::MMConfig>(config_);
      const PoolId pid = cache_->addPool(
          fmt::format("pool_{}", i), poolSize, {} /* allocSizes */, mmConfig,
          nullptr /* rebalanceStrategy */, nullptr /* resizeStrategy */,
          true /* ensureSufficientMem */);
      pools_.push_back(pid);
    }
  }

  if (!config_.memoryMonitorScript.empty()) {
    memoryMonitorScript_ = std::make_unique<MemoryMonitorScript>(
        config_.memoryMonitorScript,
        std::chrono::milliseconds{config_.memoryMonitorIntervalMs},
        config_.memoryMonitorScriptRepeat);
    memoryMonitorScript_->install(
        config_.memoryMonitorMode == "resident"
            ? MemoryMonitorScript::Target::RSS
            : MemoryMonitorScript::Target::MemAvailable);
    cache_->startCacheWorkers();
  }

  if (config_.cacheMonitorFactory) {
    monitor_ = config_.cacheMonitorFactory->create(*cache_);
  }

  cleanupGuard.dismiss();
}

template <typename Allocator>
Cache<Allocator>::~Cache() {
  try {
    monitor_.reset();

    auto res = cache_->shutDown();
    if (res == Allocator::ShutDownStatus::kSuccess) {
      XLOG(INFO, fmt::format("Shut down succeeded. Metadata is at: {}",
                             allocatorConfig_.cacheDir));
    } else {
      XLOG(INFO,
           fmt::format("Shut down failed. Metadata is at: {}. Return code: {}",
                       allocatorConfig_.cacheDir, static_cast<int>(res)));
    }

    // Reset cache first which will drain all nvm operations if present
    cache_.reset();

    if (!nvmCacheFilePath_.empty()) {
      util::removePath(nvmCacheFilePath_);
    }
  } catch (...) {
  }
}

template <typename Allocator>
void Cache<Allocator>::reAttach() {
  cache_ =
      std::make_unique<Allocator>(Allocator::SharedMemAttach, allocatorConfig_);
  if (!config_.memoryMonitorScript.empty()) {
    // Preserve script progress across reattach because the script models
    // external memory pressure, which continues across cache restarts.
    cache_->startCacheWorkers();
  }
}

template <typename Allocator>
void Cache<Allocator>::shutDown() {
  monitor_.reset();
  cache_->shutDown();
}

template <typename Allocator>
void Cache<Allocator>::cleanupSharedMem() {
  if (!allocatorConfig_.cacheDir.empty()) {
    cache_->cleanupStrayShmSegments(allocatorConfig_.cacheDir,
                                    allocatorConfig_.usePosixShm,
                                    allocatorConfig_.hugePageMountDir);
    util::removePath(allocatorConfig_.cacheDir);
  }
}

template <typename Allocator>
void Cache<Allocator>::enableConsistencyCheck(
    const std::vector<std::string>& keys) {
  XDCHECK(valueTracker_ == nullptr);
  valueTracker_ =
      std::make_unique<ValueTracker>(ValueTracker::wrapStrings(keys));
  for (const std::string& key : keys) {
    invalidKeys_.emplace(key, false);
  }
}

template <typename Allocator>
typename Cache<Allocator>::RemoveRes Cache<Allocator>::remove(Key key) {
  if (!consistencyCheckEnabled()) {
    return cache_->remove(key);
  }

  auto opId = valueTracker_->beginDelete(key);
  auto rv = cache_->remove(key);
  valueTracker_->endDelete(opId);
  return rv;
}

template <typename Allocator>
typename Cache<Allocator>::RemoveRes Cache<Allocator>::remove(
    const ReadHandle& it) {
  if (!consistencyCheckEnabled()) {
    return cache_->remove(it);
  }

  auto opId = valueTracker_->beginDelete(it->getKey());
  auto rv = cache_->remove(it->getKey());
  valueTracker_->endDelete(opId);
  return rv;
}

template <typename Allocator>
typename Cache<Allocator>::WriteHandle Cache<Allocator>::allocateChainedItem(
    const ReadHandle& parent, size_t size) {
  auto handle = cache_->allocateChainedItem(parent, CacheValue::getSize(size));
  if (handle) {
    CacheValue::initialize(handle->getMemory());
  }
  return handle;
}

template <typename Allocator>
void Cache<Allocator>::addChainedItem(WriteHandle& parent, WriteHandle child) {
  itemRecords_.updateItemVersion(*parent);
  cache_->addChainedItem(parent, std::move(child));
}

template <typename Allocator>
typename Cache<Allocator>::WriteHandle Cache<Allocator>::replaceChainedItem(
    Item& oldItem, WriteHandle newItemHandle, Item& parent) {
  itemRecords_.updateItemVersion(parent);
  return cache_->replaceChainedItem(oldItem, std::move(newItemHandle), parent);
}

template <typename Allocator>
bool Cache<Allocator>::insert(WriteHandle& handle) {
  // Insert is not supported in consistency checking mode because consistency
  // checking assumes a Set always succeeds and overrides existing value.
  XDCHECK(!consistencyCheckEnabled());
  itemRecords_.addItemRecord(handle);
  return cache_->insert(handle);
}

template <typename Allocator>
typename Cache<Allocator>::WriteHandle Cache<Allocator>::allocate(
    PoolId pid, folly::StringPiece key, size_t size, uint32_t ttlSecs) {
  WriteHandle handle;
  try {
    handle = cache_->allocate(pid, key, CacheValue::getSize(size), ttlSecs);
    if (handle) {
      CacheValue::initialize(handle->getMemory());
    }
  } catch (const std::invalid_argument& e) {
    XLOGF(DBG, "Unable to allocate, reason: {}", e.what());
  }

  return handle;
}
template <typename Allocator>
typename Cache<Allocator>::WriteHandle Cache<Allocator>::insertOrReplace(
    WriteHandle& handle) {
  itemRecords_.addItemRecord(handle);

  if (!consistencyCheckEnabled()) {
    try {
      return cache_->insertOrReplace(handle);
    } catch (const cachelib::exception::RefcountOverflow& ex) {
      XLOGF(DBG, "overflow exception: {}", ex.what());
    }
  }

  auto checksum = getUint64FromItem(*handle);
  auto opId = valueTracker_->beginSet(handle->getKey(), checksum);
  auto rv = cache_->insertOrReplace(handle);
  valueTracker_->endSet(opId);
  return rv;
}

template <typename Allocator>
void Cache<Allocator>::touchValue(const ReadHandle& it) const {
  XDCHECK(touchValueEnabled());

  auto ptr = reinterpret_cast<const uint8_t*>(getMemory(it));

  /* The accumulate call is intended to access all bytes of the value
   * and nothing more. */
  auto sum = std::accumulate(ptr, ptr + getSize(it), 0ULL);
  folly::doNotOptimizeAway(sum);
}

template <typename Allocator>
bool Cache<Allocator>::couldExist(Key key) {
  if (!consistencyCheckEnabled()) {
    return cache_->couldExistFast(key);
  }

  // TODO: implement consistency checking.
  // For couldExist, we need a weaker version of consistnecy check. The
  // following are a list of conditions that wouldn't be valid with only
  // sequence of get operations.
  //  couldExist == true -> get == miss
  //  couldExist == false -> couldExist == true
  return cache_->couldExistFast(key);
}

template <typename Allocator>
typename Cache<Allocator>::ReadHandle Cache<Allocator>::find(Key key) {
  auto findFn = [&]() {
    util::LatencyTracker tracker;
    if (FLAGS_report_api_latency) {
      tracker = util::LatencyTracker(cacheFindLatency_);
    }
    // find from cache and wait for the result to be ready.
    auto it = cache_->find(key);
    it.wait();

    if (touchValueEnabled()) {
      touchValue(it);
    }

    return it;
  };

  if (!consistencyCheckEnabled()) {
    return findFn();
  }

  auto opId = valueTracker_->beginGet(key);
  auto it = findFn();
  if (checkGet(opId, it)) {
    invalidKeys_.find(key)->second.store(true, std::memory_order_relaxed);
  }
  return it;
}

template <typename Allocator>
folly::SemiFuture<typename Cache<Allocator>::ReadHandle>
Cache<Allocator>::asyncFind(Key key) {
  auto findFn = [&]() {
    util::LatencyTracker tracker;
    if (FLAGS_report_api_latency) {
      tracker = util::LatencyTracker(cacheFindLatency_);
    }
    // find from cache, don't wait for the result to be ready.
    auto it = cache_->find(key);

    if (it.isReady()) {
      if (touchValueEnabled()) {
        touchValue(it);
      }

      return std::move(it).toSemiFuture();
    }

    // if the handle is not ready, return a SemiFuture with deferValue for
    // touchValue
    return std::move(it).toSemiFuture().deferValue(
        [this, t = std::move(tracker)](auto handle) {
          if (touchValueEnabled()) {
            touchValue(handle);
          }
          return handle;
        });
  };

  if (!consistencyCheckEnabled()) {
    return findFn();
  }

  auto opId = valueTracker_->beginGet(key);
  auto sf = findFn();

  if (sf.isReady()) {
    if (checkGet(opId, sf.value())) {
      invalidKeys_.find(key)->second.store(true, std::memory_order_relaxed);
    }

    return sf;
  }

  // if the handle is not ready, return a SemiFuture with deferValue for
  // checking consistency
  return std::move(sf).deferValue(
      [this, opId = std::move(opId), key = std::move(key)](auto handle) {
        if (checkGet(opId, handle)) {
          invalidKeys_.find(key)->second.store(true, std::memory_order_relaxed);
        }

        return handle;
      });
}

template <typename Allocator>
typename Cache<Allocator>::WriteHandle Cache<Allocator>::findToWrite(Key key) {
  auto findToWriteFn = [&]() {
    util::LatencyTracker tracker;
    if (FLAGS_report_api_latency) {
      tracker = util::LatencyTracker(cacheFindLatency_);
    }
    // find from cache and wait for the result to be ready.
    auto it = cache_->findToWrite(key);
    it.wait();

    if (touchValueEnabled()) {
      touchValue(it);
    }

    return it;
  };

  if (!consistencyCheckEnabled()) {
    return findToWriteFn();
  }

  auto opId = valueTracker_->beginGet(key);
  auto it = findToWriteFn();
  if (checkGet(opId, it)) {
    invalidKeys_.find(key)->second.store(true, std::memory_order_relaxed);
  }
  return it;
}

template <typename Allocator>
bool Cache<Allocator>::checkGet(ValueTracker::Index opId,
                                const ReadHandle& it) {
  LogEventStream es;
  auto found = it != nullptr;
  uint64_t expected = 0;
  if (found) {
    if (it->hasChainedItem()) {
      expected = genHashForChain(it);
    } else {
      expected = getUint64FromItem(*it);
    }
  }
  if (!valueTracker_->endGet(opId, expected, found, &es)) {
    std::cout << (es.format() + it->toString() + "\n");
    inconsistencyCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

template <typename Allocator>
double Cache<Allocator>::getNvmBytesWritten() const {
  const auto statsMap = cache_->getNvmCacheStatsMap();
  const auto& ratesMap = statsMap.getRates();
  if (const auto& it = ratesMap.find("navy_device_bytes_written");
      it != ratesMap.end()) {
    return it->second;
  }
  XLOG(INFO) << "Bytes written not found";
  return 0;
}

template <typename Allocator>
typename Cache<Allocator>::DramIteratorSweepResult
Cache<Allocator>::runRegularDramIteratorSweep(
    const std::optional<util::Throttler::Config>& throttlerConfig) {
  if (throttlerConfig) {
    return runDramIteratorSweep(cache_->begin(*throttlerConfig), cache_->end());
  }
  return runDramIteratorSweep(cache_->begin(), cache_->end());
}

template <typename Allocator>
typename Cache<Allocator>::DramIteratorSweepResult
Cache<Allocator>::runLockGroupDramIteratorSweep(
    const std::optional<util::Throttler::Config>& throttlerConfig) {
  if (throttlerConfig) {
    return runDramIteratorSweep(cache_->beginLockGroup(*throttlerConfig),
                                cache_->endLockGroup());
  }
  return runDramIteratorSweep(cache_->beginLockGroup(), cache_->endLockGroup());
}

template <typename Allocator>
template <typename Iterator>
typename Cache<Allocator>::DramIteratorSweepResult
Cache<Allocator>::runDramIteratorSweep(Iterator iter, const Iterator& end) {
  DramIteratorSweepResult result;
  for (; iter != end; ++iter) {
    const auto& item = *iter;
    ++result.items;
    result.keyBytes += item.getKey().size();
    result.valueBytes += getSize(&item);
  }
  return result;
}

template <typename Allocator>
std::unique_ptr<StatsBase> Cache<Allocator>::getStats() const {
  auto retPtr = std::make_unique<Stats>();
  auto& ret = *retPtr;

  PoolStats aggregate = cache_->getPoolStats(pools_[0]);
  auto usageFraction =
      1.0 - (static_cast<double>(aggregate.freeMemoryBytes())) /
                aggregate.poolUsableSize;
  ret.poolUsageFraction.push_back(usageFraction);
  for (size_t pid = 1; pid < pools_.size(); pid++) {
    auto poolStats = cache_->getPoolStats(static_cast<PoolId>(pid));
    usageFraction = 1.0 - (static_cast<double>(poolStats.freeMemoryBytes())) /
                              poolStats.poolUsableSize;
    ret.poolUsageFraction.push_back(usageFraction);
    aggregate += poolStats;
  }

  std::map<PoolId, std::map<ClassId, ACStats>> allocationClassStats{};

  for (size_t pid = 0; pid < pools_.size(); pid++) {
    PoolId poolId = static_cast<PoolId>(pid);
    auto poolStats = cache_->getPoolStats(poolId);
    auto cids = poolStats.getClassIds();
    for (auto [cid, stats] : poolStats.mpStats.acStats) {
      allocationClassStats[poolId][cid] = stats;
    }
  }

  const auto cacheStats = cache_->getGlobalCacheStats();
  const auto memoryStats = cache_->getCacheMemoryStats();
  const auto rebalanceStats = cache_->getSlabReleaseStats();
  const auto navyStats = cache_->getNvmCacheStatsMap().toMap();
  for (const auto& [name, value] : cache_->getEventTrackerStatsMap()) {
    ret.eventTrackerCounters[name] = value;
  }

  ret.allocationClassStats = allocationClassStats;
  ret.numEvictions = aggregate.numEvictions();
  ret.numItems = aggregate.numItems();
  ret.evictAttempts = cacheStats.evictionAttempts;
  ret.allocAttempts = cacheStats.allocAttempts;
  ret.allocFailures = cacheStats.allocFailures;

  ret.backgndEvicStats.nEvictedItems = cacheStats.evictionStats.numMovedItems;
  ret.backgndEvicStats.nTraversals = cacheStats.evictionStats.runCount;
  ret.backgndEvicStats.nClasses = cacheStats.evictionStats.totalClasses;
  ret.backgndEvicStats.evictionSize = cacheStats.evictionStats.totalBytesMoved;

  ret.backgndPromoStats.nPromotedItems =
      cacheStats.promotionStats.numMovedItems;
  ret.backgndPromoStats.nTraversals = cacheStats.promotionStats.runCount;

  ret.numCacheGets = cacheStats.numCacheGets;
  ret.numCacheGetMiss = cacheStats.numCacheGetMiss;
  ret.numCacheEvictions = cacheStats.numCacheEvictions;
  ret.numRamDestructorCalls = cacheStats.numRamDestructorCalls;
  ret.numNvmGets = cacheStats.numNvmGets;
  ret.numNvmGetMiss = cacheStats.numNvmGetMiss;
  ret.numNvmGetCoalesced = cacheStats.numNvmGetCoalesced;
  ret.numNvmRejectsByExpiry = cacheStats.numNvmRejectsByExpiry;
  ret.numNvmRejectsByClean = cacheStats.numNvmRejectsByClean;

  ret.numNvmPuts = cacheStats.numNvmPuts;
  ret.numNvmPutErrs = cacheStats.numNvmPutErrs;
  ret.numNvmAbortedPutOnTombstone = cacheStats.numNvmAbortedPutOnTombstone;
  ret.numNvmAbortedPutOnInflightGet = cacheStats.numNvmAbortedPutOnInflightGet;
  ret.numNvmPutFromClean = cacheStats.numNvmPutFromClean;
  ret.numNvmUncleanEvict = cacheStats.numNvmUncleanEvict;
  ret.numNvmCleanEvict = cacheStats.numNvmCleanEvict;
  ret.numNvmCleanDoubleEvict = cacheStats.numNvmCleanDoubleEvict;
  ret.numNvmDestructorCalls = cacheStats.numNvmDestructorCalls;
  ret.numNvmEvictions = cacheStats.numNvmEvictions;

  ret.numNvmDeletes = cacheStats.numNvmDeletes;
  ret.numNvmSkippedDeletes = cacheStats.numNvmSkippedDeletes;

  ret.slabsReleased = rebalanceStats.numSlabReleaseForRebalance;
  ret.slabsReleasedForAdvise = rebalanceStats.numSlabReleaseForAdvise;
  ret.advisedSlabs = memoryStats.numAdvisedSlabs();
  ret.numAbortedSlabReleases = cacheStats.numAbortedSlabReleases;
  ret.numReaperSkippedSlabs = cacheStats.numReaperSkippedSlabs;
  ret.moveAttemptsForSlabRelease = rebalanceStats.numMoveAttempts;
  ret.moveSuccessesForSlabRelease = rebalanceStats.numMoveSuccesses;
  ret.evictionAttemptsForSlabRelease = rebalanceStats.numEvictionAttempts;
  ret.evictionSuccessesForSlabRelease = rebalanceStats.numEvictionSuccesses;

  ret.inconsistencyCount = getInconsistencyCount();
  ret.isNvmCacheDisabled = isNvmCacheDisabled();
  ret.invalidDestructorCount = invalidDestructor_;
  ret.unDestructedItemCount =
      static_cast<int64_t>(itemRecords_.count()) - totalDestructor_;

  ret.cacheAllocateLatencyNs = cacheStats.allocateLatencyNs;
  ret.cacheFindLatencyNs = cacheFindLatency_.estimate();
  ret.nvmLookupLatencyNs = cacheStats.nvmLookupLatencyNs;
  ret.nvmInsertLatencyNs = cacheStats.nvmInsertLatencyNs;

  // Populate counters.
  // TODO: Populate more counters that are interesting to cachebench.
  if (config_.printNvmCounters) {
    ret.nvmCounters = cache_->getNvmCacheStatsMap().toMap();
  }

  ret.backgroundEvictionClasses =
      cache_->getBackgroundMoverClassStats(MoverDir::Evict);
  ret.backgroundPromotionClasses =
      cache_->getBackgroundMoverClassStats(MoverDir::Promote);

  // nvm stats from navy
  if (!isRamOnly() && !navyStats.empty()) {
    auto lookup = [&navyStats](const std::string& key) {
      return navyStats.find(key) != navyStats.end()
                 ? static_cast<uint64_t>(navyStats.at(key))
                 : 0;
    };
    auto lookupEngineTotal = [&](const std::string& key) {
      if (config_.getNavyNumArenas() == 1) {
        return lookup(key);
      }
      uint64_t total = 0;
      for (size_t i = 0; i < config_.getNavyNumArenas(); ++i) {
        total += lookup(fmt::format("{}_{}", key, i));
      }
      return total;
    };
    ret.numNvmItems =
        lookupEngineTotal("navy_bh_items") + lookupEngineTotal("navy_bc_items");
    ret.numNvmBytesWritten = lookup("navy_device_bytes_written");
    uint64_t now = fetchNandWrites();
    if (now > nandBytesBegin_) {
      ret.numNvmNandBytesWritten = now - nandBytesBegin_;
    }
    double bhLogicalBytes = lookupEngineTotal("navy_bh_logical_written");
    double bcLogicalBytes = lookupEngineTotal("navy_bc_logical_written");
    ret.numNvmLogicalBytesWritten =
        static_cast<size_t>(bhLogicalBytes + bcLogicalBytes);
    ret.nvmReadLatencyMicrosP50 = lookup("navy_device_read_latency_us_p50");
    ret.nvmReadLatencyMicrosP90 = lookup("navy_device_read_latency_us_p90");
    ret.nvmReadLatencyMicrosP99 = lookup("navy_device_read_latency_us_p99");
    ret.nvmReadLatencyMicrosP999 = lookup("navy_device_read_latency_us_p999");
    ret.nvmReadLatencyMicrosP9999 = lookup("navy_device_read_latency_us_p9999");
    ret.nvmReadLatencyMicrosP99999 =
        lookup("navy_device_read_latency_us_p99999");
    ret.nvmReadLatencyMicrosP999999 =
        lookup("navy_device_read_latency_us_p999999");
    ret.nvmReadLatencyMicrosP100 = lookup("navy_device_read_latency_us_max");
    ret.nvmWriteLatencyMicrosP50 = lookup("navy_device_write_latency_us_p50");
    ret.nvmWriteLatencyMicrosP90 = lookup("navy_device_write_latency_us_p90");
    ret.nvmWriteLatencyMicrosP99 = lookup("navy_device_write_latency_us_p99");
    ret.nvmWriteLatencyMicrosP999 = lookup("navy_device_write_latency_us_p999");
    ret.nvmWriteLatencyMicrosP9999 =
        lookup("navy_device_write_latency_us_p9999");
    ret.nvmWriteLatencyMicrosP99999 =
        lookup("navy_device_write_latency_us_p99999");
    ret.nvmWriteLatencyMicrosP999999 =
        lookup("navy_device_write_latency_us_p999999");
    ret.nvmWriteLatencyMicrosP100 = lookup("navy_device_write_latency_us_max");
    ret.numNvmItemRemovedSetSize = lookup("items_tracked_for_destructor");

    ret.blockCacheLatencyStats =
        getBlockCacheLatencyStats(navyStats, config_.getNavyNumArenas());

    // track any non-zero check sum errors or io errors
    for (const auto& [k, v] : navyStats) {
      if (v != 0 && ((k.find("checksum_error") != std::string::npos) ||
                     (k.find("io_error") != std::string::npos))) {
        ret.nvmErrors.insert(std::make_pair(k, v));
      }
    }
  }

  return retPtr;
}

template <typename Allocator>
double Cache<Allocator>::getNvmEvictionRate() const {
  const auto nvmStats = cache_->getNvmCacheStatsMap();
  const auto& ratesMap = nvmStats.getRates();
  if (config_.getNavyNumArenas() == 1) {
    const auto it = ratesMap.find("navy_bc_evictions");
    return it == ratesMap.end() ? 0 : it->second;
  }
  double total = 0;
  for (size_t i = 0; i < config_.getNavyNumArenas(); ++i) {
    const auto it = ratesMap.find(fmt::format("navy_bc_evictions_{}", i));
    if (it != ratesMap.end()) {
      total += it->second;
    }
  }
  return total;
}

template <typename Allocator>
void Cache<Allocator>::clearCache(uint64_t errorLimit) {
  if (config_.enableItemDestructorCheck) {
    // all items leftover in the cache must be removed
    // at the end of the test to trigger ItemDestructor
    // so that we are able to check if ItemDestructor
    // are called once and only once for every item.
    auto keys = itemRecords_.getKeys();
    XLOGF(DBG, "clearCache keys: {}", keys.size());
    for (const auto& key : keys) {
      cache_->remove(key);
    }
  }

  // Drain asynchronous NVM removals before final stats validate that the
  // item-destructor ownership bookkeeping is empty.
  if (config_.enableItemDestructorCheck || config_.enableItemDestructor) {
    cache_->flushNvmCache();
  }

  if (config_.enableItemDestructorCheck) {
    itemRecords_.findUndestructedItem(std::cout, errorLimit);
  }
}

template <typename Allocator>
uint32_t Cache<Allocator>::getSize(const Item* item) const noexcept {
  if (item == nullptr) {
    return 0;
  }
  return item->template getMemoryAs<CacheValue>()->getDataSize(item->getSize());
}

template <typename Allocator>
uint64_t Cache<Allocator>::genHashForChain(const ReadHandle& handle) const {
  auto chainedAllocs = cache_->viewAsChainedAllocs(handle);
  uint64_t hash = getUint64FromItem(*handle);
  for (const auto& item : chainedAllocs.getChain()) {
    hash = folly::hash::hash_128_to_64(hash, getUint64FromItem(item));
  }
  return hash;
}

template <typename Allocator>
void Cache<Allocator>::trackChainChecksum(const ReadHandle& handle) {
  XDCHECK(consistencyCheckEnabled());
  auto checksum = genHashForChain(handle);
  auto opId = valueTracker_->beginSet(handle->getKey(), checksum);
  valueTracker_->endSet(opId);
}

template <typename Allocator>
void Cache<Allocator>::setUint64ToItem(WriteHandle& handle,
                                       uint64_t num) const {
  XDCHECK(handle);
  auto ptr = handle->template getMemoryAs<CacheValue>();
  ptr->setConsistencyNum(num);
}

template <typename Allocator>
void Cache<Allocator>::setStringItem(WriteHandle& handle,
                                     const std::string& str) {
  auto dataSize = getSize(handle);
  if (dataSize < 1) {
    return;
  }

  auto ptr = reinterpret_cast<char*>(getMemory(handle));
  // memcpy/memset instead of strncpy: strncpy scans for the terminator byte
  // by byte and is not interposed by DTO, while memcpy/memset of large
  // values can be offloaded to DSA. Like strncpy, write exactly dataSize
  // bytes: the string (truncated if needed), then a zero-filled tail.
  const size_t copyLen = std::min<size_t>(str.size() + 1, dataSize);
  std::memcpy(ptr, str.c_str(), copyLen);

  // Make sure the copied string ends with null char
  if (str.size() + 1 > dataSize) {
    ptr[dataSize - 1] = '\0';
  } else if (copyLen < dataSize) {
    std::memset(ptr + copyLen, 0, dataSize - copyLen);
  }
}

template <typename Allocator>
void Cache<Allocator>::updateItemRecordVersion(WriteHandle& it) {
  itemRecords_.updateItemVersion(*it);
  cache_->invalidateNvm(*it);
}
} // namespace facebook::cachelib::cachebench
