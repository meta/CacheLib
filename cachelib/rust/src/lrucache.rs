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

use std::collections::HashMap;
use std::io;
use std::io::Cursor;
use std::io::Read;
use std::io::Write;
use std::marker::PhantomData;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::path::PathBuf;
use std::pin::Pin;
use std::slice;
use std::sync::Mutex;
use std::sync::OnceLock;
use std::sync::RwLock;
use std::time::Duration;

use anyhow::Context;
use anyhow::Error;
use anyhow::Result;
use bytes::Buf;
use bytes::BufMut;
use bytes::Bytes;
use bytes::BytesMut;
use bytes::buf::UninitSlice;
use cxx::let_cxx_string;
use fbinit::FacebookInit;
use folly::StringPiece;

use crate::errors::ErrorKind;
use crate::ffi;

#[derive(Copy, Clone)]
pub enum RebalanceStrategy {
    /// Release slabs which aren't hit often
    HitsPerSlab {
        /// Relative improvement in hit ratio needed to choose an eviction.
        /// E.g. with diff_ratio of 0.1, a victim with a 0.8 hit ratio will only be removed if it
        /// can give memory to a slab with a hit ratio of at least 0.88. Increase diff_ratio to 0.2,
        /// and the slab receiving memory must have a hit ratio of at least 0.96.
        diff_ratio: f64,
        /// Minimum number of slabs to retain in each class.
        min_retained_slabs: u32,
        /// Minimum age (in seconds) of the oldest item in a class; if all items in a class are
        /// newer than this, leave it alone
        min_tail_age: Duration,
    },
    /// Release slabs to free up the least recently used items
    LruTailAge {
        /// How much older than average the oldest item needs to be to make a slab eligible for
        /// removal
        age_difference_ratio: f64,
        /// Minimum number of slabs to retain in each class.
        min_retained_slabs: u32,
    },
}

impl RebalanceStrategy {
    fn get_strategy(&self) -> Result<cxx::SharedPtr<ffi::RebalanceStrategy>> {
        match self {
            RebalanceStrategy::HitsPerSlab {
                diff_ratio,
                min_retained_slabs,
                min_tail_age,
            } => ffi::make_hits_per_slab_rebalancer(
                *diff_ratio,
                *min_retained_slabs,
                min_tail_age.as_secs() as u32,
            ),
            RebalanceStrategy::LruTailAge {
                age_difference_ratio,
                min_retained_slabs,
            } => ffi::make_lru_tail_age_rebalancer(*age_difference_ratio, *min_retained_slabs),
        }
        .map_err(Error::from)
    }
}

pub enum ShrinkMonitorType {
    /// Shrink the cache in response to the system running low on memory, and expand when there
    /// is plenty of free memory. This is meant for services running on bare metal
    FreeMemory {
        /// Once this much memory (in bytes) is free, start growing the cache back to the
        /// configured size.
        max_free_mem_gib: u32,
        /// If free memory drops this low, shrink the cache to allow the system to keep working
        min_free_mem_gib: u32,
    },
    /// Shrink the cache in response to the process becoming large, and expand when the process
    /// shrinks again. This is recommended for containers where you know the limits of the container
    ResidentSize {
        /// Once the process is this large, shrink the cache to allow the system to keep working
        max_process_size_gib: u32,
        /// When the process falls to this size, expand the cache back to the configured size
        min_process_size_gib: u32,
    },
}

pub struct ShrinkMonitor {
    /// Which shrinker type to use
    pub shrinker_type: ShrinkMonitorType,
    /// Seconds between monitor checks
    pub interval: Duration,
    /// How quickly to resize the cache, as a percentage of the difference between min_free_mem_gib
    /// and max_free_mem_gib.
    pub max_resize_per_iteration_percent: u32,
    /// How much of the total cache size can be removed to keep the cache size under control
    pub max_removed_percent: u32,
    /// What strategy to use when shrinking or growing the cache
    pub strategy: RebalanceStrategy,
}

pub struct PoolRebalanceConfig {
    /// How often to check for underutilized cache slabs (seconds)
    pub interval: Duration,
    /// What strategy to use for finding underutilized slabs
    pub strategy: RebalanceStrategy,
}

pub struct PoolResizeConfig {
    /// How often to check for slabs to remove from a pool (seconds)
    pub interval: Duration,
    /// How many slabs to move per iteration
    pub slabs_per_iteration: u32,
    /// What strategy to use for finding underutilized slabs
    pub strategy: RebalanceStrategy,
}

pub enum ShrinkerType {
    /// No shrinker - the cache is fixed size
    NoShrinker,
    /// Container-aware shrinking that aims to keep the cache within container limits.
    Container,
    /// Manually configured shrinking
    Config(ShrinkMonitor),
}

struct AccessConfig {
    bucket_power: u32,
    lock_power: u32,
}

/// NvmDevice configuration options.
#[allow(dead_code)]
pub enum NvmDevice {
    SimpleFile {
        file_name: PathBuf,
        file_size: u64,
        truncate: bool,
    },
    DefaultDevice,
}

/// NvmCache configuration options.
pub struct NvmCacheConfig {
    pub block_size: u64,
    pub region_size: u32,
    pub device: NvmDevice,
}

/// LRU cache configuration options.
pub struct LruCacheConfig {
    size: usize,
    shrinker: ShrinkerType,
    empty_pool_recovery: bool,
    cache_name: Option<String>,
    pool_rebalance: Option<PoolRebalanceConfig>,
    pool_resize: Option<PoolResizeConfig>,
    access_config: Option<AccessConfig>,
    cache_directory: Option<PathBuf>,
    base_address: Option<*mut std::ffi::c_void>,
    nvm_cache_config: Option<NvmCacheConfig>,
}

impl LruCacheConfig {
    /// Configure an LRU cache that will evict elements to stay below `size` bytes
    pub fn new(size: usize) -> Self {
        Self {
            size,
            shrinker: ShrinkerType::NoShrinker,
            empty_pool_recovery: false,
            cache_name: None,
            pool_rebalance: None,
            pool_resize: None,
            access_config: None,
            cache_directory: None,
            base_address: None,
            nvm_cache_config: None,
        }
    }

    /// Enable NvmCache with the given configuration.
    pub fn set_nvm_cache_config(mut self, config: NvmCacheConfig) -> Self {
        self.nvm_cache_config = Some(config);
        self
    }

    /// Enables the container shrinker if running in a supported container runtime.
    /// So far, this only works for Facebook internal containers
    pub fn set_container_shrinker(mut self) -> Self {
        self.shrinker = ShrinkerType::Container;
        self
    }

    /// Enable automatic shrinking with the chosen `ShrinkMonitor` settings
    pub fn set_shrinker(mut self, shrinker: ShrinkMonitor) -> Self {
        self.shrinker = ShrinkerType::Config(shrinker);
        self
    }

    /// Let the shrinker from `set_shrinker` grow back pools it has emptied: under low memory
    /// pressure it reclaims the slabs it actually advised away, even when every monitored pool
    /// is empty. Off by default. It has no effect on the container shrinker or without a shrinker.
    pub fn set_empty_pool_recovery(mut self, enabled: bool) -> Self {
        self.empty_pool_recovery = enabled;
        self
    }

    /// Enable pool rebalancing that evicts items early to increase overall cache utilization
    pub fn set_pool_rebalance(mut self, rebalancer: PoolRebalanceConfig) -> Self {
        self.pool_rebalance = Some(rebalancer);
        self
    }

    /// Enable pool reiszing at runtime.
    pub fn set_pool_resizer(mut self, resizer: PoolResizeConfig) -> Self {
        self.pool_resize = Some(resizer);
        self
    }

    /// Set access config parameters. Both parameters are log2 of the real value - consult cachelib
    /// C++ documentation for more details.
    /// lock_power is number of RW mutexes (small critical sections). 10 should be reasonable for
    /// millions of lookups per second (it represents 1024 locks).
    /// bucket_power is number of buckets in the hash table. You should aim to set this to
    /// log2(num elements in cache) + 1 - e.g. for 1 million items, 21 is appropriate (2,097,152
    /// buckets), while for 1 billion items, 31 is appropriate.
    /// Both values cap out at 32.
    pub fn set_access_config(mut self, bucket_power: u32, lock_power: u32) -> Self {
        self.access_config = Some(AccessConfig {
            bucket_power,
            lock_power,
        });
        self
    }

    /// Set cache name
    pub fn set_cache_name(mut self, cache_name: &str) -> Self {
        self.cache_name = Some(cache_name.to_string());
        self
    }

    /// Set cache directory.
    /// This disables volatile pools to reduce the risk of accidentally saving something
    /// whose layout is not stable
    pub fn set_cache_dir(mut self, cache_directory: impl Into<PathBuf>) -> Self {
        self.cache_directory = Some(cache_directory.into());
        self
    }

    /// Get cache directory.
    pub fn get_cache_dir(&self) -> Option<&Path> {
        self.cache_directory.as_deref()
    }

    /// Set the address at which the cache will be mounted.
    pub fn set_base_address(mut self, addr: *mut std::ffi::c_void) -> Self {
        self.base_address = Some(addr);
        self
    }

    /// Get the mounting address for the cache.
    pub fn get_base_address(&self) -> Option<*mut std::ffi::c_void> {
        self.base_address
    }
}

fn configure_shrinker(
    mut cache_config: Pin<&mut ffi::LruAllocatorConfig>,
    shrinker: ShrinkerType,
    empty_pool_recovery: bool,
) -> Result<()> {
    match shrinker {
        ShrinkerType::NoShrinker => {}
        ShrinkerType::Container => {
            if !ffi::enable_container_memory_monitor(cache_config)? {
                return Err(ErrorKind::NotInContainer.into());
            }
        }
        ShrinkerType::Config(shrinker) => {
            let rebalancer = shrinker.strategy.get_strategy()?;
            match shrinker.shrinker_type {
                ShrinkMonitorType::FreeMemory {
                    max_free_mem_gib,
                    min_free_mem_gib,
                } => {
                    ffi::enable_free_memory_monitor(
                        cache_config.as_mut(),
                        shrinker.interval.into(),
                        shrinker.max_resize_per_iteration_percent,
                        shrinker.max_removed_percent,
                        min_free_mem_gib,
                        max_free_mem_gib,
                        rebalancer,
                    )?;
                }
                ShrinkMonitorType::ResidentSize {
                    max_process_size_gib,
                    min_process_size_gib,
                } => {
                    ffi::enable_resident_memory_monitor(
                        cache_config.as_mut(),
                        shrinker.interval.into(),
                        shrinker.max_resize_per_iteration_percent,
                        shrinker.max_removed_percent,
                        min_process_size_gib,
                        max_process_size_gib,
                        rebalancer,
                    )?;
                }
            }
            ffi::set_memory_monitor_empty_pool_recovery(
                cache_config.as_mut(),
                empty_pool_recovery,
            )?;
            if empty_pool_recovery {
                debug_assert!(ffi::get_memory_monitor_empty_pool_recovery(&cache_config));
            }
        }
    };
    Ok(())
}

static GLOBAL_CACHE: OnceLock<LruCache> = OnceLock::new();

struct LruCache {
    pools: Mutex<HashMap<String, LruCachePool>>,
    admin: cxx::UniquePtr<ffi::CacheAdmin>,
    cache: cxx::UniquePtr<ffi::LruAllocator>,
    is_volatile: RwLock<bool>,
}

impl LruCache {
    fn new() -> Self {
        Self {
            cache: cxx::UniquePtr::null(),
            admin: cxx::UniquePtr::null(),
            pools: HashMap::new().into(),
            is_volatile: RwLock::new(true),
        }
    }

    fn init_cache_once(&mut self, _fb: FacebookInit, config: LruCacheConfig) -> Result<()> {
        let mut cache_config = ffi::make_lru_allocator_config()?;

        cache_config.pin_mut().setCacheSize(config.size);

        configure_shrinker(
            cache_config.pin_mut(),
            config.shrinker,
            config.empty_pool_recovery,
        )?;

        if let Some(pool_rebalance) = config.pool_rebalance {
            let rebalancer = pool_rebalance.strategy.get_strategy()?;
            ffi::enable_pool_rebalancing(
                cache_config.pin_mut(),
                rebalancer,
                pool_rebalance.interval.into(),
            )?;
        }
        if let Some(pool_resize) = config.pool_resize {
            let rebalancer = pool_resize.strategy.get_strategy()?;
            ffi::enable_pool_resizing(
                cache_config.pin_mut(),
                rebalancer,
                pool_resize.interval.into(),
                pool_resize.slabs_per_iteration,
            )?;
        }

        if let Some(AccessConfig {
            bucket_power,
            lock_power,
        }) = config.access_config
        {
            ffi::set_access_config(cache_config.pin_mut(), bucket_power, lock_power)?;
        }

        if let Some(cache_name) = config.cache_name {
            let_cxx_string!(name = cache_name);
            cache_config.pin_mut().setCacheName(&name);
        }

        if let Some(addr) = config.base_address {
            ffi::set_base_address(cache_config.pin_mut(), addr as usize)?;
        }

        if let Some(nvm_config) = config.nvm_cache_config {
            let mut nvm_cache_config = ffi::make_nvm_cache_config()?;
            ffi::set_block_size(nvm_cache_config.pin_mut(), nvm_config.block_size)?;
            if let NvmDevice::SimpleFile {
                file_name,
                file_size,
                truncate,
            } = nvm_config.device
            {
                let_cxx_string!(name = file_name.as_os_str().as_bytes());
                ffi::set_simple_file(nvm_cache_config.pin_mut(), &name, file_size, truncate)?;
            }
            ffi::set_region_size(nvm_cache_config.pin_mut(), nvm_config.region_size)?;
            ffi::enable_nvm_cache(cache_config.pin_mut(), nvm_cache_config.pin_mut())?;
        }

        if let Some(cache_directory) = config.cache_directory {
            // If cache directory is enabled, create a persistent shared-memory cache.
            let_cxx_string!(cache_directory = cache_directory.as_os_str().as_bytes());
            ffi::enable_cache_persistence(cache_config.pin_mut(), cache_directory);
            self.cache = ffi::make_shm_lru_allocator(cache_config)?;
        } else {
            let mut is_volatile = self.is_volatile.write().expect("lock poisoned");
            *is_volatile = true;
            self.cache = ffi::make_lru_allocator(cache_config)?;
        }

        if self.cache.is_null() {
            Err(ErrorKind::CacheNotInitialized.into())
        } else {
            Ok(())
        }
    }

    fn init_cacheadmin(&mut self, oncall: &str) -> Result<()> {
        let_cxx_string!(oncall = oncall);

        self.admin = ffi::make_cacheadmin(self.cache.pin_mut(), &oncall)?;

        Ok(())
    }

    fn get_allocator(&self) -> Result<&ffi::LruAllocator> {
        self.cache
            .as_ref()
            .ok_or_else(|| ErrorKind::CacheNotInitialized.into())
    }

    fn is_volatile(&self) -> bool {
        *self.is_volatile.read().expect("lock poisoned")
    }

    fn get_available_space(&self) -> Result<usize> {
        Ok(ffi::get_unreserved_size(self.get_allocator()?))
    }

    fn get_pool(&self, pool_name: &str) -> Option<LruCachePool> {
        let pools = self.pools.lock().expect("lock poisoned");

        pools.get(pool_name).cloned()
    }

    fn get_or_create_pool(&self, pool_name: &str, pool_bytes: usize) -> Result<LruCachePool> {
        let mut pools = self.pools.lock().expect("lock poisoned");

        pools.get(pool_name).cloned().ok_or(()).or_else(move |_| {
            let pool = self.create_pool(pool_name, pool_bytes)?;
            pools.insert(pool_name.to_string(), pool.clone());
            Ok(pool)
        })
    }

    fn create_pool(&self, pool_name: &str, pool_bytes: usize) -> Result<LruCachePool> {
        let name = StringPiece::from(pool_name);

        let pool = ffi::add_pool(self.get_allocator()?, name, pool_bytes)?;

        Ok(LruCachePool {
            pool,
            pool_name: pool_name.to_string(),
        })
    }
}

/// Initialise the LRU cache based on the supplied config. This should be called once and once
/// only per execution
pub fn init_cache(fb: FacebookInit, config: LruCacheConfig) -> Result<()> {
    GLOBAL_CACHE
        .get_or_try_init(|| {
            let mut cache = LruCache::new();
            cache.init_cache_once(fb, config)?;
            Ok(cache)
        })
        .map(|_| ())
}

/// Initialise the LRU cache based on the supplied config, and start CacheAdmin.
/// This should be called once and once only per execution
pub fn init_cache_with_cacheadmin(
    fb: FacebookInit,
    config: LruCacheConfig,
    oncall: &str,
) -> Result<()> {
    GLOBAL_CACHE
        .get_or_try_init(|| {
            let mut cache = LruCache::new();
            cache.init_cache_once(fb, config)?;
            cache.init_cacheadmin(oncall)?;
            Ok(cache)
        })
        .map(|_| ())
}

fn get_global_cache() -> Result<&'static LruCache> {
    GLOBAL_CACHE
        .get()
        .ok_or_else(|| ErrorKind::CacheNotInitialized.into())
}
/// Get the remaining unallocated space in the cache
pub fn get_available_space() -> Result<usize> {
    get_global_cache()?.get_available_space()
}

/// Obtain a new pool from the cache. Pools are sub-caches that have their own slice of the cache's
/// available memory, but that otherwise function as independent caches. You cannot write to a
/// cache without a pool. Note that pools are filled in slabs of 4 MiB, so the actual size you
/// receive is floor(pool_bytes / 4 MiB).
///
/// If the pool already exists, you will get the pre-existing pool instead of a new pool
///
/// Pools from this function are potentially persistent, and should not be used for items whose
/// layout might change - e.g. from abomonation
pub fn get_or_create_pool(pool_name: &str, pool_bytes: usize) -> Result<LruCachePool> {
    get_global_cache()?.get_or_create_pool(pool_name, pool_bytes)
}

/// Obtain a new volatile pool from the cache.
///
/// Volatile pools cannot be created from a persistent cache, and are therefore safe for
/// all objects.
pub fn get_or_create_volatile_pool(
    pool_name: &str,
    pool_bytes: usize,
) -> Result<VolatileLruCachePool> {
    let cache = get_global_cache()?;
    if !cache.is_volatile() {
        return Err(ErrorKind::VolatileCachePoolError.into());
    }
    let pool = cache.get_or_create_pool(pool_name, pool_bytes)?;
    Ok(VolatileLruCachePool { inner: pool })
}

/// Returns an existing cache pool by name. Returns Some(pool) if the pool exists, None if the
/// pool has not yet been created.
///
/// This is a potentially persistent pool.
pub fn get_pool(pool_name: &str) -> Option<LruCachePool> {
    get_global_cache().ok()?.get_pool(pool_name)
}

/// Obtains an existing volatile cache pool by name.
///
/// Volatile pools cannot be created from a persistent cache, and are therefore safe for
/// all objects.
pub fn get_volatile_pool(pool_name: &str) -> Result<Option<VolatileLruCachePool>> {
    let cache = get_global_cache()?;
    if !cache.is_volatile() {
        return Err(ErrorKind::VolatileCachePoolError.into());
    }
    match cache.get_pool(pool_name) {
        None => Ok(None),
        Some(cache_pool) => Ok(Some(VolatileLruCachePool { inner: cache_pool })),
    }
}

/// A handle to data stored inside the cache. Can be used to get accessor structs
pub struct LruCacheHandle<T> {
    handle: cxx::UniquePtr<ffi::LruItemHandle>,
    _marker: PhantomData<T>,
}

/// Marker for a read-only handle
pub enum ReadOnly {}
/// Marker for a read-write handle
pub enum ReadWrite {}
/// Marker for a read-write handle that can be shared between processes using this cache
pub enum ReadWriteShared {}

impl<T> LruCacheHandle<T> {
    /// Get this item as a read-only streaming buffer
    pub fn get_reader(&self) -> Result<LruCacheHandleReader<'_>> {
        Ok(LruCacheHandleReader {
            buffer: Cursor::new(self.get_value_bytes()),
        })
    }

    /// Get this item as a byte slice
    pub fn get_value_bytes(&self) -> &[u8] {
        let len = ffi::get_size(&self.handle);
        let src = ffi::get_memory(&self.handle);
        unsafe { slice::from_raw_parts(src, len) }
    }
}

fn get_cache_handle_writer<'a>(
    handle: &'a mut cxx::UniquePtr<ffi::LruItemHandle>,
) -> Result<LruCacheHandleWriter<'a>> {
    let slice: &'a mut [u8] = {
        let len = ffi::get_size(&*handle);
        let src = ffi::get_writable_memory(handle.pin_mut())?;
        unsafe { slice::from_raw_parts_mut(src, len) }
    };

    Ok(LruCacheHandleWriter {
        buffer: Cursor::new(slice),
    })
}

impl LruCacheHandle<ReadWrite> {
    /// Get this item as a writeable streaming buffer
    pub fn get_writer(&mut self) -> Result<LruCacheHandleWriter<'_>> {
        get_cache_handle_writer(&mut self.handle)
    }
}

impl LruCacheHandle<ReadWriteShared> {
    /// Get this item as a writeable streaming buffer
    pub fn get_writer(&mut self) -> Result<LruCacheHandleWriter<'_>> {
        get_cache_handle_writer(&mut self.handle)
    }

    /// Get this item as a remote handle, giving the item's offset and size within
    /// the cache memory block, usable by another process sharing the cache.
    pub fn get_remote_handle(&self) -> Result<LruCacheRemoteHandle<'_>> {
        let cache = get_global_cache()?.get_allocator()?;
        let len = ffi::get_size(&self.handle);
        let src = ffi::get_memory(&self.handle);
        let offset = unsafe { ffi::get_item_ptr_as_offset(cache, src)? };

        Ok(LruCacheRemoteHandle {
            offset,
            len,
            _phantom: PhantomData,
        })
    }
}

/// A read-only handle to an element in the cache. Implements `std::io::Read` and `bytes::Buf`
/// for easy access to the data within the handle
pub struct LruCacheHandleReader<'a> {
    buffer: Cursor<&'a [u8]>,
}

impl<'a> LruCacheHandleReader<'a> {
    pub fn peek(&mut self, cnt: usize) -> Option<&[u8]> {
        let start = self.buffer.position() as usize;
        let data = self.buffer.get_ref();
        if start + cnt < data.len() {
            Some(&data[start..start + cnt])
        } else {
            None
        }
    }

    /// Get direct access to the underlying buffer.
    pub fn buffer(&self) -> &[u8] {
        self.buffer.get_ref()
    }
}

impl<'a> Buf for LruCacheHandleReader<'a> {
    fn remaining(&self) -> usize {
        self.buffer.remaining()
    }

    fn chunk(&self) -> &[u8] {
        Buf::chunk(&self.buffer)
    }

    fn advance(&mut self, cnt: usize) {
        self.buffer.advance(cnt)
    }
}

impl<'a> Read for LruCacheHandleReader<'a> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        self.buffer.read(buf)
    }

    fn read_exact(&mut self, buf: &mut [u8]) -> io::Result<()> {
        self.buffer.read_exact(buf)
    }
}

/// A writable handle to an element in the cache. Implements `std::io::{Read, Write}` and
/// `bytes::{Buf, BufMut}` for easy access to the data within the handle
pub struct LruCacheHandleWriter<'a> {
    buffer: Cursor<&'a mut [u8]>,
}

impl<'a> LruCacheHandleWriter<'a> {
    pub fn position(&self) -> usize {
        self.buffer.position() as usize
    }

    /// Get direct access to the underlying buffer.
    pub fn buffer(&mut self) -> &mut [u8] {
        self.buffer.get_mut()
    }
}

// SAFETY: Only calls to advance_mut modify the current position.
unsafe impl<'a> BufMut for LruCacheHandleWriter<'a> {
    #[inline]
    fn remaining_mut(&self) -> usize {
        let pos = self.buffer.position();
        let len = self.buffer.get_ref().len();
        len.saturating_sub(pos as usize)
    }

    #[inline]
    unsafe fn advance_mut(&mut self, cnt: usize) {
        self.buffer
            .set_position(self.buffer.position() + cnt as u64);
    }

    #[inline]
    fn chunk_mut(&mut self) -> &mut UninitSlice {
        let pos = self.buffer.position();
        let remaining = self
            .buffer
            .get_mut()
            .get_mut(pos as usize..)
            .unwrap_or(&mut []);

        unsafe { UninitSlice::from_raw_parts_mut(remaining.as_mut_ptr(), remaining.len()) }
    }
}

impl<'a> Write for LruCacheHandleWriter<'a> {
    #[inline]
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.buffer.write(buf)
    }

    #[inline]
    fn flush(&mut self) -> io::Result<()> {
        self.buffer.flush()
    }
}

/// A handle remotely access data stored inside the cache. Tied to the lifetime of the
/// LruCacheHandle it is created from.
pub struct LruCacheRemoteHandle<'a> {
    offset: usize,
    len: usize,
    _phantom: PhantomData<&'a ()>,
}

impl<'a> LruCacheRemoteHandle<'a> {
    pub fn get_offset(&self) -> usize {
        self.offset
    }

    pub fn get_length(&self) -> usize {
        self.len
    }
}

/// An LRU cache pool
///
/// There are two interfaces to the cache, depending on the complexity of your use case:
/// 1. `get`/`set`, treating the cache as a simple KV store.
/// 2. `allocate`/`insert_handle`/`get_handle`, which give you smart references to the cache's memory.
///
/// The simple `get`/`set` interface involves multiple copies of data, but protects you from making mistakes
/// The `allocate`/`insert_handle`/`get_handle` interface allows you to pin data in the cache by mistake,
/// but allows you to avoid copying into and out of the cache.
#[derive(Clone)]
pub struct LruCachePool {
    pool: i8,
    pool_name: String,
}

impl LruCachePool {
    /// Setting max TTL allowed to be 2 months
    const MAX_TTL_SECS: u32 = 2 * 30 * 86400;

    fn calc_ttl(&self, ttl: Option<Duration>) -> Result<u32, Error> {
        // Unless ttl is 0 or empty, ttl_secs will be at least 1 and upto
        // 2 months which is equal to 2 * 30 * 86,400 seconds (MAX_TTL_SECS).
        // For example, if ttl is 0 (unit doesn't matter), ttl_secs = 0.
        // If ttl is <=1 sec (100ms, 999ms, 10us, 1000ms), ttl_secs = 1.
        // If ttl is > 1 sec and <= MAX_TTL_SECS, ttl_secs = ttl (round down to the nearest second).
        // If ttl is 1999ms, ttl_secs = 1 sec.
        // if ttl > MAX_TTL_SECS, return Error

        // convert Duration to seconds
        let ttl_secs = ttl.map_or(0, |d| {
            if d == Duration::default() {
                // respect 0 for None and 0 Duration with any unit (ns, us, ms or s)
                0
            } else {
                // if ttl > 0 and <= 1 second, set to 1
                // if ttl > 1, round down to closest second
                std::cmp::max(1, d.as_secs())
            }
        });

        // check if input in seconds in valid
        if ttl_secs > LruCachePool::MAX_TTL_SECS.into() {
            Err(Error::new(ErrorKind::LargeTTLError(
                ttl_secs,
                LruCachePool::MAX_TTL_SECS,
            )))
        } else {
            Ok(ttl_secs
                .try_into()
                .expect("MAX_TTL_SECS should be const-asserted to fit into a u32"))
        }
    }

    /// Allocate memory for a key of known size; this will claim the memory until the handle is
    /// dropped or inserted into the cache.
    ///
    /// Note that if you do not insert the handle, it will not be visible to `get`, and the
    /// associated memory will be pinned until the handle is inserted or dropped. Do not hold onto
    /// handles for long time periods, as this will reduce cachelib's efficiency.
    pub fn allocate_with_ttl<K>(
        &self,
        key: K,
        size: usize,
        ttl: Option<Duration>,
    ) -> Result<Option<LruCacheHandle<ReadWriteShared>>>
    where
        K: AsRef<[u8]>,
    {
        let cache = get_global_cache()?.get_allocator()?;
        let mut full_key = self.pool_name.clone().into_bytes();
        full_key.extend_from_slice(key.as_ref());
        let key = StringPiece::from(full_key.as_slice());

        let ttl_secs = self.calc_ttl(ttl);
        match ttl_secs {
            Ok(ttl_secs) => {
                let size = size.try_into().context("Cache allocation too large")?;
                let handle = ffi::allocate_item(cache, self.pool, key, size, ttl_secs)?;
                if handle.is_null() {
                    Ok(None)
                } else {
                    Ok(Some(LruCacheHandle {
                        handle,
                        _marker: PhantomData,
                    }))
                }
            }
            Err(e) => {
                eprintln!("An error in calculating TTL: {}", e);
                Ok(None)
            }
        }
    }

    /// As allocate_with_ttl, but implicit infinite TTL
    pub fn allocate<K>(
        &self,
        key: K,
        size: usize,
    ) -> Result<Option<LruCacheHandle<ReadWriteShared>>>
    where
        K: AsRef<[u8]>,
    {
        self.allocate_with_ttl(key, size, None)
    }

    /// Insert a previously allocated handle into the cache, making it visible to `get`
    ///
    /// Returns `false` if the handle could not be inserted (e.g. another handle with the same
    /// key was inserted first)
    pub fn insert_handle(&self, mut handle: LruCacheHandle<ReadWriteShared>) -> Result<bool> {
        let cache = get_global_cache()?.get_allocator()?;

        ffi::insert_handle(cache, handle.handle.pin_mut()).map_err(Error::from)
    }

    /// Insert a previously allocated handle into the cache, replacing any pre-existing data.
    pub fn insert_or_replace_handle(
        &self,
        mut handle: LruCacheHandle<ReadWriteShared>,
    ) -> Result<()> {
        let cache = get_global_cache()?.get_allocator()?;

        ffi::insert_or_replace_handle(cache, handle.handle.pin_mut()).map_err(Error::from)
    }

    /// Insert a previously allocated handle into the cache, making it visible to
    /// `get`. Ownership of the handle is given to the caller in cases where the
    /// caller needs the memory to remain pinned. Attempting another insert on
    /// this handle will result in an error.
    pub fn insert_and_keep_handle(
        &self,
        handle: &mut LruCacheHandle<ReadWriteShared>,
    ) -> Result<bool> {
        let cache = get_global_cache()?.get_allocator()?;

        ffi::insert_handle(cache, handle.handle.pin_mut()).map_err(Error::from)
    }

    /// Allocate a new handle, and write value to it.
    pub fn allocate_populate_with_ttl<K, V>(
        &self,
        key: K,
        value: V,
        ttl: Option<Duration>,
    ) -> Result<Option<LruCacheHandle<ReadWriteShared>>>
    where
        K: AsRef<[u8]>,
        V: Buf,
    {
        let datalen = value.remaining();

        match self.allocate_with_ttl(key, datalen, ttl)? {
            None => Ok(None),
            Some(mut handle) => {
                handle.get_writer()?.put(value);
                Ok(Some(handle))
            }
        }
    }

    /// Allocate a new handle, and write value to it. TTL is implicitly infinite
    pub fn allocate_populate<K, V>(
        &self,
        key: K,
        value: V,
    ) -> Result<Option<LruCacheHandle<ReadWriteShared>>>
    where
        K: AsRef<[u8]>,
        V: Buf,
    {
        self.allocate_populate_with_ttl(key, value, None)
    }

    /// Insert a key->value mapping into the pool. Returns true if the insertion was successful,
    /// false otherwise. This will not overwrite existing data.
    pub fn set_with_ttl<K, V>(&self, key: K, value: V, ttl: Option<Duration>) -> Result<bool>
    where
        K: AsRef<[u8]>,
        V: Buf,
    {
        match self.allocate_populate_with_ttl(key, value, ttl)? {
            None => Ok(false),
            Some(handle) => self.insert_handle(handle),
        }
    }

    /// Insert a key->value mapping into the pool. Returns true if the insertion was successful,
    /// false otherwise. This will not overwrite existing data, and sets the TTL to infinite
    pub fn set<K, V>(&self, key: K, value: V) -> Result<bool>
    where
        K: AsRef<[u8]>,
        V: Buf,
    {
        self.set_with_ttl(key, value, None)
    }

    /// Insert a key->value mapping into the pool. Returns true if the insertion was successful,
    /// false otherwise. This will overwrite existing data.
    pub fn set_or_replace_with_ttl<K, V>(
        &self,
        key: K,
        value: V,
        ttl: Option<Duration>,
    ) -> Result<bool>
    where
        K: AsRef<[u8]>,
        V: Buf,
    {
        match self.allocate_populate_with_ttl(key, value, ttl)? {
            None => Ok(false),
            Some(handle) => {
                self.insert_or_replace_handle(handle)?;
                Ok(true)
            }
        }
    }

    /// Insert a key->value mapping into the pool. Returns true if the insertion was successful,
    /// false otherwise. This will overwrite existing data, and sets the TTL to infinite
    pub fn set_or_replace<K, V>(&self, key: K, value: V) -> Result<bool>
    where
        K: AsRef<[u8]>,
        V: Buf,
    {
        self.set_or_replace_with_ttl(key, value, None)
    }

    /// Fetch a read handle for a key. Returns None if the key could not be found in the pool,
    /// Some(handle) if the key was found in the pool
    ///
    /// Note that the handle will stop the key being evicted from the cache until dropped -
    /// do not hold onto the handle for longer than the minimum necessary time.
    pub fn get_handle<K>(&self, key: K) -> Result<Option<LruCacheHandle<ReadWriteShared>>>
    where
        K: AsRef<[u8]>,
    {
        let cache = get_global_cache()?.get_allocator()?;
        let mut full_key = self.pool_name.clone().into_bytes();
        full_key.extend_from_slice(key.as_ref());
        let key = StringPiece::from(full_key.as_slice());
        let handle = ffi::find_item(cache, key)?;

        if handle.is_null() {
            Ok(None)
        } else {
            Ok(Some(LruCacheHandle {
                handle,
                _marker: PhantomData,
            }))
        }
    }

    /// Fetch the value for a key. Returns None if the key could not be found in the pool,
    /// Some(value) if the key was found in the pool
    pub fn get<K>(&self, key: K) -> Result<Option<Bytes>>
    where
        K: AsRef<[u8]>,
    {
        // I have option handle, I want option bytes.
        match self.get_handle(key)? {
            None => Ok(None),
            Some(handle) => {
                let mut bytes = BytesMut::new();
                bytes.put(handle.get_reader()?);
                Ok(Some(bytes.freeze()))
            }
        }
    }

    /// Remove the value for a key. Returns true is successful
    pub fn remove<K>(&self, key: K) -> Result<()>
    where
        K: AsRef<[u8]>,
    {
        let cache = get_global_cache()?.get_allocator()?;
        let mut full_key = self.pool_name.clone().into_bytes();
        full_key.extend_from_slice(key.as_ref());
        let key = StringPiece::from(full_key.as_slice());

        ffi::remove_item(cache, key).map_err(Error::from)
    }

    /// Return the current size of this pool
    pub fn get_size(&self) -> Result<usize> {
        let cache = get_global_cache()?.get_allocator()?;

        ffi::get_pool_size(cache, self.pool).map_err(Error::from)
    }

    /// Increase the size of the pool by size, returning true if it grew, false if there is
    /// insufficent available memory to grow this pool
    pub fn grow_pool(&self, size: usize) -> Result<bool> {
        let cache = get_global_cache()?.get_allocator()?;

        ffi::grow_pool(cache, self.pool, size).map_err(Error::from)
    }

    /// Decrease the size of the pool by size, returning `true` if the pool will shrink, `false`
    /// if the pool is already smaller than size.
    ///
    /// Note that the actual shrinking is done asynchronously, based on the PoolResizeConfig
    /// supplied at the creation of the cachelib setup.
    pub fn shrink_pool(&self, size: usize) -> Result<bool> {
        let cache = get_global_cache()?.get_allocator()?;

        ffi::shrink_pool(cache, self.pool, size).map_err(Error::from)
    }

    /// Move bytes from this pool to another pool, returning true if this pool can shrink,
    /// false if you asked to move more bytes than are available
    ///
    /// Note that the actual movement of capacity is done asynchronously, based on the
    /// PoolResizeConfig supplied at the creation of the cachelib setup.
    pub fn transfer_capacity_to(&self, dest: &Self, bytes: usize) -> Result<bool> {
        let cache = get_global_cache()?.get_allocator()?;

        ffi::resize_pools(cache, self.pool, dest.pool, bytes).map_err(Error::from)
    }

    /// Get the pool name supplied at creation time
    pub fn get_pool_name(&self) -> &str {
        &self.pool_name
    }
}

/// A volatile cache pool. See `LruCachePool` for more details
#[derive(Clone)]
pub struct VolatileLruCachePool {
    inner: LruCachePool,
}

impl VolatileLruCachePool {
    /// Allocate memory for a key of known size; this will claim the memory until the handle is
    /// dropped or inserted into the cache.
    ///
    /// Note that if you do not insert the handle, it will not be visible to `get`, and the
    /// associated memory will be pinned until the handle is inserted or dropped. Do not hold onto
    /// handles for long time periods, as this will reduce cachelib's efficiency.
    pub fn allocate_with_ttl<K>(
        &self,
        key: K,
        size: usize,
        ttl: Option<Duration>,
    ) -> Result<Option<LruCacheHandle<ReadWrite>>>
    where
        K: AsRef<[u8]>,
    {
        let result = self.inner.allocate_with_ttl(key, size, ttl)?;
        Ok(result.map(|cache_handle| LruCacheHandle {
            handle: cache_handle.handle,
            _marker: PhantomData,
        }))
    }

    /// As allocate_with_ttl, but sets the TTL to infinite
    pub fn allocate<K>(&self, key: K, size: usize) -> Result<Option<LruCacheHandle<ReadWrite>>>
    where
        K: AsRef<[u8]>,
    {
        self.allocate_with_ttl(key, size, None)
    }

    /// Insert a previously allocated handle into the cache, making it visible to `get`
    ///
    /// Returns `false` if the handle could not be inserted (e.g. another handle with the same
    /// key was inserted first)
    pub fn insert_handle(&self, handle: LruCacheHandle<ReadWrite>) -> Result<bool> {
        self.inner.insert_handle(LruCacheHandle {
            handle: handle.handle,
            _marker: PhantomData,
        })
    }

    /// Insert a previously allocated handle into the cache, replacing any pre-existing data.
    pub fn insert_or_replace_handle(&self, handle: LruCacheHandle<ReadWrite>) -> Result<()> {
        self.inner.insert_or_replace_handle(LruCacheHandle {
            handle: handle.handle,
            _marker: PhantomData,
        })
    }

    /// Insert a key->value mapping into the pool. Returns true if the insertion was successful,
    /// false otherwise. This will not overwrite existing data.
    pub fn set_with_ttl<K, V>(&self, key: K, value: V, ttl: Option<Duration>) -> Result<bool>
    where
        K: AsRef<[u8]>,
        V: Buf,
    {
        self.inner.set_with_ttl(key, value, ttl)
    }

    /// Insert a key->value mapping into the pool. Returns true if the insertion was successful,
    /// false otherwise. This will not overwrite existing data, and uses an infinite TTL
    pub fn set<K, V>(&self, key: K, value: V) -> Result<bool>
    where
        K: AsRef<[u8]>,
        V: Buf,
    {
        self.inner.set(key, value)
    }

    /// Insert a key->value mapping into the pool. Returns true if the insertion was successful,
    /// false otherwise. This will overwrite existing data.
    pub fn set_or_replace_with_ttl<K, V>(
        &self,
        key: K,
        value: V,
        ttl: Option<Duration>,
    ) -> Result<bool>
    where
        K: AsRef<[u8]>,
        V: Buf,
    {
        self.inner.set_or_replace_with_ttl(key, value, ttl)
    }

    /// Insert a key->value mapping into the pool. Returns true if the insertion was successful,
    /// false otherwise. This will overwrite existing data, and uses an infinite TTL
    pub fn set_or_replace<K, V>(&self, key: K, value: V) -> Result<bool>
    where
        K: AsRef<[u8]>,
        V: Buf,
    {
        self.inner.set_or_replace(key, value)
    }

    /// Fetch a read handle for a key. Returns None if the key could not be found in the pool,
    /// Some(handle) if the key was found in the pool
    ///
    /// Note that the handle will stop the key being evicted from the cache until dropped -
    /// do not hold onto the handle for longer than the minimum necessary time.
    pub fn get_handle<K>(&self, key: K) -> Result<Option<LruCacheHandle<ReadOnly>>>
    where
        K: AsRef<[u8]>,
    {
        let result = self.inner.get_handle(key)?;
        Ok(result.map(|cache_handle| LruCacheHandle {
            handle: cache_handle.handle,
            _marker: PhantomData,
        }))
    }

    /// Remove the value for a key. Returns true is successful
    pub fn remove<K>(&self, key: K) -> Result<()>
    where
        K: AsRef<[u8]>,
    {
        self.inner.remove(key)
    }

    /// Fetch the value for a key. Returns None if the key could not be found in the pool,
    /// Some(value) if the key was found in the pool
    pub fn get<K>(&self, key: K) -> Result<Option<Bytes>>
    where
        K: AsRef<[u8]>,
    {
        self.inner.get(key)
    }

    /// Return the current size of this pool
    pub fn get_size(&self) -> Result<usize> {
        self.inner.get_size()
    }

    /// Increase the size of the pool by size, returning true if it grew, false if there is
    /// insufficent available memory to grow this pool
    pub fn grow_pool(&self, size: usize) -> Result<bool> {
        self.inner.grow_pool(size)
    }

    /// Decrease the size of the pool by size, returning `true` if the pool will shrink, `false`
    /// if the pool is already smaller than size.
    ///
    /// Note that the actual shrinking is done asynchronously, based on the PoolResizeConfig
    /// supplied at the creation of the cachelib setup.
    pub fn shrink_pool(&self, size: usize) -> Result<bool> {
        self.inner.shrink_pool(size)
    }

    /// Move bytes from this pool to another pool, returning true if this pool can shrink,
    /// false if you asked to move more bytes than are available
    ///
    /// Note that the actual movement of capacity is done asynchronously, based on the
    /// PoolResizeConfig supplied at the creation of the cachelib setup.
    pub fn transfer_capacity_to(&self, dest: &Self, bytes: usize) -> Result<bool> {
        self.inner.transfer_capacity_to(&dest.inner, bytes)
    }
}

#[cfg(test)]
mod empty_pool_recovery_tests;

#[cfg(test)]
mod test {
    use tempfile::NamedTempFile;
    use tempfile::TempDir;

    use super::*;

    fn create_basic_cache_config() -> LruCacheConfig {
        LruCacheConfig::new(128 * 1024 * 1024)
            .set_shrinker(ShrinkMonitor {
                shrinker_type: ShrinkMonitorType::ResidentSize {
                    max_process_size_gib: 16,
                    min_process_size_gib: 1,
                },
                interval: Duration::new(1, 0),
                max_resize_per_iteration_percent: 10,
                max_removed_percent: 90,
                strategy: RebalanceStrategy::LruTailAge {
                    age_difference_ratio: 0.1,
                    min_retained_slabs: 1,
                },
            })
            .set_pool_resizer(PoolResizeConfig {
                interval: Duration::new(1, 0),
                slabs_per_iteration: 100,
                strategy: RebalanceStrategy::LruTailAge {
                    age_difference_ratio: 0.1,
                    min_retained_slabs: 1,
                },
            })
            .set_pool_rebalance(PoolRebalanceConfig {
                interval: Duration::new(1, 0),
                strategy: RebalanceStrategy::LruTailAge {
                    age_difference_ratio: 0.1,
                    min_retained_slabs: 1,
                },
            })
    }

    fn create_cache(fb: FacebookInit) {
        let config = create_basic_cache_config();

        if let Err(e) = init_cache(fb, config) {
            panic!("{}", e);
        }
    }

    fn create_temp_dir(dir_prefix: &str) -> TempDir {
        TempDir::with_prefix(dir_prefix).expect("failed to create temp dir")
    }

    fn create_temp_file() -> PathBuf {
        NamedTempFile::new()
            .expect("Failed to create temporary file in test")
            .path()
            .to_path_buf()
    }

    fn create_shared_cache(fb: FacebookInit, cache_directory: PathBuf) {
        let mut config = create_basic_cache_config();
        config = config.set_cache_dir(cache_directory);
        if let Err(e) = init_cache(fb, config) {
            panic!("{}", e);
        }
    }

    fn create_hybrid_cache(fb: FacebookInit) {
        let mut config = create_basic_cache_config();
        let nvm_cache_config = NvmCacheConfig {
            block_size: 4096,
            region_size: 16 * 1024 * 1024,
            device: NvmDevice::SimpleFile {
                file_name: create_temp_file(),
                file_size: 18 * 1024 * 1024,
                truncate: false,
            },
        };
        config = config.set_nvm_cache_config(nvm_cache_config);
        if let Err(e) = init_cache(fb, config) {
            panic!("{}", e);
        }
    }

    #[fbinit::test]
    fn only_create_cache(fb: FacebookInit) {
        create_cache(fb);
    }

    #[fbinit::test]
    fn only_create_shared_cache(fb: FacebookInit) {
        create_shared_cache(
            fb,
            create_temp_dir("test_create_shared_cache").path().into(),
        );
    }

    #[fbinit::test]
    fn only_create_hybrid_cache(fb: FacebookInit) {
        create_hybrid_cache(fb);
    }

    #[fbinit::test]
    fn set_item(fb: FacebookInit) {
        // Insert only, and confirm insert success
        create_cache(fb);

        let pool = get_or_create_volatile_pool("set_item", 4 * 1024 * 1024)
            .unwrap()
            .inner;

        assert!(
            pool.set(b"rimmer", Bytes::from(b"I am a fish".as_ref()))
                .unwrap(),
            "Set failed"
        );
    }

    #[fbinit::test]
    fn set_or_replace_item(fb: FacebookInit) {
        // Insert only, and confirm insert success
        create_cache(fb);

        let pool = get_or_create_volatile_pool("set_or_replace_item", 4 * 1024 * 1024)
            .unwrap()
            .inner;

        pool.set(b"foo", Bytes::from(b"bar1".as_ref())).unwrap();
        pool.set_or_replace(b"foo", Bytes::from(b"bar2".as_ref()))
            .unwrap();

        assert_eq!(
            pool.get(b"foo").unwrap(),
            Some(Bytes::from(b"bar2".as_ref())),
            "Fetch failed"
        );
    }

    #[fbinit::test]
    fn remove_item(fb: FacebookInit) {
        // Set and remove item. Confirm removal
        create_cache(fb);

        let pool = get_or_create_volatile_pool("remove_item", 4 * 1024 * 1024)
            .unwrap()
            .inner;

        pool.set(b"foo", Bytes::from(b"bar1".as_ref())).unwrap();
        pool.remove(b"foo").unwrap();

        assert_eq!(pool.get(b"foo").unwrap(), None, "Remove failed");
    }

    #[fbinit::test]
    fn get_bad_item(fb: FacebookInit) {
        // Fetch an item that doesn't exist
        create_cache(fb);

        let pool = get_or_create_volatile_pool("set_item", 4 * 1024 * 1024).unwrap();

        assert_eq!(
            pool.get(b"rimmer").unwrap(),
            None,
            "Successfully fetched a bad value"
        );
    }

    #[fbinit::test]
    fn set_and_get(fb: FacebookInit) {
        // Set an item, confirm I can get it
        // Insert only, and confirm insert success
        create_cache(fb);

        let pool = get_or_create_volatile_pool("set_item", 4 * 1024 * 1024).unwrap();

        assert!(
            pool.set(b"rimmer", Bytes::from(b"I am a fish".as_ref()))
                .unwrap(),
            "Set failed"
        );

        assert_eq!(
            pool.get(b"rimmer").unwrap(),
            Some(Bytes::from(b"I am a fish".as_ref())),
            "Fetch failed"
        );
    }

    #[fbinit::test]
    fn find_pool_by_name(fb: FacebookInit) -> Result<()> {
        create_cache(fb);

        let pool = get_or_create_volatile_pool("find_pool_by_name", 4 * 1024 * 1024)?;

        assert!(
            pool.set(b"rimmer", Bytes::from(b"I am a fish".as_ref()))
                .unwrap(),
            "Set failed"
        );

        let pool = get_volatile_pool("find_pool_by_name")?.unwrap();
        assert_eq!(
            pool.get(b"rimmer").unwrap(),
            Some(Bytes::from(b"I am a fish".as_ref())),
            "Fetch failed"
        );

        assert!(
            get_volatile_pool("There is no pool")?.is_none(),
            "non-existent pool found"
        );

        Ok(())
    }

    #[fbinit::test]
    fn pool_resizing(fb: FacebookInit) {
        create_cache(fb);
        let pool = get_or_create_volatile_pool("resize", 4 * 1024 * 1024).unwrap();
        let other = get_or_create_volatile_pool("other_pool", 12 * 1024 * 1024).unwrap();

        assert_eq!(
            pool.get_size().unwrap(),
            4 * 1024 * 1024,
            "New pool not of requested size"
        );

        assert!(
            pool.grow_pool(12 * 1024 * 1024).unwrap(),
            "Could not grow pool"
        );
        assert_eq!(
            pool.get_size().unwrap(),
            16 * 1024 * 1024,
            "Pool did not grow"
        );
        assert!(
            other.transfer_capacity_to(&pool, 8 * 1024 * 1024).unwrap(),
            "Could not move capacity"
        );
        assert_eq!(
            pool.get_size().unwrap(),
            24 * 1024 * 1024,
            "Pool stayed too small"
        );
        assert!(
            pool.shrink_pool(20 * 1024 * 1024).unwrap(),
            "Could not shrink pool"
        );
        assert_eq!(
            pool.get_size().unwrap(),
            4 * 1024 * 1024,
            "Pool did not shrink"
        );
    }

    #[fbinit::test]
    fn test_shared_cache(fb: FacebookInit) -> Result<()> {
        // All in same test to avoid race conditions when creating shared cache
        let temp_dir = create_temp_dir("test_shared_cache");
        create_shared_cache(fb, temp_dir.path().into());

        // Test set
        let pool = get_or_create_pool("find_pool_by_name", 4 * 1024 * 1024)?;
        assert!(
            pool.set(b"rimmer", Bytes::from(b"I am a fish".as_ref()))
                .unwrap(),
            "Set failed"
        );

        // Fetch an item that doesn't exist
        assert_eq!(
            pool.get(b"doesnotexist").unwrap(),
            None,
            "Successfully fetched a bad value"
        );

        // Test get
        assert_eq!(
            pool.get(b"rimmer").unwrap(),
            Some(Bytes::from(b"I am a fish".as_ref())),
            "Fetch failed"
        );

        // Test that RemoteHandle length is correct
        pool.set(b"not-rimmer", Bytes::from(b"I am a fish".as_ref()))?;
        assert_eq!(
            pool.get_handle(b"not-rimmer")?
                .unwrap()
                .get_remote_handle()?
                .get_length(),
            b"I am a fish".len(),
            "RemoteHandle handle length is incorrect"
        );

        // Test that two data offsets are not the same
        assert_ne!(
            pool.get_handle(b"rimmer")?
                .unwrap()
                .get_remote_handle()?
                .get_offset(),
            pool.get_handle(b"not-rimmer")?
                .unwrap()
                .get_remote_handle()?
                .get_offset(),
            "Two handles have same offset"
        );

        // Test getting pool by name
        let pool = get_pool("find_pool_by_name").unwrap();
        assert_eq!(
            pool.get(b"rimmer").unwrap(),
            Some(Bytes::from(b"I am a fish".as_ref())),
            "Fetch failed"
        );

        // Test getting nonexistent pool
        assert!(
            get_pool("There is no pool").is_none(),
            "non-existent pool found"
        );
        Ok(())
    }

    #[fbinit::test]
    fn test_hybrid_cache(fb: FacebookInit) -> Result<()> {
        create_hybrid_cache(fb);

        // Test set or replace (since Insert API is not support for HybridCache right now)
        let pool = get_or_create_pool("find_pool_by_name", 24 * 1024 * 1024)?;
        assert!(
            pool.set_or_replace(b"rimmer", Bytes::from(b"I am a fish".as_ref()))
                .unwrap(),
            "Set failed"
        );

        // Fetch an item that doesn't exist
        assert_eq!(
            pool.get(b"doesnotexist").unwrap(),
            None,
            "Successfully fetched a bad value"
        );

        // Test get
        assert_eq!(
            pool.get(b"rimmer").unwrap(),
            Some(Bytes::from(b"I am a fish".as_ref())),
            "Fetch failed"
        );

        // Test update and fetch
        pool.set_or_replace(b"rimmer", Bytes::from(b"I am a bird".as_ref()))?;
        assert_eq!(
            pool.get(b"rimmer").unwrap(),
            Some(Bytes::from(b"I am a bird".as_ref())),
            "Fetch failed"
        );
        Ok(())
    }

    #[fbinit::test]
    fn ttl(fb: FacebookInit) {
        // test ttl in seconds of cache items are set correctly based on the input

        create_cache(fb);
        let pool = get_or_create_volatile_pool("ttl", 4 * 1024 * 1024).unwrap();

        let ttl_test = |pool: &VolatileLruCachePool,
                        key: &[u8],
                        ttl_duration: Option<Duration>,
                        ttl_sec_expected: u32| {
            const VAL: &[u8] = b"TICK TOK";
            assert!(
                pool.set_with_ttl(key, Bytes::from(VAL), ttl_duration)
                    .unwrap(),
                "Set failed in ttl test."
            );

            let item_handle = pool.get_handle(key).unwrap().unwrap();
            let item = item_handle.handle.as_ref().unwrap();
            assert_eq!(
                ffi::get_ttl_secs(item),
                ttl_sec_expected,
                "TTL in seconds did not match."
            );
        };

        // 1. None and 0 (s, ms, us, ns) input sets ttl to 0.
        ttl_test(&pool, b"None", Option::None, 0);
        ttl_test(&pool, b"0ns", Option::Some(Duration::from_nanos(0)), 0);
        ttl_test(&pool, b"0us", Option::Some(Duration::from_micros(0)), 0);
        ttl_test(&pool, b"0ms", Option::Some(Duration::from_millis(0)), 0);
        ttl_test(&pool, b"0s", Option::Some(Duration::from_secs(0)), 0);

        // 2. Any value < 1 second should set ttl to 1.
        // 2a. Test with the smallest value > 0.
        ttl_test(&pool, b"1ns", Option::Some(Duration::from_nanos(1)), 1);

        // 2b. Test with the largest value we can specify less than 1 second.
        ttl_test(
            &pool,
            b"999999999ns",
            Option::Some(Duration::from_nanos(999999999)),
            1,
        );

        // 3. Any value >= 1 should round down to the closest second but max value tested
        // is ttl of 2 months. Large values like u32:MAX fail. The smallest value tested that
        // failed is 3 months (3 * 30 * 86400).
        ttl_test(
            &pool,
            b"6999ms",
            Option::Some(Duration::from_millis(6999)),
            6,
        );

        ttl_test(
            &pool,
            b"16999999us",
            Option::Some(Duration::from_micros(16999999)),
            16,
        );

        ttl_test(
            &pool,
            b"4days",
            Option::Some(Duration::from_secs(4 * 86400)),
            4 * 86400,
        );

        ttl_test(
            &pool,
            b"7days",
            Option::Some(Duration::from_secs(7 * 86400)),
            7 * 86400,
        );

        ttl_test(
            &pool,
            b"30days",
            Option::Some(Duration::from_secs(30 * 86400)),
            30 * 86400,
        );

        ttl_test(
            &pool,
            b"60days",
            Option::Some(Duration::from_secs(2 * 30 * 86400)),
            2 * 30 * 86400,
        );
    }
}
