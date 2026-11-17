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

#include "cachelib/rust/src/cachelib.h"

#include <folly/Range.h>

#include <chrono>

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/allocator/HitsPerSlabStrategy.h"
#include "cachelib/allocator/LruTailAgeStrategy.h"
#include "cachelib/facebook/twutil/TwUtil.h"

namespace facebook {
namespace rust {
namespace cachelib {
std::unique_ptr<facebook::cachelib::CacheAdmin> make_cacheadmin(
    LruAllocator& cache, const std::string& oncall) {
  facebook::cachelib::CacheAdmin::Config adminConfig;
  adminConfig.oncall = oncall;
  return std::make_unique<facebook::cachelib::CacheAdmin>(cache, adminConfig);
}

std::unique_ptr<LruAllocator> make_lru_allocator(
    std::unique_ptr<LruAllocatorConfig> config) {
  return std::make_unique<LruAllocator>(*config);
}
std::unique_ptr<LruAllocator> make_shm_lru_allocator(
    std::unique_ptr<LruAllocatorConfig> config) {
  return std::make_unique<LruAllocator>(
      LruAllocator::SharedMemNewT::SharedMemNew, *config);
}
std::unique_ptr<LruAllocatorConfig> make_lru_allocator_config() {
  return std::make_unique<LruAllocatorConfig>();
}

std::unique_ptr<NvmCacheConfig> make_nvm_cache_config() {
  return std::make_unique<NvmCacheConfig>();
}

std::shared_ptr<facebook::cachelib::RebalanceStrategy>
make_hits_per_slab_rebalancer(double diff_ratio,
                              unsigned int min_retained_slabs,
                              unsigned int min_tail_age) {
  facebook::cachelib::HitsPerSlabStrategy::Config config;
  config.diffRatio = diff_ratio;
  config.minSlabs = min_retained_slabs;
  config.minLruTailAge = min_tail_age;
  return std::make_shared<facebook::cachelib::HitsPerSlabStrategy>(config);
}
std::shared_ptr<facebook::cachelib::RebalanceStrategy>
make_lru_tail_age_rebalancer(double age_diference_ratio,
                             unsigned int min_retained_slabs) {
  facebook::cachelib::LruTailAgeStrategy::Config config;
  config.tailAgeDifferenceRatio = age_diference_ratio;
  config.minSlabs = min_retained_slabs;
  return std::make_shared<facebook::cachelib::LruTailAgeStrategy>(config);
}
void enable_free_memory_monitor(
    LruAllocatorConfig& config,
    std::chrono::milliseconds interval,
    uint32_t advisePercentPerIteration,
    uint32_t maxAdvisePercentage,
    uint32_t lowerLimit,
    uint32_t upperLimit,
    std::shared_ptr<facebook::cachelib::RebalanceStrategy> adviseStrategy) {
  facebook::cachelib::MemoryMonitor::Config memConfig;
  memConfig.mode = facebook::cachelib::MemoryMonitor::FreeMemory;
  memConfig.maxAdvisePercentPerIter = advisePercentPerIteration;
  memConfig.maxReclaimPercentPerIter = advisePercentPerIteration;
  memConfig.lowerLimitGB = lowerLimit;
  memConfig.upperLimitGB = upperLimit;
  memConfig.maxAdvisePercent = maxAdvisePercentage;
  config.enableMemoryMonitor(interval, memConfig, adviseStrategy);
}
void enable_resident_memory_monitor(
    LruAllocatorConfig& config,
    std::chrono::milliseconds interval,
    uint32_t advisePercentPerIteration,
    uint32_t maxAdvisePercentage,
    uint32_t lowerLimit,
    uint32_t upperLimit,
    std::shared_ptr<facebook::cachelib::RebalanceStrategy> adviseStrategy) {
  facebook::cachelib::MemoryMonitor::Config memConfig;
  memConfig.mode = facebook::cachelib::MemoryMonitor::ResidentMemory;
  memConfig.maxAdvisePercentPerIter = advisePercentPerIteration;
  memConfig.maxReclaimPercentPerIter = advisePercentPerIteration;
  memConfig.lowerLimitGB = lowerLimit;
  memConfig.upperLimitGB = upperLimit;
  memConfig.maxAdvisePercent = maxAdvisePercentage;
  config.enableMemoryMonitor(interval, memConfig, adviseStrategy);
}
void set_memory_monitor_empty_pool_recovery(LruAllocatorConfig& config,
                                            bool enabled) {
  config.memMonitorConfig.enableEmptyPoolRecovery = enabled;
}
bool get_memory_monitor_empty_pool_recovery(const LruAllocatorConfig& config) {
  return config.memMonitorConfig.enableEmptyPoolRecovery;
}
void enable_pool_rebalancing(
    LruAllocatorConfig& config,
    std::shared_ptr<facebook::cachelib::RebalanceStrategy> strategy,
    std::chrono::milliseconds interval) {
  config.enablePoolRebalancing(strategy, interval);
}
void enable_pool_resizing(
    LruAllocatorConfig& config,
    std::shared_ptr<facebook::cachelib::RebalanceStrategy> strategy,
    std::chrono::milliseconds interval,
    uint32_t slabs_per_iteration) {
  config.enablePoolResizing(strategy, interval, slabs_per_iteration);
}

void set_access_config(LruAllocatorConfig& config,
                       unsigned int bucketsPower,
                       unsigned int locksPower) {
  auto accessConfig =
      LruAllocatorConfig::AccessConfig(bucketsPower, locksPower);
  config.setAccessConfig(accessConfig);
}

void enable_cache_persistence(LruAllocatorConfig& config,
                              std::string& directory) {
  config.enableCachePersistence(std::move(directory));
}

void set_base_address(LruAllocatorConfig& config, size_t addr) {
  config.slabMemoryBaseAddr = (void*)addr;
}

void enable_nvm_cache(LruAllocatorConfig& config, NvmCacheConfig& nvmConfig) {
  config.enableNvmCache(nvmConfig);
}

void set_block_size(NvmCacheConfig& config, uint64_t blockSize) {
  config.navyConfig.setBlockSize(blockSize);
}

void set_simple_file(NvmCacheConfig& config,
                     const std::string& fileName,
                     uint64_t fileSize,
                     bool truncateFile) {
  config.navyConfig.setSimpleFile(fileName, fileSize, truncateFile);
}

void set_region_size(NvmCacheConfig& config, uint32_t regionSize) {
  config.navyConfig.blockCache().setRegionSize(regionSize);
}

int8_t add_pool(const LruAllocator& cache,
                folly::StringPiece name,
                size_t size) {
  return const_cast<LruAllocator&>(cache).addPool(name, size);
}

size_t get_unreserved_size(const LruAllocator& cache) {
  return cache.getCacheMemoryStats().unReservedSize;
}

size_t get_size(const LruItemHandle& handle) { return handle->getSize(); }
const uint8_t* get_memory(const LruItemHandle& handle) {
  return static_cast<const uint8_t*>(handle->getMemory());
}
uint32_t get_ttl_secs(const LruItemHandle& handle) {
  return static_cast<const uint32_t>(handle->getConfiguredTTL().count());
}
uint8_t* get_writable_memory(LruItemHandle& handle) {
  return static_cast<uint8_t*>(handle->getMemory());
}
size_t get_item_ptr_as_offset(const LruAllocator& cache, const uint8_t* ptr) {
  return const_cast<LruAllocator&>(cache).getItemPtrAsOffset(ptr);
}

std::unique_ptr<LruItemHandle> allocate_item(const LruAllocator& cache,
                                             facebook::cachelib::PoolId id,
                                             folly::StringPiece key,
                                             uint32_t size,
                                             uint32_t ttlSecs) {
  auto item = const_cast<LruAllocator&>(cache).allocate(id, key, size, ttlSecs);
  if (item) {
    return std::make_unique<LruItemHandle>(std::move(item));
  } else {
    return std::unique_ptr<LruItemHandle>();
  }
}

bool insert_handle(const LruAllocator& cache, LruItemHandle& handle) {
  return const_cast<LruAllocator&>(cache).insert(handle);
}
void insert_or_replace_handle(const LruAllocator& cache,
                              LruItemHandle& handle) {
  const_cast<LruAllocator&>(cache).insertOrReplace(handle);
}

void remove_item(const LruAllocator& cache, folly::StringPiece key) {
  const_cast<LruAllocator&>(cache).remove(key);
}
std::unique_ptr<LruItemHandle> find_item(const LruAllocator& cache,
                                         folly::StringPiece key) {
  auto item = const_cast<LruAllocator&>(cache).find(key);
  if (item) {
    return std::make_unique<LruItemHandle>(std::move(item).toWriteHandle());
  } else {
    return std::unique_ptr<LruItemHandle>();
  }
}
size_t get_pool_size(const LruAllocator& cache, facebook::cachelib::PoolId id) {
  return cache.getPool(id).getPoolSize();
}
bool grow_pool(const LruAllocator& cache,
               facebook::cachelib::PoolId id,
               size_t size) {
  return const_cast<LruAllocator&>(cache).growPool(id, size);
}
bool shrink_pool(const LruAllocator& cache,
                 facebook::cachelib::PoolId id,
                 size_t size) {
  return const_cast<LruAllocator&>(cache).shrinkPool(id, size);
}
bool resize_pools(const LruAllocator& cache,
                  facebook::cachelib::PoolId src,
                  facebook::cachelib::PoolId dst,
                  size_t size) {
  return const_cast<LruAllocator&>(cache).resizePools(src, dst, size);
}
} // namespace cachelib
} // namespace rust
} // namespace facebook
