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

#![feature(once_cell_try)]

pub mod bincode_cache;
mod errors;
mod lrucache;

pub use crate::errors::*;
pub use crate::lrucache::*;

#[cxx::bridge(namespace = "facebook::rust::cachelib")]
mod ffi {
    #[namespace = "std::chrono"]
    extern "C++" {
        type milliseconds = stdchrono::milliseconds;
    }

    #[namespace = "folly"]
    extern "C++" {
        type StringPiece<'a> = folly::StringPiece<'a>;
    }

    unsafe extern "C++" {
        include!("cachelib/rust/src/cachelib.h");

        #[namespace = "facebook::cachelib"]
        type CacheAdmin;
        fn make_cacheadmin(
            cache: Pin<&mut LruAllocator>,
            oncall: &CxxString,
        ) -> Result<UniquePtr<CacheAdmin>>;

        type LruAllocator;
        fn make_lru_allocator(
            config: UniquePtr<LruAllocatorConfig>,
        ) -> Result<UniquePtr<LruAllocator>>;
        fn make_shm_lru_allocator(
            config: UniquePtr<LruAllocatorConfig>,
        ) -> Result<UniquePtr<LruAllocator>>;

        type NvmCacheConfig;
        fn make_nvm_cache_config() -> Result<UniquePtr<NvmCacheConfig>>;

        type LruAllocatorConfig;
        fn make_lru_allocator_config() -> Result<UniquePtr<LruAllocatorConfig>>;
        fn setCacheSize(
            self: Pin<&mut LruAllocatorConfig>,
            size: usize,
        ) -> Pin<&mut LruAllocatorConfig>;
        fn enable_container_memory_monitor(config: Pin<&mut LruAllocatorConfig>) -> Result<bool>;

        #[namespace = "facebook::cachelib"]
        type RebalanceStrategy;
        fn make_hits_per_slab_rebalancer(
            diff_ratio: f64,
            min_retained_slabs: u32,
            min_tail_age: u32,
        ) -> Result<SharedPtr<RebalanceStrategy>>;
        fn make_lru_tail_age_rebalancer(
            age_difference_ratio: f64,
            min_retained_slabs: u32,
        ) -> Result<SharedPtr<RebalanceStrategy>>;

        fn enable_nvm_cache(
            config: Pin<&mut LruAllocatorConfig>,
            nvm_config: Pin<&mut NvmCacheConfig>,
        ) -> Result<()>;

        fn enable_free_memory_monitor(
            config: Pin<&mut LruAllocatorConfig>,
            interval: milliseconds,
            advisePercentPerIteration: u32,
            maxAdvisePercentage: u32,
            lowerLimit: u32,
            upperLimit: u32,
            adviseStrategy: SharedPtr<RebalanceStrategy>,
        ) -> Result<()>;
        fn enable_resident_memory_monitor(
            config: Pin<&mut LruAllocatorConfig>,
            interval: milliseconds,
            advisePercentPerIteration: u32,
            maxAdvisePercentage: u32,
            lowerLimit: u32,
            upperLimit: u32,
            adviseStrategy: SharedPtr<RebalanceStrategy>,
        ) -> Result<()>;
        fn set_memory_monitor_empty_pool_recovery(
            config: Pin<&mut LruAllocatorConfig>,
            enabled: bool,
        ) -> Result<()>;
        fn get_memory_monitor_empty_pool_recovery(config: &LruAllocatorConfig) -> bool;

        fn enable_pool_rebalancing(
            config: Pin<&mut LruAllocatorConfig>,
            strategy: SharedPtr<RebalanceStrategy>,
            interval: milliseconds,
        ) -> Result<()>;
        fn enable_pool_resizing(
            config: Pin<&mut LruAllocatorConfig>,
            strategy: SharedPtr<RebalanceStrategy>,
            interval: milliseconds,
            slabs_per_iteration: u32,
        ) -> Result<()>;

        fn set_access_config(
            config: Pin<&mut LruAllocatorConfig>,
            bucketsPower: u32,
            locksPower: u32,
        ) -> Result<()>;

        fn set_base_address(config: Pin<&mut LruAllocatorConfig>, addr: usize) -> Result<()>;

        fn setCacheName<'a>(
            self: Pin<&'a mut LruAllocatorConfig>,
            name: &CxxString,
        ) -> Pin<&'a mut LruAllocatorConfig>;
        fn enable_cache_persistence(
            config: Pin<&mut LruAllocatorConfig>,
            directory: Pin<&mut CxxString>,
        );

        fn set_block_size(config: Pin<&mut NvmCacheConfig>, size: u64) -> Result<()>;

        fn set_simple_file(
            config: Pin<&mut NvmCacheConfig>,
            file_name: &CxxString,
            file_size: u64,
            truncate_file: bool,
        ) -> Result<()>;

        fn set_region_size(config: Pin<&mut NvmCacheConfig>, region_size: u32) -> Result<()>;

        fn add_pool(cache: &LruAllocator, name: StringPiece<'_>, size: usize) -> Result<i8>;
        fn get_unreserved_size(cache: &LruAllocator) -> usize;

        type LruItemHandle;
        fn get_size(handle: &LruItemHandle) -> usize;
        fn get_memory(handle: &LruItemHandle) -> *const u8;
        #[allow(dead_code)]
        fn get_ttl_secs(handle: &LruItemHandle) -> u32;
        fn get_writable_memory(handle: Pin<&mut LruItemHandle>) -> Result<*mut u8>;
        unsafe fn get_item_ptr_as_offset(cache: &LruAllocator, ptr: *const u8) -> Result<usize>;

        fn allocate_item(
            cache: &LruAllocator,
            id: i8,
            key: StringPiece<'_>,
            size: u32,
            ttl_secs: u32,
        ) -> Result<UniquePtr<LruItemHandle>>;

        fn insert_handle(cache: &LruAllocator, handle: Pin<&mut LruItemHandle>) -> Result<bool>;
        fn insert_or_replace_handle(
            cache: &LruAllocator,
            handle: Pin<&mut LruItemHandle>,
        ) -> Result<()>;

        fn remove_item(cache: &LruAllocator, key: StringPiece<'_>) -> Result<()>;
        fn find_item(
            cache: &LruAllocator,
            key: StringPiece<'_>,
        ) -> Result<UniquePtr<LruItemHandle>>;

        fn get_pool_size(cache: &LruAllocator, pool: i8) -> Result<usize>;
        fn grow_pool(cache: &LruAllocator, pool: i8, size: usize) -> Result<bool>;
        fn shrink_pool(cache: &LruAllocator, pool: i8, size: usize) -> Result<bool>;
        fn resize_pools(cache: &LruAllocator, src: i8, dst: i8, size: usize) -> Result<bool>;
    }
}

/// The C++ implementation of LruAllocator is thread safe.
unsafe impl Send for ffi::LruAllocator {}
unsafe impl Sync for ffi::LruAllocator {}

/// The C++ implementation of CacheAdmin is thread safe.
unsafe impl Send for ffi::CacheAdmin {}
unsafe impl Sync for ffi::CacheAdmin {}
