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

#include <any>

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/allocator/RebalanceStrategy.h"
#include "cachelib/allocator/nvmcache/BlockCacheReinsertionPolicy.h"
#include "cachelib/allocator/nvmcache/NavyConfig.h"
#include "cachelib/cachebench/util/JSONConfig.h"
#include "cachelib/cachebench/util/MemoryMonitorScript.h"
#include "cachelib/common/Ticker.h"
#include "cachelib/navy/common/Device.h"

namespace facebook {
namespace cachelib {

namespace navy {
class Index;
} // namespace navy

namespace cachebench {
// Monitor that is set up after CacheAllocator is created and
// destroyed before CacheAllocator is shut down.
class CacheMonitor {
 public:
  virtual ~CacheMonitor() = default;
};

class CacheMonitorFactory {
 public:
  virtual ~CacheMonitorFactory() = default;
  virtual std::unique_ptr<CacheMonitor> create(LruAllocator& cache) = 0;
  virtual std::unique_ptr<CacheMonitor> create(Lru2QAllocator& cache) = 0;
  virtual std::unique_ptr<CacheMonitor> create(Lru5BAllocator& cache) = 0;
  virtual std::unique_ptr<CacheMonitor> create(Lru5B2QAllocator& cache) = 0;
};

// Parse memory tiers configuration from JSON config
struct MemoryTierConfig : public JSONConfig {
  MemoryTierConfig() {}

  explicit MemoryTierConfig(const folly::dynamic& configJson);

  // Returns MemoryTierCacheConfig parsed from JSON config
  MemoryTierCacheConfig getMemoryTierCacheConfig() {
    MemoryTierCacheConfig config = MemoryTierCacheConfig::fromShm();
    config.setRatio(ratio);
    config.setMemBind(NumaBitMask(memBindNodes));
    return config;
  }

  // Specifies ratio of this memory tier to other tiers
  size_t ratio{0};
  // Allocate memory only from specified NUMA nodes
  std::string memBindNodes;
};

struct NavyArenaConfig : public JSONConfig {
  explicit NavyArenaConfig(const folly::dynamic& configJson);

  // Convert the arena-relative BigHash percentage to the device-relative
  // percentage required by NavyConfig.
  unsigned int getBigHashDeviceSizePct(uint64_t arenaSize,
                                       uint64_t deviceSize) const;

  std::string name;
  // Percentage of usable NVM capacity assigned to this arena.
  uint64_t sizePct{0};
  // Percentage of this arena assigned to BigHash. BlockCache uses the rest.
  uint64_t bigHashPct{0};
};

struct CacheConfig : public JSONConfig {
  // by defaullt, lru allocator. can be set to LRU-2Q.
  std::string allocator{"LRU"};

  // if set, we will persist the cache across cachebench runs. The directory
  // is used to store some metadata about the cache.
  std::string cacheDir;

  uint64_t cacheSizeMB{0};
  uint64_t poolRebalanceIntervalSec{0};
  std::string rebalanceStrategy;
  uint64_t rebalanceMinSlabs{1};
  uint64_t rebalanceMinDiff{100};
  double rebalanceDiffRatio{0.25};
  bool moveOnSlabRelease{false};

  uint64_t htBucketPower{22}; // buckets in hash table
  uint64_t htLockPower{20};   // locks in hash table

  // Hash table config for chained items
  uint64_t chainedItemHtBucketPower{22};
  uint64_t chainedItemHtLockPower{20};

  // time to sleep between MMContainer reconfigures
  uint64_t mmReconfigureIntervalSecs{0};

  // LRU and 2Q params
  uint64_t lruRefreshSec{60};
  double lruRefreshRatio{0.1};
  bool lruUpdateOnWrite{false};
  bool lruUpdateOnRead{true};
  bool tryLockUpdate{false};
  bool useCombinedLockForIterators{false};

  // LRU param
  uint64_t lruIpSpec{0};

  // 2Q params
  size_t lru2qHotPct{20};
  size_t lru2qColdPct{20};

  double allocFactor{1.5};
  // maximum alloc size generated using the alloc factor above.
  size_t maxAllocSize{1024 * 1024};
  size_t minAllocSize{64};

  std::vector<uint64_t> allocSizes{};

  // These specify the number of pools and how keys will
  // be distributed among the pools
  uint64_t numPools{1};
  std::vector<double> poolSizes{1.0};

  // uses a user specified file for caching. If the path specified is a file or
  // raw device, then navy uses that directly. If the path specified is a
  // directory, we will create a file inside with appropriate size. If a
  // directory is specified by user, cachebench cleans it up at exit. If it is a
  // file, cachebench preserves the file upon exit. User can also specify a
  // number of devices. Cachelib's flash cache engine (Navy) will use them in a
  // raid0 fashion
  std::vector<std::string> nvmCachePaths{};

  // size of the NVM for caching. When more than one device path is
  // specified, this is the size per device path. When this is non-zero and
  // nvmCachePaths is empty, an in-memory block device is used.
  uint64_t nvmCacheSizeMB{0};

  // if 0, use the default. Note this takes away space from NvmCache
  uint64_t nvmCacheMetadataSizeMB{0};

  // list of device identifiers for the device path that can be used to
  // monitor the physical write amplification. If empty, physical write amp is
  // not computed. This can be specified as nvme1n1 or nvme1n2 etc, confirming
  // to be a physical device identifier;
  std::vector<std::string> writeAmpDeviceList{};

  // Navy specific: block size in bytes
  uint64_t navyBlockSize{512};

  // Navy specific: region size in MB
  uint64_t navyRegionSizeMB{16};

  // Independent Navy engine pairs sharing the NVM device. If empty, cachebench
  // configures one arena using navyBigHashSizePct for backward compatibility.
  std::vector<NavyArenaConfig> navyArenas;

  size_t getNavyNumArenas() const {
    return navyArenas.empty() ? 1 : navyArenas.size();
  }

  // If non-empty, configures Navy to use FIFO instead of LRU. If there are
  // more than one values provided, it enables segmented fifo with the
  // appropriate ratios.
  std::vector<unsigned int> navySegmentedFifoSegmentRatio{};

  // Number of BlockCache allocators for each priority. 0 uses Navy's default.
  uint32_t navyAllocatorsPerPriority{0};

  // Number of shards expressed as power of two for request ordering in
  // Navy. If 0, the default configuration of Navy(20) is used.
  uint64_t navyReqOrderShardsPower{21};

  // Percentage of NVM dedicated to BigHash when navyArenas is empty.
  uint64_t navyBigHashSizePct = 50;

  // bucket size for BigHash. This controls the write amplification for small
  // objects in Navy. Every small object write performs a RMW for a bucket.
  uint64_t navyBigHashBucketSize = 4096;

  // Big Hash bloom filter size in bytes per bucket above.
  uint64_t navyBloomFilterPerBucketSize = 8;

  // Small Item Max Size determines the upper bound of an item size that
  // can be admitted into Big Hash engine.
  uint64_t navySmallItemMaxSize = 2048;

  // total memory limit for in-flight insertion operations for NVM. Once this is
  // reached, requests will be rejected until the memory usage gets under
  // the limit.
  uint64_t navyParcelMemoryMB = 1024;

  // use a hits based reinsertion policy with navy
  uint64_t navyHitsReinsertionThreshold{0};

  // use a probability based reinsertion policy with navy
  uint64_t navyProbabilityReinsertionThreshold{0};

  // Optional custom reinsertion policy factory for Navy's BlockCache.
  // When set, this takes precedence over the standard hits-based
  // reinsertion policy.
  std::function<std::shared_ptr<BlockCacheReinsertionPolicy>(
      const navy::Index&)>
      customReinsertionPolicyFactory;

  // Enable item history tracking in Navy's BlockCache index.
  // Required by reinsertion policies that use item access history.
  bool navyEnableItemHistoryTracking{false};

  // Number of bucket maps in Navy's SparseMapIndex.
  uint32_t navyNumSparseMapBuckets{
      navy::BlockCacheIndexConfig::kDefaultNumSparseMapBuckets};

  // Number of consecutive SparseMapIndex bucket maps protected by each mutex.
  uint32_t navyNumBucketsPerMutex{
      navy::BlockCacheIndexConfig::kDefaultNumBucketsPerMutex};

  // number of asynchronous worker thread for navy read operation.
  uint32_t navyReaderThreads{32};

  // number of asynchronous worker thread for navy write operation,
  uint32_t navyWriterThreads{32};

  // Max number of concurrent reads/writes in whole Navy
  uint32_t navyMaxNumReads{0};
  uint32_t navyMaxNumWrites{0};

  // Default stack size of Navy fibers when async IO is enabled
  uint32_t navyStackSizeKB{32};

  // qdepth to be used; override if already set automatically
  // by navyMaxNumReads and navyMaxNumWrites
  uint32_t navyQDepth{0};
  // Use either io_uring or libaio for async IO
  bool navyEnableIoUring{true};

  // buffer of clean regions to be maintained free to ensure writes
  // into navy don't queue behind a reclaim of region.
  uint32_t navyCleanRegions{1};

  // The number of RegionManager threads for reclaim and flush
  uint32_t navyCleanRegionThreads{1};

  // Whether RegionManager workers flush regions asynchronously.
  bool navyRegionManagerFlushAsync{false};

  // disabled when value is 0
  uint32_t navyAdmissionWriteRateMB{0};

  // maximum pending inserts before rejecting new inserts.
  uint32_t navyMaxConcurrentInserts{1000000};

  // enables data checksuming for navy. metadata checksum is enabled by
  // default
  bool navyDataChecksum{true};

  // offloads navy BlockCache data checksumming (fused with the value copy on
  // the write path) to Intel DSA via the DTO library, for values of at least
  // navyChecksumOffloadMinSize bytes. Requires navyDataChecksum and a build
  // with BUILD_WITH_DTO.
  bool navyChecksumOffload{false};
  // BlockCache flush path: write the region buffer straight to the device
  // (no intermediate copy), or do that copy on Intel DSA.
  bool navyBlockCacheDirectFlush{false};
  bool navyBlockCacheFlushCopyOffload{false};
  uint32_t navyChecksumOffloadMinSize{16384};
  // separate gate for read-side verification (0 = same as the write gate;
  // 4294967295 disables read-side offload), and the cache-control hint on the
  // fused write copy. See BlockCache::Config.
  uint32_t navyChecksumOffloadReadMinSize{0};
  bool navyChecksumOffloadCacheControl{true};
  // batched value verification in region reclaim/cleanup, fiber-coalesced
  // write batching, deferred small-value checksums, and skipping value
  // verification for in-memory hits - all default off. See BlockCache::Config.
  bool navyChecksumOffloadBatchReclaim{false};
  bool navyChecksumOffloadBatch{false};
  bool navyChecksumDeferSmall{false};
  bool navySkipInMemValueVerify{false};

  // offloads navy BigHash bucket checksumming to Intel DSA. Most beneficial
  // with large buckets (navyBigHashBucketSize of 16KB+).
  bool navyBigHashChecksumOffload{false};

  // by default, only store the size requested by the user into nvm cache
  bool truncateItemToOriginalAllocSizeInNvm = false;

  // by default, we do not encrypt content in Navy
  bool navyEncryption = false;

  // By default Navy will only flush to device at most 1MB, if larger than 1MB,
  // Navy will split it into multiple IOs.
  uint32_t deviceMaxWriteSize{1024 * 1024};

  // Enable the FDP Data placement mode in the device, if it is capable.
  bool deviceEnableFDP{false};

  // Whether to enable the AccessTimeMap for tracking NVM item access times.
  bool navyEnableAccessTimeMap{false};
  // Maximum entries in the AccessTimeMap. 0 means unbounded.
  size_t navyAccessTimeMapMaxSize{0};

  // Don't write to flash if cache TTL is smaller than this value.
  // Not used when its value is 0.  In seconds.
  uint32_t memoryOnlyTTL{0};

  // Use Posix Shm instead of SysVShm
  bool usePosixShm{false};

  // Selects the memory backing explicitly, overriding the legacy behavior of
  // inferring it from cacheDir/usePosixShm. Valid values:
  //   ""      preserve legacy behavior (shm iff cacheDir set; POSIX iff
  //           usePosixShm, otherwise SysV)
  //   "none"  heap memory, no shared memory
  //   "tmp"   ephemeral temp shared memory (always SysV)
  //   "sysv"  persistent SysV shared memory (requires cacheDir)
  //   "posix" persistent POSIX shared memory (requires cacheDir)
  std::string shmType;

  // Requests HugeTLB-backed pages for both the slab and hash-table memory.
  // Value is a kernel-supported huge-page size in bytes (e.g. 2097152 for 2MB
  // pages); 0 means normal pages. The HugeTLB pool must be reserved
  // out-of-band. The per-side fields below override this when non-zero.
  size_t hugePageSize{0};

  // Huge-page size in bytes for slab memory only; overrides hugePageSize when
  // non-zero.
  size_t slabHugePageSize{0};

  // Huge-page size in bytes for the access-container hash tables only;
  // overrides hugePageSize when non-zero.
  size_t accessHugePageSize{0};

  // Mounted hugetlbfs directory backing POSIX huge-page segments. Required when
  // combining hugePageSize with POSIX shm; ignored for SysV and temp shm.
  std::string hugePageMountDir;

  // Lock memory in the RAM
  bool lockMemory{false};

  // Poison freed slab memory so ASAN can detect use-after-free bugs.
  bool enableSlabAsanPoisoning{false};

  // Memory monitor configuration. An empty script uses system memory values.
  std::string memoryMonitorMode{"disabled"};
  uint64_t memoryMonitorIntervalMs{0};
  uint64_t memoryMonitorLowerLimitGB{10};
  uint64_t memoryMonitorUpperLimitGB{15};
  uint64_t memoryMonitorMaxAdvisePercentPerIter{5};
  uint64_t memoryMonitorMaxReclaimPercentPerIter{5};
  uint64_t memoryMonitorMaxAdvisePercent{20};
  uint64_t memoryMonitorReclaimRateLimitWindowSecs{0};
  bool memoryMonitorScriptRepeat{false};
  std::vector<MemoryMonitorScriptPhase> memoryMonitorScript;

  // Memory tiers configs
  std::vector<MemoryTierCacheConfig> memoryTierConfigs{};

  // If enabled, we will use the timestamps from the trace file in the ticker
  // so that the cachebench will observe time based on timestamps from the trace
  // instead of the system time.
  bool useTraceTimeStamp{false};

  // If enabled, when printing CacheStats, also print the NvmCounters (could be
  // spammy).
  bool printNvmCounters{false};

  // When set to positive, CacheBench use this as TimeStampTickers bucket ticks
  // For example, a value of 3600 means the threads will
  // always be processing traces from the same hour at any time.
  // When set to 0, TimeStampTicker is not used.
  uint64_t tickerSynchingSeconds{0};

  // Check if ItemDestructor is triggered properly for every item.
  // Be careful that this will keep a record of every item allocated,
  // and won't be dropped after item is removed from cache, it the size
  // is not bounded by the size of cache.
  bool enableItemDestructorCheck{false};
  // enable the ItemDestructor feature, but not check correctness,
  // this verifies whether the feature affects throughputs.
  bool enableItemDestructor{false};

  // If specified, we will not admit any item into NvmCache if their
  // eviction-age is more than this threshold. 0 means no threshold
  uint32_t nvmAdmissionRetentionTimeThreshold{0};

  // If specified, enable CacheLib's EventTracker and write sampled events to
  // this CSV file.
  std::string eventTrackerFilePath;

  // EventTracker sampling rate. 1 records every key, N records roughly 1/N of
  // keys, and 0 records no keys.
  uint32_t eventTrackerSamplingRate{1};

  // EventTracker queue size. Events are dropped when this queue is full.
  uint32_t eventTrackerQueueSize{100000};

  // CoroFiberAdapter config for FlashCacheComponent run via
  // CacheComponentStressor.
  // Number of worker threads for the CoroFiberAdapter executor
  size_t fccCoroFiberAdapterNumThreads{32};
  // Number of fibers per thread for the CoroFiberAdapter executor
  size_t fccCoroFiberAdapterFibersPerThread{4};
  // Stack size in KB for CoroFiberAdapter fibers
  uint32_t fccCoroFiberAdapterStackSizeKB{32};

  //
  // Options below are not to be populated with JSON
  //

  // User is free to implement any encryption that conforms to this API
  // If supplied, all payloads into Navy will be encrypted.
  std::function<std::shared_ptr<navy::DeviceEncryptor>()> createEncryptor;

  // User will implement a function that returns NvmAdmissionPolicy.
  // The reason we return as a std::any is because the interface is a virtual
  // template class, and the actual type is determined by @allocator in
  // this config.
  std::function<std::any(const CacheConfig&)> nvmAdmissionPolicyFactory;

  // User can implement a structure that polls stats from CacheAllocator
  // and saves the states to a backend/file/place they prefer.
  std::shared_ptr<CacheMonitorFactory> cacheMonitorFactory;

  // shared pointer of the ticker to support time stamp based cachebench
  // simulation. Stressor uses this to pass the ticker into the cache.
  std::shared_ptr<cachelib::Ticker> ticker;

  // A callback function to get the number of NVM write bytes. Stressor uses
  // this to pass it into the cache.
  std::function<double()> nvmWriteBytesCallback;

  // A nested dynamic for custom config. Customized configs can be put under
  // this field and be consumed during the initialization of the cache.
  folly::dynamic customConfigJson;

  explicit CacheConfig(const folly::dynamic& configJson);

  CacheConfig() {}

  std::shared_ptr<RebalanceStrategy> getRebalanceStrategy() const;
  std::vector<uint32_t> getNavyAllocatorCounts() const;

  bool memoryMonitorEnabled() const;
  MemoryMonitor::Config getMemoryMonitorConfig() const;
};
} // namespace cachebench
} // namespace cachelib
} // namespace facebook
