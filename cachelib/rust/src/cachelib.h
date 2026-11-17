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

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/allocator/RebalanceStrategy.h"
#include "cachelib/facebook/admin/CacheAdmin.h"

namespace facebook {
namespace rust {
namespace cachelib {
using LruAllocator = facebook::cachelib::LruAllocator;
using LruAllocatorConfig = LruAllocator::Config;
using LruItemHandle = LruAllocator::WriteHandle;
using NvmCacheConfig = LruAllocator::NvmCacheConfig;

std::unique_ptr<facebook::cachelib::CacheAdmin> make_cacheadmin(
    LruAllocator& cache, const std::string& oncall);
std::unique_ptr<LruAllocator> make_lru_allocator(
    std::unique_ptr<LruAllocatorConfig> config);
std::unique_ptr<LruAllocator> make_shm_lru_allocator(
    std::unique_ptr<LruAllocatorConfig> config);
std::unique_ptr<LruAllocatorConfig> make_lru_allocator_config();

std::unique_ptr<NvmCacheConfig> make_nvm_cache_config();

bool enable_container_memory_monitor(LruAllocatorConfig& config);

void enable_nvm_cache(LruAllocatorConfig& config, NvmCacheConfig& nvmConfig);

std::shared_ptr<facebook::cachelib::RebalanceStrategy>
make_hits_per_slab_rebalancer(double diff_ratio,
                              unsigned int min_retained_slabs,
                              unsigned int min_tail_age);
std::shared_ptr<facebook::cachelib::RebalanceStrategy>
make_lru_tail_age_rebalancer(double age_diference_ratio,
                             unsigned int min_retained_slabs);

void enable_free_memory_monitor(
    LruAllocatorConfig& config,
    std::chrono::milliseconds interval,
    uint32_t advisePercentPerIteration,
    uint32_t maxAdvisePercentage,
    uint32_t lowerLimit,
    uint32_t upperLimit,
    std::shared_ptr<facebook::cachelib::RebalanceStrategy> adviseStrategy);
void enable_resident_memory_monitor(
    LruAllocatorConfig& config,
    std::chrono::milliseconds interval,
    uint32_t advisePercentPerIteration,
    uint32_t maxAdvisePercentage,
    uint32_t lowerLimit,
    uint32_t upperLimit,
    std::shared_ptr<facebook::cachelib::RebalanceStrategy> adviseStrategy);

void set_memory_monitor_empty_pool_recovery(LruAllocatorConfig& config,
                                            bool enabled);
bool get_memory_monitor_empty_pool_recovery(const LruAllocatorConfig& config);

void enable_pool_rebalancing(
    LruAllocatorConfig& config,
    std::shared_ptr<facebook::cachelib::RebalanceStrategy> strategy,
    std::chrono::milliseconds interval);
void enable_pool_resizing(
    LruAllocatorConfig& config,
    std::shared_ptr<facebook::cachelib::RebalanceStrategy> strategy,
    std::chrono::milliseconds interval,
    uint32_t slabs_per_iteration);

void set_access_config(LruAllocatorConfig& config,
                       unsigned int bucketsPower,
                       unsigned int locksPower);

void enable_cache_persistence(LruAllocatorConfig& config,
                              std::string& directory);

void set_base_address(LruAllocatorConfig& config, size_t addr);

void set_block_size(NvmCacheConfig& config, uint64_t blockSize);

void set_simple_file(NvmCacheConfig& config,
                     const std::string& fileName,
                     uint64_t fileSize,
                     bool truncateFile);

void set_region_size(NvmCacheConfig& config, uint32_t regionSize);

int8_t add_pool(const LruAllocator& cache,
                folly::StringPiece name,
                size_t size);
size_t get_unreserved_size(const LruAllocator& cache);

size_t get_size(const LruItemHandle& handle);
const uint8_t* get_memory(const LruItemHandle& handle);
uint32_t get_ttl_secs(const LruItemHandle& handle);

uint8_t* get_writable_memory(LruItemHandle& handle);
size_t get_item_ptr_as_offset(const LruAllocator& cache, const uint8_t* ptr);

std::unique_ptr<LruItemHandle> allocate_item(const LruAllocator& cache,
                                             facebook::cachelib::PoolId id,
                                             folly::StringPiece key,
                                             uint32_t size,
                                             uint32_t ttlSecs);

bool insert_handle(const LruAllocator& cache, LruItemHandle& handle);
void insert_or_replace_handle(const LruAllocator& cache, LruItemHandle& handle);

void remove_item(const LruAllocator& cache, folly::StringPiece key);
std::unique_ptr<LruItemHandle> find_item(const LruAllocator& cache,
                                         folly::StringPiece key);
size_t get_pool_size(const LruAllocator& cache, facebook::cachelib::PoolId id);
bool grow_pool(const LruAllocator& cache,
               facebook::cachelib::PoolId id,
               size_t size);
bool shrink_pool(const LruAllocator& cache,
                 facebook::cachelib::PoolId id,
                 size_t size);
bool resize_pools(const LruAllocator& cache,
                  facebook::cachelib::PoolId src,
                  facebook::cachelib::PoolId dst,
                  size_t size);
} // namespace cachelib
} // namespace rust
} // namespace facebook
