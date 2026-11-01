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

#include <chrono>
#include <memory>
#include <vector>

#include "cachelib/allocator/nvmcache/NavyConfig.h"
#include "cachelib/navy/AbstractCache.h"
#include "cachelib/navy/common/Device.h"
#include "cachelib/navy/scheduler/JobScheduler.h"

namespace facebook {
namespace cachelib {
namespace navy {
// *Proto class convention:
// Every method must be called no more than once and may throw std::exception
// (or derived) if parameters are invalid.

// Block Cache (BC) engine proto. BC is used to cache medium size objects
// (1Kb - 512Kb typically). User sets up BC parameters and passes proto
// to CacheProto::setBlockCache.
class BlockCacheProto {
 public:
  using EventTracker = EventInterface<KAllocation::Key>;

  virtual ~BlockCacheProto() = default;

  // Set cache layout. Cache will start at @baseOffset and will be @size bytes
  // on the device. @regionSize is the region size (bytes).
  virtual void setLayout(uint64_t baseOffset,
                         uint64_t size,
                         uint32_t regionSize) = 0;

  // Enable data checksumming (default: disabled)
  virtual void setChecksum(bool enable) = 0;

  // (Optional) Offload data checksumming to Intel DSA (fused with the value
  // copy on the write path) for values of at least @minSize bytes. Requires
  // checksumming enabled and a build with DTO support.
  virtual void setChecksumOffload(bool enable, uint32_t minSize) = 0;

  // (Optional) Separate size gate for read-side checksum verification;
  // 0 = same as the write gate. (Optional) Cache-control hint on the fused
  // write-path copy (default on). See BlockCache::Config.
  virtual void setChecksumOffloadReadMinSize(uint32_t minSize) = 0;
  virtual void setChecksumOffloadCacheControl(bool enable) = 0;
  // (Optional) Batched value verification in reclaim/cleanup, fiber-coalesced
  // write batching, deferred small-value checksums, skipping value
  // verification for in-memory hits - all default off. See BlockCache::Config.
  virtual void setChecksumOffloadBatchReclaim(bool enable) = 0;
  virtual void setChecksumOffloadBatch(bool enable) = 0;
  virtual void setChecksumDeferSmall(bool enable) = 0;
  virtual void setSkipInMemValueVerify(bool enable) = 0;

  // set*EvictionPolicy function family: sets eviction policy. Supports LRU,
  // LRU with deferred insert and FIFO. Must set up one of them.

  // Sets LRU eviction policy.
  virtual void setLruEvictionPolicy() = 0;

  // Sets FIFO eviction policy.
  virtual void setFifoEvictionPolicy() = 0;

  // Sets SegmentedFIFO eviction policy.
  // @segmentRatio  ratio of the size of each segment.
  virtual void setSegmentedFifoEvictionPolicy(
      std::vector<unsigned int> segmentRatio) = 0;

  // (Optional) In case of stack alloc, determines recommended size of the
  // read buffer. Must be multiple of block size.
  virtual void setReadBufferSize(uint32_t size) = 0;

  // (Optional) How many clean regions GC should (try to) maintain in the pool.
  // Default: 1
  virtual void setCleanRegionsPool(uint32_t cleanRegions,
                                   uint32_t cleanRegionThreads) = 0;

  // (Optional) Number of In memory buffers to maintain. Default: 0
  virtual void setNumInMemBuffers(uint32_t numInMemBuffers) = 0;

  // (Optional) Enable a reinsertion policy with the config.
  virtual void setReinsertionConfig(
      const BlockCacheReinsertionConfig& config) = 0;

  // (Optional) Set if the item destructor feature is enabled.
  virtual void setItemDestructorEnabled(bool itemDestructorEnabled) = 0;

  // (Optional) Set the fiber stack size of region_manager thread
  virtual void setStackSize(uint32_t stackSize) = 0;

  // (Optional) Set if the preciseRemove flag.
  virtual void setPreciseRemove(bool preciseRemove) = 0;

  // (Optional) Set if the region manager worker flush should be async.
  virtual void setRegionManagerFlushAsync(bool flushAsync) = 0;

  // (Optional) Persist and recover eviction policy ordering across restarts.
  virtual void setRecoverEvictionPolicy(bool enable) = 0;

  // (Optional) Set if direct flush without intermediate copy is enabled.
  virtual void setDirectFlush(bool enable) = 0;
  // Copy the region buffer into the flush write buffer on Intel DSA
  virtual void setFlushCopyOffload(bool enable) = 0;

  // (Optional) Set if the combined entry block is enabled.
  virtual void setUseCombinedEntryBlock(bool useCombinedEntryBlock) = 0;

  // Set number of allocators per priority.
  virtual void setNumAllocatorsPerPriority(
      std::vector<uint32_t> numAllocatorsPerPriority) = 0;

  // (Optional) Set Index related config.
  virtual void setIndexConfig(const BlockCacheIndexConfig& config) = 0;

  virtual void setLegacyEventTracker(
      const std::optional<std::reference_wrapper<LegacyEventTracker>>&
          legacyEventTracker) = 0;
};

// BigHash engine proto. BigHash is used to cache small objects (under 2KB)
// User sets up this proto object and passes it to CacheProto::setBigHash.
class BigHashProto {
 public:
  virtual ~BigHashProto() = default;

  // Set cache layout. Cache will start at @baseOffset and will be @size bytes
  // on the device. BigHash divides its device spcae into a number of fixed size
  // buckets, represented by @bucketSize. All IO happens on bucket-size
  // granularity.
  virtual void setLayout(uint64_t baseOffset,
                         uint64_t size,
                         uint32_t bucketSize) = 0;

  // Enable Bloom filter with @numHashes hash functions, each mapped into an
  // bit array of @hashTableBitSize bits.
  virtual void setBloomFilter(uint32_t numHashes,
                              uint32_t hashTableBitSize) = 0;

  // (Optional) Set number of mutexes for bucket locking as power of 2.
  // numMutexes = 1 << numMutexesPower. Default: 14 (16K mutexes)
  virtual void setNumMutexesPower(uint8_t numMutexesPower) = 0;

  // (Optional) Offload bucket checksumming to Intel DSA. Requires a build
  // with DTO support.
  virtual void setChecksumOffload(bool enable) = 0;
};

class EnginePairProto {
 public:
  virtual ~EnginePairProto() = default;
  EnginePairProto() = default;
  // Delete copy constructor
  EnginePairProto(const EnginePairProto&) = delete;
  EnginePairProto(EnginePairProto&) = delete;

  // Set up block cache engine.
  virtual void setBlockCache(std::unique_ptr<BlockCacheProto> proto) = 0;

  // Set up big hash engine.
  virtual void setBigHash(std::unique_ptr<BigHashProto> proto,
                          uint32_t smallItemMaxSize) = 0;

  virtual void setName(const std::string& name) = 0;
};

// Cache object prototype. Setup cache desired parameters and pass proto to
// @createCache function.
class CacheProto {
 public:
  virtual ~CacheProto() = default;

  // Set maximum concurrent insertions allowed in the driver.
  virtual void setMaxConcurrentInserts(uint32_t limit) = 0;

  // Set maximum parcel memory for all queues inserts. Parcel is a buffer with
  // key and value.
  virtual void setMaxParcelMemory(uint64_t limit) = 0;

  // Set whether to use write size (instead of ) for admission policy.
  virtual void setUseEstimatedWriteSize(bool useEstimatedWriteSize) = 0;

  // Sets device that engine will use.
  virtual void setDevice(std::unique_ptr<Device> device) = 0;

  // Sets Persistence Params for navy
  virtual void setPersistParams(const NavyPersistParams& persistParams) = 0;

  // Sets metadata size.
  virtual void setMetadataSize(size_t metadataSize) = 0;

  // Sets the in-memory staging size used to batch metadata writes.
  virtual void setMetadataStagingSize(size_t metadataStagingSize) = 0;

  // Sets maximum key size.
  virtual void setMaxKeySize(uint32_t keySize) = 0;

  // Set JobScheduler for async function calls.
  virtual void setJobScheduler(std::unique_ptr<JobScheduler> ex) = 0;

  virtual void addEnginePair(std::unique_ptr<EnginePairProto> proto) = 0;

  virtual void setEnginesSelector(NavyConfig::EnginesSelector selector) = 0;

  // Set callback used to if the passed NvmItem is expired
  virtual void setExpiredCheck(ExpiredCheck checkExpired) = 0;

  // (Optional) Set destructor callback.
  //   - Callback invoked exactly once for every insert, even if it was removed
  //     manually from the cache with @AbstractCache::remove.
  //   - If a key was removed manually, the DestructorEvent will be
  //     DestructorEvent::Removed. If it was evicted, DestructorEvent::Recycled
  //   - No any guarantees on order, even for a specific key.
  //   - No any guarantees on timing. If entry was removed/evicted, we only
  //     guarantee to invoke the callback at some point of time.
  //   - Callback should be lightweight.
  virtual void setDestructorCallback(DestructorCallback cb) = 0;

  // (Optional) Set admission policy to accept a random item with specified
  // probability.
  //
  // @param config  Random policy configured in nvmcache
  virtual void setRejectRandomAdmissionPolicy(const RandomAPConfig& config) = 0;
  //   double probability) = 0;

  // (Optional) Set admission policy to accept a random item with a target write
  // rate in bytes/s. setBlockCache is a dependency and must be called before
  // this.
  //
  // @param config  Dynamic Random policy configured in nvmcache
  virtual void setDynamicRandomAdmissionPolicy(
      const DynamicRandomAPConfig& config) = 0;
};

// Creates BlockCache engine prototype.
std::unique_ptr<BlockCacheProto> createBlockCacheProto();

// Creates BigHash engine prototype.
std::unique_ptr<BigHashProto> createBigHashProto();

std::unique_ptr<EnginePairProto> createEnginePairProto();

// Creates Cache object prototype.
std::unique_ptr<CacheProto> createCacheProto();

// Creates Cache object.
// @param proto   cache object prototype
std::unique_ptr<AbstractCache> createCache(std::unique_ptr<CacheProto> proto);
} // namespace navy
} // namespace cachelib
} // namespace facebook
