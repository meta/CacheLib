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
#include <folly/CPortability.h>
#include <folly/Likely.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/fibers/TimedMutex.h>
#include <folly/json/DynamicConverter.h>
#include <folly/logging/xlog.h>
#include <folly/synchronization/SanitizeThread.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <folly/Range.h>
#pragma GCC diagnostic pop

#include "cachelib/allocator/BackgroundMover.h"
#include "cachelib/allocator/CCacheManager.h"
#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/CacheAllocatorConfig.h"
#include "cachelib/allocator/CacheChainedItemIterator.h"
#include "cachelib/allocator/CacheItem.h"
#include "cachelib/allocator/CacheStats.h"
#include "cachelib/allocator/CacheStatsInternal.h"
#include "cachelib/allocator/CacheTraits.h"
#include "cachelib/allocator/CacheVersion.h"
#include "cachelib/allocator/ChainedAllocs.h"
#include "cachelib/allocator/ICompactCache.h"
#include "cachelib/allocator/KAllocation.h"
#include "cachelib/allocator/MemoryMonitor.h"
#include "cachelib/allocator/NvmAdmissionPolicy.h"
#include "cachelib/allocator/NvmCacheState.h"
#include "cachelib/allocator/PoolOptimizeStrategy.h"
#include "cachelib/allocator/PoolOptimizer.h"
#include "cachelib/allocator/PoolRebalancer.h"
#include "cachelib/allocator/PoolResizer.h"
#include "cachelib/allocator/ReadOnlySharedCacheView.h"
#include "cachelib/allocator/Reaper.h"
#include "cachelib/allocator/RebalanceStrategy.h"
#include "cachelib/allocator/Refcount.h"
#include "cachelib/allocator/TempShmMapping.h"
#include "cachelib/allocator/TlsActiveItemRing.h"
#include "cachelib/allocator/TypedHandle.h"
#include "cachelib/allocator/Util.h"
#include "cachelib/allocator/memory/MemoryAllocator.h"
#include "cachelib/allocator/memory/MemoryAllocatorStats.h"
#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"
#include "cachelib/allocator/nvmcache/NvmCache.h"
#include "cachelib/allocator/serialize/gen-cpp2/objects_types.h"
#include "cachelib/common/EventInterface.h"
#include "cachelib/common/Exceptions.h"
#include "cachelib/common/Hash.h"
#include "cachelib/common/Mutex.h"
#include "cachelib/common/PeriodicWorker.h"
#include "cachelib/common/Serialization.h"
#include "cachelib/common/Throttler.h"
#include "cachelib/common/Time.h"
#include "cachelib/common/Utils.h"
#include "cachelib/shm/ShmManager.h"

namespace facebook::cachelib {

using folly::fibers::TimedMutex;

template <typename AllocatorT>
class FbInternalRuntimeUpdateWrapper;

template <typename K, typename V, typename C>
class ReadOnlyMap;

namespace objcache {
template <typename CacheDescriptor, typename AllocatorRes>
class deprecated_ObjectCache;
namespace test {
#define GET_CLASS_NAME(test_case_name, test_name) \
  test_case_name##_##test_name##_Test

#define GET_DECORATED_CLASS_NAME(namespace, test_case_name, test_name) \
  namespace ::GET_CLASS_NAME(test_case_name, test_name)

class GET_CLASS_NAME(ObjectCache, ObjectHandleInvalid);
} // namespace test
} // namespace objcache

namespace objcache2 {
template <typename AllocatorT>
class ObjectCache;

template <typename AllocatorT>
class ObjectCacheBase;
} // namespace objcache2

namespace cachebench {
template <typename Allocator>
class Cache;
namespace tests {
class CacheTest;
}
} // namespace cachebench

namespace tests {
template <typename AllocatorT>
class BaseAllocatorTest;

template <typename AllocatorT>
class AllocatorHitStatsTest;

template <typename AllocatorT>
class AllocatorResizeTest;

template <typename AllocatorT>
class FixedSizeArrayTest;

template <typename AllocatorT>
class MapTest;

class NvmCacheTest;

template <typename AllocatorT>
class PoolOptimizeStrategyTest;

class NvmAdmissionPolicyTest;

class CacheAllocatorTestWrapper;
class PersistenceCache;
class HugePageMatrixTest;
} // namespace tests

namespace interface {
class RAMCacheComponent;
} // namespace interface

// CacheAllocator can provide an interface to make Keyed Allocations(Item) and
// takes two templated types that control how the allocation is
// maintained(MMType aka MemoryManagementType) and accessed(AccessType). The
// cache allocator internally has an allocator that it interacts with to make
// allocations. All active allocations are put into the AccessContainer and
// the MMContainer for maintenance. When the cache is full, allocations are
// garbage collected by the implementation of MMType.
//
// The MMType is used for keeping track of allocations that are currently
// under the control of cache allocator. The MMType is required to provide a
// data structure MMContainer with a well defined interface. For example,
// check MMLru.h's Container (TODO use boost concepts to enforce the
// interface if possible and have it defined in a .h file). The MMType must
// provide a Hook type that will be used to instrument the resultant Item to
// be compatible for use with the MMContainer similar to a boost intrusive
// member hook. MMType::Hook must be sufficient for MMType::Container<T> to
// operate. The MMContainer is expected to implement interfaces to
// add/remove/evict/recordAccess a T& object into the container. This allows
// us to change/abstract away the memory management implementation of the
// cache from the other parts of the cache.
//
// Similar to the MMType, the AccessType is an intrusive data type that
// provides a container to access the keyed allocations. AccessType must
// provide an AccessType::Hook and AccessType::Container with
// find/insert/remove interface similar to a hash table.
//
template <typename CacheTrait>
class CacheAllocator : public CacheBase {
 public:
  using CacheT = CacheAllocator<CacheTrait>;
  using CompressedPtrType = typename CacheTrait::CompressedPtrType;
  using MMType = typename CacheTrait::MMType;
  using AccessType = typename CacheTrait::AccessType;

  using Config = CacheAllocatorConfig<CacheT>;

  // configs for the MMtype and AccessType.
  using MMConfig = typename MMType::Config;
  using AccessConfig = typename AccessType::Config;

  using Item = CacheItem<CacheTrait>;
  using ChainedItem = typename Item::ChainedItem;

  // the holder for the item when we hand it to the caller. This ensures
  // that the reference count is maintained when the caller is done with the
  // item. The ReadHandle/WriteHandle provides a getMemory() and getKey()
  // interface. The caller is free to use the result of these two as long as the
  // handle is active/alive. Using the result of the above interfaces after
  // destroying the ReadHandle/WriteHandle is UB. The ReadHandle/WriteHandle
  // safely wraps a pointer to the "const Item"/"Item".
  using ReadHandle = typename Item::ReadHandle;
  using WriteHandle = typename Item::WriteHandle;
  // Following is deprecated as of allocator version 17 and this line will be
  // removed at a future date
  // using ItemHandle = WriteHandle;
  template <typename UserType,
            typename Converter =
                detail::DefaultUserTypeConverter<Item, UserType>>
  using TypedHandle = TypedHandleImpl<Item, UserType, Converter>;

  // TODO (sathya) some types take CacheT and some take CacheTrait. need to
  // clean this up and come up with a consistent policy that is intuitive.
  using ChainedItemIter = CacheChainedItemIterator<CacheT, const Item>;
  using WritableChainedItemIter = CacheChainedItemIterator<CacheT, Item>;
  using ChainedAllocs = CacheChainedAllocs<CacheT, ReadHandle, ChainedItemIter>;
  using WritableChainedAllocs =
      CacheChainedAllocs<CacheT, WriteHandle, WritableChainedItemIter>;

  using Key = typename Item::Key;
  using PoolIds = std::set<PoolId>;

  using LegacyEventTracker = facebook::cachelib::LegacyEventTracker;

  // SampleItem is a wrapper for the CacheItem which is provided as the sample
  // for uploading to Scuba (see ItemStatsExporter). It is guaranteed that the
  // CacheItem is accessible as long as the SampleItem is around since the
  // internal resource (e.g., ref counts, buffer) will be managed by the iobuf
  class SampleItem {
   public:
    explicit SampleItem(bool fromNvm) : fromNvm_{fromNvm} {}

    SampleItem(folly::IOBuf&& iobuf, const AllocInfo& allocInfo, bool fromNvm)
        : iobuf_{std::move(iobuf)}, allocInfo_{allocInfo}, fromNvm_{fromNvm} {}

    SampleItem(folly::IOBuf&& iobuf,
               PoolId poolId,
               ClassId classId,
               size_t allocSize,
               bool fromNvm)
        : SampleItem(std::move(iobuf),
                     AllocInfo{poolId, classId, allocSize},
                     fromNvm) {}

    const Item* operator->() const noexcept { return get(); }

    const Item& operator*() const noexcept { return *get(); }

    [[nodiscard]] const Item* get() const noexcept {
      return reinterpret_cast<const Item*>(iobuf_.data());
    }

    [[nodiscard]] bool isValid() const { return !iobuf_.empty(); }

    [[nodiscard]] bool isNvmItem() const { return fromNvm_; }

    [[nodiscard]] const AllocInfo& getAllocInfo() const { return allocInfo_; }

   private:
    folly::IOBuf iobuf_;
    AllocInfo allocInfo_{};
    bool fromNvm_ = false;
  };

  // A struct holding optional event details
  struct EventRecordParams {
    folly::Optional<size_t> size{folly::none};       // Size of the item's value
    folly::Optional<uint32_t> ttlSecs{folly::none};  // Time-to-live in seconds
    folly::Optional<time_t> expiryTime{folly::none}; // Absolute expiry
                                                     // timestamp
    folly::Optional<uint32_t> allocSize{folly::none}; // Actual allocated size
    folly::Optional<PoolId> poolId{folly::none};      // Memory pool identifier
  };

  // Called immediately before an item is unlinked from RAM by an explicit
  // removal, expiry, or eviction. The item is valid only for the duration of
  // the call. The callback runs while the access-container bucket is
  // exclusively locked, so it must not throw, call cache APIs, or acquire a
  // lock that may be held while calling cache APIs. Throwing terminates the
  // process. A successful insertOrReplace() does not invoke this callback for
  // the replaced item.
  using PreRemoveCb =
      std::function<void(RemoveContext context, const Item& item)>;

  // holds information about removal, used in RemoveCb
  struct RemoveCbData {
    // remove or eviction
    RemoveContext context;

    // item about to be freed back to allocator
    Item& item;

    // Iterator range pointing to chained allocs associated with @item
    folly::Range<ChainedItemIter> chainedAllocs;
  };

  struct DestructorData {
    DestructorData(DestructorContext ctx,
                   Item& it,
                   folly::Range<ChainedItemIter> iter,
                   PoolId id)
        : context(ctx), item(it), chainedAllocs(iter), pool(id) {}

    // helps to convert RemoveContext to DestructorContext,
    // the context for RemoveCB is re-used to create DestructorData,
    // this can be removed if RemoveCB is dropped.
    DestructorData(RemoveContext ctx,
                   Item& it,
                   folly::Range<ChainedItemIter> iter,
                   PoolId id)
        : item(it), chainedAllocs(iter), pool(id) {
      if (ctx == RemoveContext::kEviction) {
        context = DestructorContext::kEvictedFromRAM;
      } else {
        context = DestructorContext::kRemovedFromRAM;
      }
    }

    // remove or eviction
    DestructorContext context;

    // item about to be freed back to allocator
    // when the item is evicted/removed from NVM, the item is created on the
    // heap, functions (e.g. CacheAllocator::getAllocInfo) that assumes item is
    // located in cache slab doesn't work in such case.
    // chained items must be iterated though @chainedAllocs.
    // Other APIs used to access chained items are not compatible and should not
    // be used.
    Item& item;

    // Iterator range pointing to chained allocs associated with @item
    // when chained items are evicted/removed from NVM, items are created on the
    // heap, functions (e.g. CacheAllocator::getAllocInfo) that assumes items
    // are located in cache slab doesn't work in such case.
    folly::Range<ChainedItemIter> chainedAllocs;

    // the pool that this item is/was
    PoolId pool;
  };

  // call back to execute when moving an item, this could be a simple memcpy
  // or something more complex.
  // An optional parentItem pointer is provided if the item being moved is a
  // chained item.
  using MoveCb =
      std::function<void(Item& oldItem, Item& newItem, Item* parentItem)>;

  // call back type that is executed when the cache item is removed
  // (evicted / freed) from RAM, only items inserted into cache (not nascent)
  // successfully are tracked
  using RemoveCb = std::function<void(const RemoveCbData& data)>;

  // the destructor being executed when the item is removed from cache (both RAM
  // and NVM), only items inserted into cache (not nascent) successfully are
  // tracked.
  using ItemDestructor = std::function<void(const DestructorData& data)>;

  // Predicate called during eviction to decide whether a candidate may be
  // evicted.  Return true to allow eviction, false to skip the candidate and
  // try the next item in the LRU.
  //
  // Caveats:
  //  - The predicate is called while the MMContainer lock is held, so it must
  //    be fast and must not call back into the cache.
  //  - The item has NOT been marked for eviction yet; its memory may be
  //    concurrently mutated by writers holding a WriteHandle.  Use std::atomic
  //    or tolerate benign races.
  //  - If too many candidates are skipped, the eviction search will exhaust
  //    evictionSearchTries and the allocation will fail.  Ensure skipped items
  //    are eventually made evictable.
  //  - For chained items the predicate receives the parent item, not the
  //    chained child.
  //  - This only applies to evictions caused by new allocations, not
  //    evictions from slab releases.
  using EvictionPredicate = std::function<bool(const Item&)>;

  using NvmCacheT = NvmCache<CacheT>;
  using NvmCacheConfig = typename NvmCacheT::Config;
  using DeleteTombStoneGuard = typename NvmCacheT::DeleteTombStoneGuard;

  // Interface for the sync object provided by the user if movingSync is turned
  // on.
  // SyncObj is for CacheLib to obtain exclusive access to an item when
  // it is moving it during slab release. Once held, the user should guarantee
  // the item will not be accessed from another thread.
  struct SyncObj {
    virtual ~SyncObj() = default;

    // Override this function to indicate success/failure of the sync obj,
    // if user-supplied SyncObj can fail. e.g. if a lock can timeout.
    virtual bool isValid() const { return true; }
  };
  using ChainedItemMovingSync = std::function<std::unique_ptr<SyncObj>(Key)>;

  using AccessContainer = typename Item::AccessContainer;
  using MMContainer = typename Item::MMContainer;

  // serialization types
  using MMSerializationType = typename MMType::SerializationType;
  using MMSerializationConfigType = typename MMType::SerializationConfigType;
  using MMSerializationTypeContainer =
      typename MMType::SerializationTypeContainer;
  using AccessSerializationType = typename AccessType::SerializationType;

  using ShmManager = facebook::cachelib::ShmManager;

  // The shared memory segments that can be persisted and re-attached to
  enum SharedMemNewT { SharedMemNew };
  // Attach to a persisted shared memory segment
  enum SharedMemAttachT { SharedMemAttach };

  // instantiates a cache allocator on heap memory
  //
  // @param config        the configuration for the whole cache allocator
  explicit CacheAllocator(Config config);

  // instantiates a cache allocator on shared memory
  //
  // @param config        the configuration for the whole cache allocator
  CacheAllocator(SharedMemNewT, Config config);

  // restore a cache allocator from shared memory
  //
  // @param config        the configuration for the whole cache allocator
  //
  // @throw std::invalid_argument if cannot restore successful
  CacheAllocator(SharedMemAttachT, Config config);

  // Shared segments will be detached upon destruction
  ~CacheAllocator() override;

  // create a new cache allocation. The allocation can be initialized
  // appropriately and made accessible through insert or insertOrReplace.
  // If the handle returned from this api is not passed on to
  // insert/insertOrReplace, the allocation gets destroyed when the handle
  // goes out of scope.
  //
  // @param id              the pool id for the allocation that was previously
  //                        created through addPool
  // @param key             the key for the allocation. This will be made a
  //                        part of the Item and be available through getKey().
  // @param size            the size of the allocation, exclusive of the key
  //                        size.
  // @param ttlSecs         Time To Live(second) for the item,
  //                        default with 0 means no expiration time.
  // @param creationTime    time when the object was created, default with 0
  //                        means creation time of now
  //
  // @return      the handle for the item or an invalid handle(nullptr) if the
  //              allocation failed. Allocation can fail if we are out of memory
  //              and can not find an eviction.
  // @throw   std::invalid_argument if the poolId is invalid or the size
  //          requested is invalid or if the key is invalid(key.size() == 0 or
  //          key.size() > max key length)
  WriteHandle allocate(PoolId id,
                       Key key,
                       uint32_t size,
                       uint32_t ttlSecs = 0,
                       uint32_t creationTime = 0);

  // Allocate a chained item
  //
  // The resulting chained item does not have a parent item and
  // will be freed once the handle is dropped
  //
  // The parent handle parameter here is mainly used to find the
  // correct pool to allocate memory for this chained item
  //
  // @param parent    handle to the cache item
  // @param size      the size for the chained allocation
  //
  // @return    handle to the chained allocation
  // @throw     std::invalid_argument if the size requested is invalid or
  //            if the item is invalid
  WriteHandle allocateChainedItem(const ReadHandle& parent, uint32_t size);

  // Link a chained item to a parent item and mark this parent handle as having
  // chained allocations.
  // The parent handle is not reset (to become a null handle) so that the caller
  // can continue using it as before calling this api.
  //
  // @param parent  handle to the parent item
  // @param child   chained item that will be linked to the parent
  //
  // @throw std::invalid_argument if parent is nullptr
  void addChainedItem(WriteHandle& parent, WriteHandle child);

  // Pop the first chained item associated with this parent and unmark this
  // parent handle as having chained allocations.
  // The parent handle is not reset (to become a null handle) so that the caller
  // can continue using it as before calling this api.
  //
  // @param parent  handle to the parent item
  //
  // @return ChainedItem  head if there exists one
  //         nullptr      otherwise
  WriteHandle popChainedItem(WriteHandle& parent);

  // Return the key to the parent item.
  //
  // This API is racy with transferChainedAndReplace and also with moving during
  // a slab release. To use this safely. user needs to synchronize calls to this
  // API using their user level lock (in exclusive mode). The same user level
  // lock should've been provided via movingSync to CacheLib if moving is
  // enabled for slab rebalancing.
  //
  // @throw std::invalid_argument if chainedItem is not actually a chained item.
  Key getParentKey(const Item& chainedItem);

  // replace a chained item in the existing chain. old and new item must be
  // chained items that have been allocated with the same parent that is
  // passed in. oldItem must be in the chain and newItem must not be.
  //
  // Upon success a handle to the oldItem is returned for the caller
  //
  // @param oldItem  the item we are replacing in the chain
  // @param newItem  the item we are replacing it with
  // @param parent   the parent for the chain
  //
  // @return  handle to the oldItem on return.
  //
  // @throw std::invalid_argument if any of the pre-conditions fails
  WriteHandle replaceChainedItem(Item& oldItem,
                                 WriteHandle newItem,
                                 Item& parent);

  // Transfers the ownership of the chain from the current parent to the new
  // parent and inserts the new parent into the cache. Parent will be unmarked
  // as having chained allocations and its nvmCache will be invalidated. Parent
  // will not be null after calling this API.
  //
  // Caller must synchronize with any modifications to the parent's chain and
  // any calls to find() for the same key to ensure there are no more concurrent
  // parent handle while doing this. While calling this method, the cache does
  // not guarantee a consistent view for the key and the caller must not rely on
  // this. The new parent and old parent must be allocations for the same key.
  // New parent must also be an allocation that is not added to the cache.
  //
  //
  // @param parent    the current parent of the chain we want to transfer
  // @param newParent the new parent for the chain
  //
  // @throw   std::invalid_argument if the parent does not have chained item or
  //          incorrect state of chained item or if any of the pre-conditions
  //          are not met
  void transferChainAndReplace(WriteHandle& parent, WriteHandle& newParent);

  // Inserts the allocated handle into the AccessContainer, making it
  // accessible for everyone. This needs to be the handle that the caller
  // allocated through _allocate_. If this call fails, the allocation will be
  // freed back when the handle gets out of scope in the caller.
  //
  // @param  handle  the handle for the allocation.
  //
  // @return true if the handle was successfully inserted into the hashtable
  //         and is now accessible to everyone. False if there was an error.
  //
  // @throw std::invalid_argument if the handle is already accessible.
  bool insert(const WriteHandle& handle);

  // Replaces the allocated handle into the AccessContainer, making it
  // accessible for everyone. If an existing handle is already in the
  // container, remove that handle. This needs to be the handle that the caller
  // allocated through _allocate_. If this call fails, the allocation will be
  // freed back when the handle gets out of scope in the caller.
  //
  // @param  handle  the handle for the allocation.
  //
  // @throw std::invalid_argument if the handle is already accessible.
  // @throw cachelib::exception::RefcountOverflow if the item we are replacing
  //        is already out of refcounts.
  // @return handle to the old item that had been replaced
  WriteHandle insertOrReplace(const WriteHandle& handle);

  // look up an item by its key across the nvm cache as well if enabled.
  //
  // @param key       the key for lookup
  //
  // @return          the read handle for the item or a handle to nullptr if the
  //                  key does not exist.
  ReadHandle find(Key key);

  // Warning: this API is synchronous today with HybridCache. This means as
  //          opposed to find(), we will block on an item being read from
  //          flash until it is loaded into DRAM-cache. In find(), if an item
  //          is missing in dram, we will return a "not-ready" handle and
  //          user can choose to block or convert to folly::SemiFuture and
  //          process the item only when it becomes ready (loaded into DRAM).
  //          If blocking behavior is NOT what you want, a workaround is:
  //            auto readHandle = cache->find("my key");
  //            if (!readHandle.isReady()) {
  //              auto sf = std::move(readHandle)
  //                .toSemiFuture()
  //                .defer([] (auto readHandle)) {
  //                  return std::move(readHandle).toWriteHandle();
  //                }
  //            }
  //
  // look up an item by its key across the nvm cache as well if enabled. Users
  // should call this API only when they are going to mutate the item data.
  //
  // @param key               the key for lookup
  //
  // @return      the write handle for the item or a handle to nullptr if the
  //              key does not exist.
  WriteHandle findToWrite(Key key);

  // look up an item by its key. This ignores the nvm cache and only does RAM
  // lookup.
  //
  // @param key         the key for lookup
  //
  // @return      the read handle for the item or a handle to nullptr if the key
  //              does not exist.
  FOLLY_ALWAYS_INLINE ReadHandle findFast(Key key);

  // look up an item by its key. This ignores the nvm cache and only does RAM
  // lookup. Users should call this API only when they are going to mutate the
  // item data.
  //
  // @param key         the key for lookup
  //
  // @return      the write handle for the item or a handle to nullptr if the
  //              key does not exist.
  FOLLY_ALWAYS_INLINE WriteHandle findFastToWrite(Key key);

  // look up an item by its key. This ignores the nvm cache and only does RAM
  // lookup. This API does not update the stats related to cache gets and misses
  // nor mark the item as useful (see markUseful below).
  //
  // @param key   the key for lookup
  // @return      the handle for the item or a handle to nullptr if the key does
  //              not exist.
  FOLLY_ALWAYS_INLINE ReadHandle peek(Key key);

  // Returns the storage medium if a key is potentially in cache. There is a
  // non-zero chance the key does not exist in cache (e.g. hash collision in
  // NvmCache) even if a storage medium is returned. This check is meant to be
  // synchronous and fast as we only check DRAM cache and in-memory index for
  // NvmCache. Similar to peek, this does not indicate to cachelib you have
  // looked up an item (i.e. no stats bump, no eviction queue promotion, etc.)
  //
  // @param key   the key for lookup
  // @return      true if the key could exist, false otherwise
  StorageMedium existFast(Key key);

  // Returns true if a key is potentially in cache, based on existFast.
  bool couldExistFast(Key key);

  // Mark an item that was fetched through peek as useful. This is useful when
  // users want to look into the cache and only mark items as useful when they
  // inspect the contents of it.
  //
  // @param handle        the item handle
  // @param mode          the mode of access for the lookup. defaults to
  //                      AccessMode::kRead
  void markUseful(const ReadHandle& handle, AccessMode mode);

  using AccessIterator = typename AccessContainer::Iterator;
  // Iterator interface for the cache. It guarantees that all keys that were
  // present when the iteration started will be accessible unless they are
  // removed. Keys that are removed/inserted during the lifetime of an
  // iterator are not guaranteed to be either visited or not-visited.
  // Adding/Removing from the hash table while the iterator is alive will not
  // inivalidate any iterator or the element that the iterator points at
  // currently. The iterator internally holds a Handle to the item and hence
  // the keys that the iterator holds reference to, will not be evictable
  // until the iterator is destroyed.
  AccessIterator begin() { return accessContainer_->begin(); }

  // return an iterator with a throttler for throttled iteration
  AccessIterator begin(util::Throttler::Config config) {
    return accessContainer_->begin(config);
  }

  AccessIterator end() { return accessContainer_->end(); }

  // Alternative iterator that batches by hash table lock group instead of
  // per-bucket.
  //
  // Differences from AccessIterator:
  //   - Fewer lock acquisitions: O(numLocks) vs O(numBuckets).
  //   - Larger pinning window: each lock group covers numBuckets/numLocks
  //     buckets. All items across those buckets are snapshotted as Handles
  //     at once, blocking eviction until the caller advances past them.
  //     AccessIterator snapshots one bucket at a time.
  // Optional key filter is applied under the lock before handle creation,
  // avoiding handle overhead for items that don't match.
  using LockGroupAccessIterator = typename AccessContainer::LockGroupIterator;
  using LockGroupFilterFn = typename LockGroupAccessIterator::FilterFn;

  LockGroupAccessIterator beginLockGroup(LockGroupFilterFn filter = {}) {
    return accessContainer_->beginLockGroup(
        [this](Item* it) { return tryAcquire(it); },
        [this](Key key) -> WriteHandle { return findInternal(key); },
        std::move(filter));
  }

  LockGroupAccessIterator beginLockGroup(util::Throttler::Config config,
                                         LockGroupFilterFn filter = {}) {
    return accessContainer_->beginLockGroup(
        [this](Item* it) { return tryAcquire(it); },
        [this](Key key) -> WriteHandle { return findInternal(key); },
        std::move(filter),
        config);
  }

  LockGroupAccessIterator endLockGroup() {
    return accessContainer_->endLockGroup();
  }

  enum class RemoveRes : uint8_t {
    kSuccess,
    kNotFoundInRam,
  };
  // removes the allocation corresponding to the key, if present in the hash
  // table. The key will not be accessible through find() after this returns
  // success. The allocation for the key will be recycled once all active
  // Item handles are released.
  //
  // @param key   the key for the allocation.
  // @return      kSuccess if the key exists and was successfully removed.
  //              kNotFoundInRam if the key was not present in memory (doesn't
  //              check nvm)
  RemoveRes remove(Key key);

  // remove the key that the iterator is pointing to. The element will
  // not be accessible upon success. However, the elemenet will not actually be
  // recycled until the iterator destroys the internal handle.
  //
  // @param  it   the iterator to the key to be destroyed.
  // @return      kSuccess if the element was still in the hashtable and it was
  //              successfully removed.
  //              kNotFoundInRam if the element the iterator was pointing to was
  //              deleted already.
  RemoveRes remove(AccessIterator& it);

  // removes the allocation corresponding to the handle. The allocation will
  // be freed when all the existing handles are released.
  //
  // @param  it   item read handle
  //
  // @return      kSuccess if the item exists and was successfully removed.
  //              kNotFoundInRam otherwise
  //
  // @throw std::invalid_argument if item handle is null
  RemoveRes remove(const ReadHandle& it);

  // view a read-only parent item as a chain of allocations if it has chained
  // alloc. The returned chained-alloc is good to iterate upon, but will block
  // any concurrent addChainedItem or popChainedItem for the same key until the
  // ChainedAllocs object is released. This is ideal for use cases which do
  // very brief operations on the chain of allocations.
  //
  // The ordering of the iteration for the chain is LIFO. Check
  // CacheChainedAllocs.h for the API and usage.
  //
  // @param parent  the parent allocation of the chain from a ReadHandle.
  // @return        read-only chained alloc view of the parent
  //
  // @throw std::invalid_argument if the parent does not have chained allocs
  ChainedAllocs viewAsChainedAllocs(const ReadHandle& parent) {
    return viewAsChainedAllocsT<ReadHandle, ChainedItemIter>(parent);
  }

  // view a writable parent item as a chain of allocations if it has chained
  // alloc. The returned chained-alloc is good to iterate upon, but will block
  // any concurrent addChainedItem or popChainedItem for the same key until the
  // ChainedAllocs object is released. This is ideal for use cases which do
  // very brief operations on the chain of allocations.
  //
  // The ordering of the iteration for the chain is LIFO. Check
  // CacheChainedAllocs.h for the API and usage.
  //
  // @param parent  the parent allocation of the chain from a WriteHandle.
  // @return        writable chained alloc view of the parent
  //
  // @throw std::invalid_argument if the parent does not have chained allocs
  WritableChainedAllocs viewAsWritableChainedAllocs(const WriteHandle& parent) {
    return viewAsChainedAllocsT<WriteHandle, WritableChainedItemIter>(parent);
  }

  // Return whether a key is valid.  The length of the key needs to be in (0,
  // kKeyMaxLenSmall) or (0, kKeyMaxLen) for large keys to be valid.
  bool isKeyValid(Key key) const;

  // Throw if the key is invalid.
  void throwIfKeyInvalid(Key key) const;

  // Returns the full usable size for this item
  // This can be bigger than item.getSize()
  //
  // @param item    reference to an item
  //
  // @return    the full usable size for this item
  uint32_t getUsableSize(const Item& item) const;

  // create memory assignment to bg workers
  auto createBgWorkerMemoryAssignments(size_t numWorkers);

  // Get a random item from memory
  // This is useful for profiling and sampling cachelib managed memory
  //
  // @return Valid SampleItem if an valid item is found
  //         Invalid SampleItem if the randomly chosen memory does not
  //                 belong to an valid item
  //         Should be checked with SampleItem.isValid() before use
  SampleItem getSampleItem();

  // Convert a Read Handle to an IOBuf. The returned IOBuf gives a
  // read-only view to the user. The item's ownership is retained by
  // the IOBuf until its destruction.
  //
  // When the read handle has one or more chained items attached to it,
  // user will also get a series of IOBufs (first of which is the Parent).
  //
  // **WARNING**: folly::IOBuf allows mutation to a cachelib item even when the
  // item is read-only. User is responsible to ensure no mutation occurs (i.e.
  // only const functions are called). If mutation is required, please use
  // `convertToIOBufForWrite`.
  //
  // @param handle    read handle that will transfer its ownership to an IOBuf
  //
  // @return   an IOBuf that contains the value of the item.
  //           This IOBuf acts as a Read Handle, on destruction, it will
  //           properly decrement the refcount (to release the item).
  // @throw   std::invalid_argument if ReadHandle is nullptr
  folly::IOBuf convertToIOBuf(ReadHandle handle) {
    return convertToIOBufT<ReadHandle>(handle);
  }

  // Convert a Write Handle to an IOBuf. The returned IOBuf gives a
  // writable view to the user. The item's ownership is retained by
  // the IOBuf until its destruction.
  //
  // When the write handle has one or more chained items attached to it,
  // user will also get a series of IOBufs (first of which is the Parent).
  //
  // @param handle    write handle that will transfer its ownership to an IOBuf
  //
  // @return   an IOBuf that contains the value of the item.
  //           This IOBuf acts as a Write Handle, on destruction, it will
  //           properly decrement the refcount (to release the item).
  // @throw   std::invalid_argument if WriteHandle is nullptr
  folly::IOBuf convertToIOBufForWrite(WriteHandle handle) {
    return convertToIOBufT<WriteHandle>(handle);
  }

  // TODO: When Read/Write Handles are ready, change this to allow
  //       const-only access to data manged by iobuf and offer a
  //       wrapAsWritableIOBuf() API.
  //
  // wrap an IOBuf over the data for an item. This IOBuf does not own the item
  // and the caller is responsible for ensuring that the IOBuf is valid with
  // the item lifetime. If the item has chained allocations, the chains are
  // also wrapped into the iobuf as chained iobufs
  //
  // @param item  the item to wrap around
  //
  // @return   an IOBuf that contains the value of the item.
  folly::IOBuf wrapAsIOBuf(const Item& item);

  // creates a pool for the cache allocator with the corresponding name.
  //
  // @param name                  name of the pool
  // @param size                  size of the pool
  // @param allocSizes            allocation class sizes, if empty, a default
  //                              one from the memory allocator will be used
  // @param config                MMConfig for the MMContainer,
  //                              default constructed if user doesn't supply one
  // @param rebalanceStrategy     rebalance strategy for the pool. If not set,
  //                              the default one will be used.
  // @param resizeStrategy        resize strategy for the pool. If not set,
  //                              the default one will be used.
  // @param ensureProvisionable   ensures that the size of the pool is enough
  //                              to give one slab to each allocation class,
  //                              false by default.
  //
  // @return a valid PoolId that the caller can use.
  // @throw   std::invalid_argument if the size is invalid or there is not
  //          enough space for creating the pool.
  //          std::logic_error if we have run out of pools.
  PoolId addPool(folly::StringPiece name,
                 size_t size,
                 const std::set<uint32_t>& allocSizes = {},
                 MMConfig config = {},
                 std::shared_ptr<RebalanceStrategy> rebalanceStrategy = nullptr,
                 std::shared_ptr<RebalanceStrategy> resizeStrategy = nullptr,
                 bool ensureProvisionable = false);

  // This should only be called on cache startup on a new memory pool. Provision
  // a pool by filling up each allocation class with prescribed number of slabs.
  // This is useful for users that know their workload distribution in
  // allocation sizes.
  //
  // @param poolId              id of the pool to provision
  // @param slabsDistribution   number of slabs in each AC
  // @return true if we have enough memory and filled each AC successfully
  //         false otherwise. On false, we also revert all provisioned ACs.
  bool provisionPool(PoolId poolId,
                     const std::vector<uint32_t>& slabsDistribution);

  // Provision slabs to a memory pool using power law from small AC to large.
  // @param poolId          id of the pool to provision
  // @param power           power for the power law
  // @param minSlabsPerAC   min number of slabs for each AC before power law
  // @return true if we have enough memory and filled each AC successfully
  //         false otherwise. On false, we also revert all provisioned ACs.
  bool provisionPoolWithPowerLaw(PoolId poolId,
                                 double power,
                                 uint32_t minSlabsPerAC = 1);

  // Cap how many unallocated slabs each allocation class may take while a new
  // pool fills, so warm-up follows a target distribution instead of going to
  // whichever classes allocate first. A class at its cap evicts within itself.
  // The caps lift once every slab is allocated or `maxDuration` elapses.
  //
  // @param poolId        id of the pool
  // @param caps          max slabs per AC, indexed by ClassId; empty to clear
  // @param maxDuration   when to lift the caps if the pool has not filled
  void setPoolSlabCaps(PoolId poolId,
                       std::vector<uint32_t> caps,
                       std::chrono::seconds maxDuration) {
    allocator_->setPoolSlabCaps(poolId, std::move(caps), maxDuration);
  }

  // Replace the caps of a pool that is still warming up, keeping its deadline.
  // No-op once the caps have lifted.
  void updatePoolSlabCaps(PoolId poolId, std::vector<uint32_t> caps) {
    allocator_->updatePoolSlabCaps(poolId, std::move(caps));
  }

  // true while the pool's warm-up slab caps are in force.
  bool poolHasSlabCaps(PoolId poolId) const {
    return allocator_->poolHasSlabCaps(poolId);
  }

  // update an existing pool's config
  //
  // @param pid       pool id for the pool to be updated
  // @param config    new config for the pool
  //
  // @throw std::invalid_argument if the poolId is invalid
  void overridePoolConfig(PoolId pid, const MMConfig& config);

  // update an existing pool's rebalance strategy
  //
  // @param pid                 pool id for the pool to be updated
  // @param rebalanceStrategy   new rebalance strategy for the pool
  //
  // @throw std::invalid_argument if the poolId is invalid
  void overridePoolRebalanceStrategy(
      PoolId pid, std::shared_ptr<RebalanceStrategy> rebalanceStrategy);

  // update an existing pool's resize strategy
  //
  // @param pid                 pool id for the pool to be updated
  // @param resizeStrategy      new resize strategy for the pool
  //
  // @throw std::invalid_argument if the poolId is invalid
  void overridePoolResizeStrategy(
      PoolId pid, std::shared_ptr<RebalanceStrategy> resizeStrategy);

  // update pool size optimization strategy for this cache
  // @param optimizeStrategy    new resize strategy
  void overridePoolOptimizeStrategy(
      std::shared_ptr<PoolOptimizeStrategy> optimizeStrategy);

  /**
   * PoolResizing can be done online while the cache allocator is being used
   * to do allocations. Pools can be grown or shrunk using the following api.
   * The actual resizing happens asynchronously and is controlled by the
   * config parameters poolResizeIntervalSecs and poolResizeSlabsPerIter. The
   * pool resizer releases slabs from pools that are over limit when the
   * memory allocator is out of memory. If there is enough free memory
   * available, the pool resizer does not do any resizing until the memory is
   * exhausted and there is some pool that is over the limit
   */

  // shrink the existing pool by _bytes_ .
  // @param bytes  the number of bytes to be taken away from the pool
  // @return  true if the operation succeeded. false if the size of the pool is
  //          smaller than _bytes_
  // @throw   std::invalid_argument if the poolId is invalid.
  bool shrinkPool(PoolId pid, size_t bytes) {
    return allocator_->shrinkPool(pid, bytes);
  }

  // grow an existing pool by _bytes_. This will fail if there is no
  // available memory across all the pools to provide for this pool
  // @param bytes  the number of bytes to be added to the pool.
  // @return    true if the pool was grown. false if the necessary number of
  //            bytes were not available.
  // @throw     std::invalid_argument if the poolId is invalid.
  bool growPool(PoolId pid, size_t bytes) {
    return allocator_->growPool(pid, bytes);
  }

  // move bytes from one pool to another. The source pool should be at least
  // _bytes_ in size.
  //
  // @param src     the pool to be sized down and giving the memory.
  // @param dest    the pool receiving the memory.
  // @param bytes   the number of bytes to move from src to dest.
  // @param   true if the resize succeeded. false if src does does not have
  //          correct size to do the transfer.
  // @throw   std::invalid_argument if src or dest is invalid pool
  bool resizePools(PoolId src, PoolId dest, size_t bytes) override {
    return allocator_->resizePools(src, dest, bytes);
  }

  // Add a new compact cache with given name and size
  //
  // @param name  name of the compact cache pool
  // @param size  size of the compact cache pool
  // @param args  All of the arguments in CompactCache afer allocator
  //              So the signature of addCompactCache is:
  //              addCompactCache(folly::StringPiece name,
  //                              size_t size,
  //                              RemoveCb removeCb,
  //                              ReplaceCb replaceCb,
  //                              ValidCb validCb,
  //                              bool allowPromotions = true);
  //              addCompactCache(folly::StringPiece name,
  //                              size_t size,
  //                              bool allowPromotions = true);
  //
  // @return pointer to CompactCache instance of the template type
  //
  // @throw std::logic_error        if compact cache is not enabled
  // @throw std::invalid_argument   There is a memory pool that has the same
  //                                name as the compact cache we are adding or
  //                                if there is no sufficient space to create
  //                                a compact cache.
  template <typename CCacheT, typename... Args>
  CCacheT* addCompactCache(folly::StringPiece name,
                           size_t size,
                           Args&&... args);

  // Attach a compact cache to the given pool after warm roll
  //
  // @param name  name of the compact cache pool
  // @param args  All of the arguments in CompactCache afer allocator
  //              So the signature of attachCompactCache is:
  //              attachCompactCache(folly::StringPiece name,
  //                                 RemoveCb removeCb,
  //                                 ReplaceCb replaceCb,
  //                                 ValidCb validCb,
  //                                 bool allowPromotions = true);
  //              attachCompactCache(folly::StringPiece name,
  //                                 bool allowPromotions = true);
  //
  // @return  pointer to CompactCache instance of the template type.
  //
  // @throw std::out_of_range       if the pool does not exist
  // @throw std::invalid_argument   if the compact key/value size does not match
  //                                from warm roll
  template <typename CCacheT, typename... Args>
  CCacheT* attachCompactCache(folly::StringPiece name, Args&&... args);

  // Return the base iterface of an attached compact cache to pull out its
  // stats. For non-active compact cache, this would throw
  // std::invalid_argument.
  const ICompactCache& getCompactCache(PoolId pid) const override;

  // The enum value that indicates the CacheAllocator's shutdown status.
  enum class ShutDownStatus {
    kSuccess = 0, // Successfully persisted the DRAM cache, and the NvmCache if
                  // enabled.
    kSavedOnlyDRAM, // Successfully persisted the DRAM cache only; NvmCache is
                    // enabled but failed to persist it.
    kSavedOnlyNvmCache, // Successfully persisted the enabled NvM cache only;
                        // Failed to persist DRAM cache.
    kFailed, // Failed to persist both the DRAM cache and the enabled NvmCache.
    kSkipped // Duplicate shutDown() call
  };

  // Persists the state of the cache allocator. On a successful shutdown,
  // this cache allocator can be restored on restart.
  //
  // precondition:  serialization must happen without any reader or writer
  // present. Any modification of this object afterwards will result in an
  // invalid, inconsistent state for the serialized data. There must not be
  // any outstanding active handles
  //
  // @throw   std::invalid_argument if the cache allocator isn't using shared
  //          memory
  // @throw   std::logic_error if any component is not restorable.
  // @return  A ShutDownStatus value indicating the result of the shutDown
  //          operation.
  //          kSuccess - successfully shut down and can be re-attached
  //          kFailed - failure due to outstanding active handles, error with
  //                    cache dir or error stopping background workers
  //          kSavedOnlyDRAM and kSavedOnlyNvmCache - partial content saved
  //          kSkipped - duplicate shutdown call
  ShutDownStatus shutDown();

  // No-op for workers that are already running. Typically user uses this in
  // conjunction with `config.delayWorkerStart()` to avoid initialization
  // ordering issues with user callback for cachelib's workers.
  void startCacheWorkers();

  // Functions that stop existing ones (if any) and create new workers

  // start pool rebalancer
  // @param interval            the period this worker fires.
  // @param strategy            rebalancing strategy
  // @param freeAllocThreshold  threshold for free-alloc-slab for picking victim
  // allocation class. free-alloc-slab is calculated by the number of free
  // allocation divided by the number of allocations in one slab. Only
  // allocation classes with a higher free-alloc-slab than the threshold would
  // be picked as a victim.
  //
  //
  bool startNewPoolRebalancer(std::chrono::milliseconds interval,
                              std::shared_ptr<RebalanceStrategy> strategy,
                              unsigned int freeAllocThreshold);

  // start pool resizer
  // @param interval                the period this worker fires.
  // @param poolResizeSlabsPerIter  maximum number of slabs each pool may remove
  //                                in resizing.
  // @param strategy                resizing strategy
  bool startNewPoolResizer(std::chrono::milliseconds interval,
                           unsigned int poolResizeSlabsPerIter,
                           std::shared_ptr<RebalanceStrategy> strategy);

  // start pool optimizer
  // @param regularInterval         the period for optimizing regular cache
  // @param ccacheInterval          the period for optimizing compact cache
  // @param strategy                pool optimizing strategy
  // @param ccacheStepSizePercent   the percentage number that controls the size
  //                                of each movement in a compact cache
  //                                optimization.
  bool startNewPoolOptimizer(std::chrono::seconds regularInterval,
                             std::chrono::seconds ccacheInterval,
                             std::shared_ptr<PoolOptimizeStrategy> strategy,
                             unsigned int ccacheStepSizePercent);

  // start memory monitor.
  //
  // A running monitor is stopped and replaced. Reclaims advised slabs from
  // pools outside config.poolIds before starting. startNewMemMonitor() and
  // stopMemMonitor() must not be called concurrently with each other.
  //
  // @throw std::invalid_argument if config.poolIds has an id that is not a
  //        regular pool; a running monitor is then left running.
  // @param interval                        the period this worker fires
  // @param config                          memory monitoring config
  // @param strategy                        strategy to find an allocation class
  //                                        to release slab from
  bool startNewMemMonitor(std::chrono::milliseconds interval,
                          MemoryMonitor::Config config,
                          std::shared_ptr<RebalanceStrategy> strategy);

  // start reaper
  // @param interval                the period this worker fires
  // @param reaperThrottleConfig    throttling config
  bool startNewReaper(std::chrono::milliseconds interval,
                      util::Throttler::Config reaperThrottleConfig);

  // start background promoter, starting/stopping of this worker
  // should not be done concurrently with addPool
  // @param interval                the period this worker fires
  // @param strategy                strategy to promote items
  // @param threads                 number of threads used by the worker
  bool startNewBackgroundPromoter(
      std::chrono::milliseconds interval,
      std::shared_ptr<BackgroundMoverStrategy> strategy,
      size_t threads);

  // start background evictor, starting/stopping of this worker
  // should not be done concurrently with addPool
  // @param interval                the period this worker fires
  // @param strategy                strategy to evict items
  // @param threads                 number of threads used by the worker
  bool startNewBackgroundEvictor(
      std::chrono::milliseconds interval,
      std::shared_ptr<BackgroundMoverStrategy> strategy,
      size_t threads);

  // Stop existing workers with a timeout
  bool stopPoolRebalancer(std::chrono::seconds timeout = std::chrono::seconds{
                              0});
  bool stopPoolResizer(std::chrono::seconds timeout = std::chrono::seconds{0});
  bool stopPoolOptimizer(std::chrono::seconds timeout = std::chrono::seconds{
                             0});
  bool stopMemMonitor(std::chrono::seconds timeout = std::chrono::seconds{0});
  bool stopReaper(std::chrono::seconds timeout = std::chrono::seconds{0});
  bool stopBackgroundEvictor(
      std::chrono::seconds timeout = std::chrono::seconds{0});
  bool stopBackgroundPromoter(
      std::chrono::seconds timeout = std::chrono::seconds{0});

  // Set pool optimization to either true or false
  //
  // @param poolId The ID of the pool to optimize
  void setPoolOptimizerFor(PoolId poolId, bool enableAutoResizing);

  // stop the background workers
  // returns true if all workers have been successfully stopped
  bool stopWorkers(std::chrono::seconds timeout = std::chrono::seconds{0});

  // get the allocation information for the specified memory address.
  // @throw std::invalid_argument if the memory does not belong to this
  //        cache allocator
  AllocInfo getAllocInfo(const void* memory) const {
    return allocator_->getAllocInfo(memory);
  }

  // total allocated size of the item and all of its chained items.
  uint64_t getTotalAllocSize(const Item& item,
                             folly::Range<ChainedItemIter> chainedItems) const {
    uint64_t size = getAllocInfo(item.getMemory()).allocSize;
    for (const auto& c : chainedItems) {
      size += getAllocInfo(c.getMemory()).allocSize;
    }
    return size;
  }

  // return the ids for the set of existing pools in this cache.
  std::set<PoolId> getPoolIds() const override final {
    return allocator_->getPoolIds();
  }

  // return a list of pool ids that are backing compact caches. This includes
  // both attached and un-attached compact caches.
  std::set<PoolId> getCCachePoolIds() const override final;

  // return a list of pool ids for regular pools.
  std::set<PoolId> getRegularPoolIds() const override final;

  // return the pool with speicified id.
  const MemoryPool& getPool(PoolId pid) const override final {
    return allocator_->getPool(pid);
  }

  // calculate the number of slabs to be advised/reclaimed in each pool
  PoolAdviseReclaimData calcNumSlabsToAdviseReclaim(
      size_t numSlabsToAdvise) override final {
    auto regularPoolIds = getRegularPoolIds();
    return allocator_->calcNumSlabsToAdviseReclaim(numSlabsToAdvise,
                                                   regularPoolIds);
  }

  // calculate the number of slabs to be advised/reclaimed in each of the
  // given regular pools
  PoolAdviseReclaimData calcNumSlabsToAdviseReclaim(
      size_t numSlabsToAdvise, const std::set<PoolId>& poolIds) override final {
    return allocator_->calcNumSlabsToAdviseReclaim(numSlabsToAdvise, poolIds);
  }

  // @return the number of slabs advised away from the given pools
  // @throw std::invalid_argument if a pool id is not a valid pool
  size_t getNumSlabsAdvised(
      const std::set<PoolId>& poolIds) const override final {
    return allocator_->getNumSlabsAdvised(poolIds);
  }

  // returns a valid PoolId corresponding to the name or kInvalidPoolId if the
  // name is not a recognized pool
  PoolId getPoolId(folly::StringPiece name) const noexcept;

  // returns the pool's name by its poolId.
  std::string getPoolName(PoolId poolId) const override {
    return allocator_->getPoolName(poolId);
  }

  // get stats related to all kinds of slab release events.
  SlabReleaseStats getSlabReleaseStats() const noexcept override final;

  // Increment the number of aborted slab releases stat
  void incrementAbortedSlabReleases() override final {
    stats_.numAbortedSlabReleases.inc();
  }

  // Check if a fast shutdown has been triggered, signalling workers to abort
  // in-progress work (e.g. slab release) so shutDown() can proceed quickly.
  bool isFastShutdownTriggered() const override final {
    return triggerFastShutdown_.load();
  }

  // return the distribution of the keys in the cache. This is expensive to
  // compute at times even with caching. So use with caution.
  // TODO think of a way to abstract this since it only makes sense for
  // bucketed hashtables with chaining.
  using DistributionStats = typename AccessContainer::DistributionStats;
  DistributionStats getAccessContainerDistributionStats() const {
    return accessContainer_->getDistributionStats();
  }

  // Provide stats on the number of keys cached and the access container for
  // this cache.
  using AccessStats = typename AccessContainer::Stats;
  AccessStats getAccessContainerStats() const {
    return accessContainer_->getStats();
  }

  // Get the total number of keys inserted into the access container
  uint64_t getAccessContainerNumKeys() const {
    return accessContainer_->getNumKeys();
  }

  // returns the reaper stats
  ReaperStats getReaperStats() const {
    auto stats = reaper_ ? reaper_->getStats() : ReaperStats{};
    return stats;
  }

  // returns the pool rebalancer stats
  RebalancerStats getRebalancerStats() const {
    auto stats =
        poolRebalancer_ ? poolRebalancer_->getStats() : RebalancerStats{};
    return stats;
  }

  // return the LruType of an item
  typename MMType::LruType getItemLruType(const Item& item) const;

  // return the recent slab release events for a pool for rebalancer, Resizer
  // and Memory monitor workers.
  AllSlabReleaseEvents getAllSlabReleaseEvents(PoolId pid) const override final;

  // get cache name
  const std::string getCacheName() const override final;

  // whether it is object-cache
  bool isObjectCache() const override final { return false; }

  // combined pool size for all memory tiers
  size_t getPoolSize(PoolId pid) const;

  // pool stats by pool id
  PoolStats getPoolStats(PoolId pid) const override final;

  // This can be expensive so it is not part of PoolStats
  PoolEvictionAgeStats getPoolEvictionAgeStats(
      PoolId pid, unsigned int slabProjectionLength) const override final;

  // return the cache's metadata
  CacheMetadata getCacheMetadata() const noexcept override final;

  // return the overall cache stats
  GlobalCacheStats getGlobalCacheStats() const override final;

  // return cache's memory usage stats
  CacheMemoryStats getCacheMemoryStats() const override final;

  // Return slab memory base address and size; useful for APIs that want to
  // pre-register buffers to avoid IO overhead (e.g., mapping pages in kernel).
  std::pair<const void*, size_t> getSlabMemoryInfo() const noexcept {
    return allocator_->getSlabMemoryInfo();
  }

  // return the nvm cache stats map
  util::StatsMap getNvmCacheStatsMap() const override final;

  // return the event tracker stats map
  std::unordered_map<std::string, uint64_t> getLegacyEventTrackerStatsMap()
      const override {
    std::unordered_map<std::string, uint64_t> legacyEventTrackerStats;
    if (auto legacyEventTracker = getLegacyEventTracker()) {
      legacyEventTracker->getStats(legacyEventTrackerStats);
    }
    return legacyEventTrackerStats;
  }

  folly::F14FastMap<std::string, uint64_t> getEventTrackerStatsMap()
      const override {
    folly::F14FastMap<std::string, uint64_t> eventTrackerStats;
    if (auto* eventTracker = getEventTracker()) {
      eventTracker->getStats(eventTrackerStats);
    }
    return eventTrackerStats;
  }

  // Set the event tracker for the cache allocator.
  // This overrides the base class method to also propagate the event tracker
  // to the NVM cache if it is enabled.
  void setEventTracker(EventTracker::Config&& config) override {
    // Call the base class method to set the event tracker
    CacheBase::setEventTracker(std::move(config));

    // If NVM cache is enabled, also set the event tracker there
    if (nvmCache_ && nvmCache_->isEnabled()) {
      if (auto* et = getEventTracker()) {
        nvmCache_->setEventTracker(et);
      }
    }
  }

  // Whether this cache allocator was created on shared memory.
  bool isOnShm() const noexcept { return isOnShm_; }

  // Whether NvmCache is currently enabled
  bool isNvmCacheEnabled() const noexcept {
    return nvmCache_ && nvmCache_->isEnabled();
  }

  // unix timestamp when the cache was created. If 0, the cache creation
  // time was not recorded from the older version of the binary.
  //
  // @return  time when the cache was created.
  time_t getCacheCreationTime() const noexcept { return cacheCreationTime_; }
  time_t getNVMCacheCreationTime() const {
    return nvmCacheState_.getCreationTime();
  }

  // Inspects the cache without changing its state.
  //
  // @param key     for the cache item
  // @return  std::pair<ReadHandle, ReadHandle> the first represents the state
  //          in the RAM and the second is a copy of the state in NVM
  std::pair<ReadHandle, ReadHandle> inspectCache(Key key);

  // blocks until the inflight operations are flushed to nvmcache. Used for
  // benchmarking when we want to load up the cache first with some data and
  // run the benchmarks after flushing.
  void flushNvmCache();

  // Writes a RAM-resident item to NVM without evicting it (write-through).
  // The item stays in RAM, and a successful put marks it NvmClean so its later
  // eviction skips the write. The write is queued asynchronously, goes through
  // the normal admission checks, and is a no-op for an item already NvmClean.
  //
  // Do not call this right after insertOrReplace on the same key: put() aborts
  // on the NVM delete tombstone that insertOrReplace enqueues
  // (numNvmAbortedPutOnTombstone). Retry on a later turn instead.
  //
  // @return true if a put was queued with a valid put token. NvmCache::put can
  //         still drop it, so callers that need a durable copy should check
  //         Item::isNvmClean().
  bool pushToNvmCacheFromRam(Key key);

  // Dump the last N items for an MM Container
  // @return  vector of the string of each item. Empty if nothing in LRU
  // @throw  std::invalid_argument if <pid, cid> does not exist
  std::vector<std::string> dumpEvictionIterator(PoolId pid,
                                                ClassId cid,
                                                size_t numItems = 10);

  // returns the current count of the active handles that are handed out
  // through the API. This also includes the handles that are maintained by
  // the iterator and internal rebalancing threads.
  int64_t getNumActiveHandles() const;

  // returns the current count of handle at an given time for this thread. If
  // the threads do not transfer handles between them, the caller can rely on
  // this being the current active outstanding handles. If handles can be
  // transferred, then the caller needs to take snapshot of the count before
  // and after and make sure that the count reflects any transfer.
  //
  // TODO (sathya) wrap this into a guard object
  //
  // @return  the current handle count. If handles are never transferred
  //          between threads, this will always be >=0. If handles are
  //          transferred, this can be negative  on threads that give away
  //          handles and always non zero on threads that acquire a handle from
  //          another thread.
  int64_t getHandleCountForThread() const;

  // (Deprecated) reset and adjust handle count for the current thread.
  // Please do not use this API as it will be moved to a private function.
  void resetHandleCountForThread_private();
  void adjustHandleCountForThread_private(int64_t delta);

  // madvise(MADV_DODUMP) all recently accessed items.
  // this function is intended for signal handler which can mprotect other
  // code. Hence, it is  inlined to make sure that the code lives in the
  // caller's text segment
  void FOLLY_ALWAYS_INLINE madviseRecentlyAccessedItems() const {
    for (const auto& tlring : ring_.accessAllThreads()) {
      tlring.madviseItems();
    }
  }

  // Mark the item as dirty and enqueue for deletion from nvmcache
  // @param item         item to invalidate.
  void invalidateNvm(Item& item);

  // Attempts to clean up left-over shared memory from preivous instance of
  // cachelib cache for the cache directory. If there are other processes
  // using the same directory, we don't touch it. If the directory is not
  // present, we do our best to clean up based on what is possible.
  // It is hard to determine if we actually cleaned up something.
  //
  // returns true if there was no error in trying to cleanup the segment
  // because another process was attached. False if the user tried to clean up
  // and the cache was actually attached.
  static bool cleanupStrayShmSegments(const std::string& cacheDir,
                                      bool posix,
                                      const std::string& hugePageMountDir = "",
                                      const std::string& posixShmDir = "");

  // gives a relative offset to a pointer within the cache.
  uint64_t getItemPtrAsOffset(const void* ptr);

  // this ensures that we dont introduce any more hidden fields like vtable by
  // inheriting from the Hooks and their bool interface.
  static_assert((sizeof(typename MMType::template Hook<Item>) +
                 sizeof(typename AccessType::template Hook<Item>) +
                 sizeof(typename RefcountWithFlags::Value) + sizeof(uint32_t) +
                 sizeof(uint32_t) + sizeof(KAllocation)) == sizeof(Item),
                "vtable overhead");
  static_assert((20 + (3 * sizeof(CompressedPtrType))) == sizeof(Item),
                "item overhead is 32 bytes for 4 byte compressed pointer and "
                "35 bytes for 5 bytes compressed pointer.");

  // make sure there is no overhead in ChainedItem on top of a regular Item
  static_assert(sizeof(Item) == sizeof(ChainedItem),
                "Item and ChainedItem must be the same size");

  static_assert(std::is_standard_layout<KAllocation>::value,
                "KAllocation not standard layout");
  static_assert(std::is_standard_layout<ChainedItemPayload<CacheTrait>>::value,
                "ChainedItemPayload not standard layout");

// ensure that Item::alloc_ is the last member of Item. If not,
// Item::alloc_::data[0] will not work as a variable sized struct.
// gcc is strict about using offsetof in Item when Item has a default
// constructor, hence becoming a non-POD. Suppress -Winvalid-offsetof for
// this sake.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
  static_assert(sizeof(Item) - offsetof(Item, alloc_) == sizeof(Item::alloc_),
                "alloc_ incorrectly arranged");
#pragma GCC diagnostic pop

 private:
  // wrapper around Item's refcount and active handle tracking
  FOLLY_ALWAYS_INLINE RefcountWithFlags::IncResult incRef(Item& it);
  FOLLY_ALWAYS_INLINE RefcountWithFlags::Value decRef(Item& it);

  // drops the refcount and if needed, frees the allocation back to the memory
  // allocator and executes the necessary callbacks. no-op if it is nullptr.
  FOLLY_ALWAYS_INLINE void release(Item* it, bool isNascent);

  // Differtiate different memory setting for the initialization
  enum class InitMemType { kNone, kMemNew, kMemAttach };
  // instantiates a cache allocator for common initialization
  //
  // @param types         the type of the memory used
  // @param config        the configuration for the whole cache allocator
  CacheAllocator(InitMemType types, Config config);

  // This is the last step in item release. We also use this for the eviction
  // scenario where we have to do everything, but not release the allocation
  // to the allocator and instead recycle it for another new allocation. If
  // toRecycle is present, the item to be released and the item to be recycled
  // will be the same if we want to recycle a parent item. If we want to
  // recycle a child item, the parent would be the item to release and the
  // child will be the item to recycle. If it is a chained item, we simply
  // free it back.
  //
  // 1. It calls the remove callback
  // 2. It releases the item and chained allocation memory back to allocator
  //
  // An item should have its refcount dropped to zero, and unlinked from
  // access and mm container before being passed to this function.
  //
  // @param it    item to be released.
  // @param ctx   removal context
  // @param toRecycle  An item that will be recycled, this item is to be
  //                   ignored if it's found in the process of freeing
  //                   a chained allocation
  //
  // @return One of ReleaseRes. In all cases, _it_ is always released back to
  // the allocator unless an exception is thrown
  //
  // @throw   runtime_error if _it_ has pending refs or is not a regular item.
  //          runtime_error if parent->chain is broken
  enum class ReleaseRes {
    kRecycled,    // _it_ was released and _toRecycle_ was recycled
    kNotRecycled, // _it_ was released and _toRecycle_ was not recycled
    kReleased,    // toRecycle == nullptr and it was released
  };
  ReleaseRes releaseBackToAllocator(Item& it,
                                    RemoveContext ctx,
                                    bool nascent = false,
                                    const Item* toRecycle = nullptr);

  // acquires an handle on the item. returns an empty handle if it is null.
  // @param it    pointer to an item
  // @return WriteHandle   return a handle to this item
  // @throw std::overflow_error is the maximum item refcount is execeeded by
  //        creating this item handle.
  WriteHandle acquire(Item* it);

  using TryAcquireResult = typename AccessContainer::TryAcquireResult;

  // Non-blocking variant of acquire. Returns a handle and a TryAcquireResult
  // indicating success, eviction, or moving.
  // Safe to call while holding hash table bucket locks.
  std::pair<WriteHandle, TryAcquireResult> tryAcquire(Item* it);

  // creates an item handle with wait context.
  WriteHandle createNvmCacheFillHandle() { return WriteHandle{*this}; }

  // acquires the wait context for the handle. This is used by NvmCache to
  // maintain a list of waiters
  std::shared_ptr<WaitContext<ReadHandle>> getWaitContext(
      ReadHandle& hdl) const {
    return hdl.getItemWaitContext();
  }

  using MMContainerPtr = std::unique_ptr<MMContainer>;
  using MMContainers =
      std::array<std::array<MMContainerPtr, MemoryAllocator::kMaxClasses>,
                 MemoryPoolManager::kMaxPools>;

  // Shards 1..N-1 of one (pool, class) MM container when
  // config_.mmContainerShards > 1. Shard 0 is mmContainers_[pid][cid].
  struct MMContainerShardSet {
    std::vector<MMContainerPtr> extraShards;
    // Racy hint of each shard's tail update time: where its last eviction
    // scan stopped. Used to pick the shard to evict from without locking.
    std::unique_ptr<std::atomic<uint32_t>[]> tailTimes;
  };
  using MMContainerShardSets =
      std::array<std::array<MMContainerShardSet, MemoryAllocator::kMaxClasses>,
                 MemoryPoolManager::kMaxPools>;

  // A sharded cache stores its shard count above these bits of the persisted
  // MM type id, so binaries that predate sharding refuse to attach instead of
  // restoring shard 0 only.
  static constexpr unsigned int kMMTypeIdShardShift{16};

  void createMMContainers(const PoolId pid, MMConfig config);

  // Tail size of one shard of the MM container of (pool, cid), 0 unless tail
  // hits are tracked. The shards' tails add up to one slab's worth of items.
  size_t getMMContainerTailSize(const MemoryPool& pool,
                                ClassId cid,
                                uint32_t shard) const;

  void initMMContainerShardSet(PoolId pid, ClassId cid);

  bool isMMContainerSharded() const noexcept {
    return config_.mmContainerShards > 1;
  }

  // acquire the MMContainer corresponding to the the Item's class and pool.
  //
  // @return pointer to the MMContainer.
  // @throw  std::invalid_argument if the Item does not point to a valid
  // allocation from the memory allocator.
  MMContainer& getMMContainer(const Item& item) const noexcept;

  // same as above for an item already known to be in (pid, cid)
  MMContainer& getMMContainer(PoolId pid,
                              ClassId cid,
                              const Item& item) const noexcept;

  // For a sharded cache this is shard 0 only. Use forEachMMContainerShard()
  // for per-class operations.
  MMContainer& getMMContainer(PoolId pid, ClassId cid) const noexcept;

  MMContainer& getMMContainerShard(PoolId pid,
                                   ClassId cid,
                                   uint32_t shard) const noexcept;

  // Shard of the item within its (pool, class). Only valid when sharded.
  uint32_t getMMShardIndex(const Item& item) const noexcept;

  // Calls fn(MMContainer&) for every shard of (pid, cid) in shard order.
  template <typename Fn>
  void forEachMMContainerShard(PoolId pid, ClassId cid, Fn&& fn) const;

  // Picks the shard to evict from: of two random shards, the one whose tail
  // hint is older. Only valid when sharded.
  uint32_t pickEvictionShard(PoolId pid, ClassId cid) const noexcept;

  static void updateTailTimeHint(std::atomic<uint32_t>& hint,
                                 uint32_t time) noexcept {
    // skip redundant stores to keep the hint's cache line shared
    if (hint.load(std::memory_order_relaxed) != time) {
      hint.store(time, std::memory_order_relaxed);
    }
  }

  // MM container stats summed over shards; the oldest tail wins.
  MMContainerStat getMMContainerStat(PoolId pid, ClassId cid) const noexcept;

  // Fragmentation bytes for a class; a read racing alloc/free on other
  // threads can transiently underflow the thread-local sum, so clamp to 0.
  uint64_t fragmentationSizeStat(PoolId pid, ClassId cid) const {
    const uint64_t v = (*stats_.fragmentationSize)[pid][cid].get();
    return static_cast<int64_t>(v) < 0 ? 0 : v;
  }

  // Eviction age stats merged over shards.
  EvictionAgeStat getMMContainerEvictionAgeStat(
      PoolId pid, ClassId cid, uint64_t projectedLength) const noexcept;

  // create a new cache allocation. The allocation can be initialized
  // appropriately and made accessible through insert or insertOrReplace.
  // If the handle returned from this api is not passed on to
  // insert/insertOrReplace, the allocation gets destroyed when the handle
  // goes out of scope.
  //
  // @param id              the pool id for the allocation that was previously
  //                        created through addPool
  // @param key             the key for the allocation. This will be made a
  //                        part of the Item and be available through getKey().
  // @param size            the size of the allocation, exclusive of the key
  //                        size.
  // @param creationTime    Timestamp when this item was created
  // @param expiryTime      set an expiry timestamp for the item (0 means no
  //                        expiration time).
  // @param fromBgThread    whether this is called from BG thread
  //
  // @return      the handle for the item or an invalid handle(nullptr) if the
  //              allocation failed. Allocation can fail if one such
  //              allocation already exists or if we are out of memory and
  //              can not find an eviction. Handle must be destroyed *before*
  //              the instance of the CacheAllocator gets destroyed
  // @throw   std::invalid_argument if the poolId is invalid or the size
  //          requested is invalid or if the key is invalid(key.size() == 0 or
  //          key.size() > 255)
  WriteHandle allocateInternal(PoolId id,
                               Key key,
                               uint32_t size,
                               uint32_t creationTime,
                               uint32_t expiryTime,
                               bool fromBgThread = false);

  // Allocate a chained item
  //
  // The resulting chained item does not have a parent item and
  // will be freed once the handle is dropped
  //
  // The parent handle parameter here is mainly used to find the
  // correct pool to allocate memory for this chained item
  //
  // @param parent    the parent item
  // @param size      the size for the chained allocation
  //
  // @return    handle to the chained allocation
  // @throw     std::invalid_argument if the size requested is invalid or
  //            if the item is invalid
  WriteHandle allocateChainedItemInternal(const Item& parent, uint32_t size);

  // Given an existing item, allocate a new one for the
  // existing one to later be moved into.
  //
  // @param oldItem    the item we want to allocate a new item for
  //
  // @return  handle to the newly allocated item
  //
  WriteHandle allocateNewItemForOldItem(const Item& oldItem);

  // internal helper that grabs a refcounted handle to the item. This does
  // not record the access to reflect in the mmContainer.
  //
  // @param key   key to look up in the access container
  //
  // @return handle if item is found, nullptr otherwise
  //
  // @throw std::overflow_error is the maximum item refcount is execeeded by
  //        creating this item handle.
  WriteHandle findInternal(Key key) {
    // Note: this method can not be const because we need  a non-const
    // reference to create the ItemReleaser.
    return accessContainer_->find(key);
  }

  // Safely obtain a refcounted handle from a raw pointer
  //
  // @param rawItem              raw pointer
  // @param allocSize            allocation size for the slot containing rawItem
  // @param wantExpiredItems     whether the caller is looking exclusively for
  //                             expired items or exclusively non-expired items
  // @param expirationCheckTime  optional time to use for expiration check
  //
  // @return  a valid WriteHandle if the memory contains an accessible item,
  //          empty handle otherwise.
  FOLLY_DISABLE_ADDRESS_SANITIZER WriteHandle
  findFromRawAlloc(const void* rawItem,
                   uint32_t allocSize,
                   bool wantExpiredItems = false,
                   std::optional<uint32_t> expirationCheckTime = {}) {
    if (!allocator_->isValidAllocMemory(rawItem)) {
      return {};
    }
    auto item = reinterpret_cast<const Item*>(rawItem);

    if (!item->isAccessibleNoAsan()) {
      return {};
    }

    auto isExpired = expirationCheckTime.has_value()
                         ? item->isExpiredNoAsan(expirationCheckTime.value())
                         : item->isExpiredNoAsan();
    // The caller determines how we check expiration. There's 2 cases:
    // 1. The caller *only* wants expired items (e.g., Reaper)
    // 2. The caller *does not* want expired items (e.g., getSampleItem())
    if (wantExpiredItems && !isExpired) {
      return {};
    } else if (!wantExpiredItems && isExpired) {
      return {};
    }

    auto key = item->getKeyCheckedNoAsan(allocSize);
    // Validate the entire key is in slab memory: rawItem < key start < key end,
    // only need to validate end of key
    if (key.empty() ||
        !allocator_->isValidAllocMemory(key.data() + key.size() - 1)) {
      return {};
    }
#if FOLLY_SANITIZE_ADDRESS
    char stackBuf[KAllocation::kKeyMaxLenSmall];
    std::unique_ptr<char[]> heapBuf;
    if (config_.isSlabAsanPoisoningEnabled()) {
      // Copy the key out so we can use it without disabling instrumentation for
      // the whole find path. Don't use std utilities (like std::string or
      // std::memcpy) because they still have ASAN enabled. If it's a small key,
      // store on the stack. Otherwise malloc on the heap.
      char* buffer;
      if (key.size() <= KAllocation::kKeyMaxLenSmall) {
        buffer = stackBuf;
      } else {
        heapBuf = std::unique_ptr<char[]>(new char[key.size()]);
        buffer = heapBuf.get();
      }
      for (size_t i = 0; i < key.size(); i++) {
        buffer[i] = key[i];
      }
      key = Key(buffer, key.size());
    }
#endif
    if (Item::getRequiredSize(key, /* size */ 0) > allocSize) {
      return {};
    }

    auto handle = findInternal(key);
    if (handle.get() != item) {
      return {};
    }

    return handle;
  }

  // TODO: do another round of audit to refactor our lookup paths. This is
  //       still convoluted.
  //
  // internal helper that grabs a refcounted handle to the item. This does
  // not record the access to reflect in the mmContainer. This also checks
  // expiration and also bumps stats if caller is a regular find or findFast.
  //
  // @param key     key to look up in the access container
  // @param event   cachelib lookup operation
  //
  // @return handle if item is found and not expired, nullptr otherwise
  //
  // @throw std::overflow_error is the maximum item refcount is execeeded by
  //        creating this item handle.
  WriteHandle findInternalWithExpiration(Key key, AllocatorApiEvent event);

  // look up an item by its key across the nvm cache as well if enabled.
  //
  // @param key         the key for lookup
  // @param mode        the mode of access for the lookup.
  //                    AccessMode::kRead or AccessMode::kWrite
  //
  // @return      the handle for the item or a handle to nullptr if the key does
  //              not exist.
  FOLLY_ALWAYS_INLINE WriteHandle findImpl(Key key, AccessMode mode);

  // look up an item by its key. This ignores the nvm cache and only does RAM
  // lookup.
  //
  // @param key         the key for lookup
  // @param mode        the mode of access for the lookup.
  //                    AccessMode::kRead or AccessMode::kWrite
  //
  // @return      the handle for the item or a handle to nullptr if the key does
  //              not exist.
  FOLLY_ALWAYS_INLINE WriteHandle findFastImpl(Key key, AccessMode mode);

  // Moves a regular item to a different slab. This should only be used during
  // slab release after the item's exclusive bit has been set. The user supplied
  // callback is responsible for copying the contents and fixing the semantics
  // of chained item.
  //
  // @param oldItem     Reference to the item being moved
  // @param newItemHdl  Reference to the handle of the new item being moved into
  //
  // @return true  If the move was completed, and the containers were updated
  //               successfully.
  bool moveRegularItem(Item& oldItem, WriteHandle& newItemHdl);

  // template class for viewAsChainedAllocs that takes either ReadHandle or
  // WriteHandle
  template <typename Handle, typename Iter>
  CacheChainedAllocs<CacheT, Handle, Iter> viewAsChainedAllocsT(
      const Handle& parent);

  // return an iterator to the item's chained allocations. The order of
  // iteration on the item will be LIFO of the addChainedItem calls.
  template <typename Iter>
  folly::Range<Iter> viewAsChainedAllocsRangeT(const Item& parent) const;

  // template class for convertToIOBuf that takes either ReadHandle or
  // WriteHandle
  template <typename Handle>
  folly::IOBuf convertToIOBufT(Handle& handle);

  // Moves a chained item to a different slab. This should only be used during
  // slab release after the item's exclusive bit has been set. The user supplied
  // callback is responsible for copying the contents and fixing the semantics
  // of chained item.
  //
  // Note: If we have successfully moved the old item into the new, the
  //       newItemHdl is reset and no longer usable by the caller.
  //
  // @param oldItem       Reference to the item being moved
  // @param newItemHdl    Reference to the handle of the new item being
  //                      moved into
  //
  // @return true  If the move was completed, and the containers were updated
  //               successfully.
  bool moveChainedItem(ChainedItem& oldItem, WriteHandle& newItemHdl);

  // Transfers the chain ownership from parent to newParent. Parent
  // will be unmarked as having chained allocations. Parent will not be null
  // after calling this API.
  //
  // Parent and NewParent must be valid handles to items with same key and
  // parent must have chained items and parent handle must be the only
  // outstanding handle for parent. New parent must be without any chained item
  // handles.
  //
  // Chained item lock for the parent's key needs to be held in exclusive mode.
  //
  // @param parent    the current parent of the chain we want to transfer
  // @param newParent the new parent for the chain
  //
  // @throw if any of the conditions for parent or newParent are not met.
  void transferChainLocked(Item& parent, Item& newParent);

  // replace a chained item in the existing chain. This needs to be called
  // with the chained item lock held exclusive
  //
  // @param oldItem  the item we are replacing in the chain
  // @param newItem  the item we are replacing it with
  // @param parent   the parent for the chain
  //
  // @return handle to the oldItem
  WriteHandle replaceChainedItemLocked(Item& oldItem,
                                       WriteHandle newItemHdl,
                                       const Item& parent);

  //
  // Performs the actual inplace replace - it is called from
  // moveChainedItem and replaceChainedItemLocked
  // must hold chainedItemLock
  //
  // @param oldItem  the item we are replacing in the chain
  // @param newItem  the item we are replacing it with
  // @param parent   the parent for the chain
  // @param fromMove used to determine if the replaced was called from
  //                 moveChainedItem - we avoid the handle destructor
  //                 in this case.
  //
  // @return handle to the oldItem
  void replaceInChainLocked(Item& oldItem,
                            WriteHandle& newItemHdl,
                            const Item& parent,
                            bool fromMove);

  // Insert an item into MM container. The caller must hold a valid handle for
  // the item.
  //
  // @param  item  Item that we want to insert.
  //
  // @throw std::runtime_error if the handle is already in the mm container
  void insertInMMContainer(Item& item);

  // Removes an item from the corresponding MMContainer if it is in the
  // container. The caller must hold a valid handle for the item.
  //
  // @param  item  Item that we want to remove.
  //
  // @return true   if the item is successfully removed
  //         false  if the item is not in MMContainer
  bool removeFromMMContainer(Item& item);

  // Replaces an item in the MMContainer with another item, at the same
  // position.
  //
  // @param oldItem   item being replaced
  // @param newItem   item to replace oldItem with
  //
  // @return true  If the replace was successful. Returns false if the
  //               destination item did not exist in the container, or if the
  //               source item already existed.
  bool replaceInMMContainer(Item& oldItem, Item& newItem);

  // Replaces an item in the MMContainer with another item, at the same
  // position. Or, if the two chained items belong to two different MM
  // containers, remove the old one from its MM container and add the new
  // one to its MM container.
  //
  // @param oldItem   item being replaced
  // @param newItem   item to replace oldItem with
  //
  // @return true  If the replace was successful. Returns false if the
  //               destination item did not exist in the container, or if the
  //               source item already existed.
  bool replaceChainedItemInMMContainer(Item& oldItem, Item& newItem);

  // Only replaces an item if it is accessible
  bool replaceIfAccessible(Item& oldItem, Item& newItem);

  // Inserts the allocated handle into the AccessContainer, making it
  // accessible for everyone. This needs to be the handle that the caller
  // allocated through _allocate_. If this call fails, the allocation will be
  // freed back when the handle gets out of scope in the caller.
  //
  // @param handle the handle for the allocation.
  // @param event AllocatorApiEvent that corresponds to the current operation.
  //              supported events are INSERT, corresponding to the client
  //              insert call, and INSERT_FROM_NVM, cooresponding to the insert
  //              call that happens when an item is promoted from NVM storage
  //              to memory.
  //
  // @return true if the handle was successfully inserted into the hashtable
  //         and is now accessible to everyone. False if there was an error.
  //
  // @throw std::invalid_argument if the handle is already accessible or invalid
  bool insertImpl(const WriteHandle& handle, AllocatorApiEvent event);

  // Removes an item from the access container and MM container.
  //
  // @param hk               the hashed key for the item
  // @param it               Item to remove
  // @param tombstone        A tombstone for nvm::remove job created by
  //                         nvm::createDeleteTombStone, can be empty if nvm is
  //                         not enable, or removeFromNvm is false
  // @param removeFromNvm    if true clear key from nvm
  // @param recordApiEvent   should we record API event for this operation.
  RemoveRes removeImpl(HashedKey hk,
                       Item& it,
                       DeleteTombStoneGuard tombstone,
                       bool removeFromNvm = true,
                       bool recordApiEvent = true);

  // Must be called by the thread which called markForEviction and
  // succeeded. After this call, the item is unlinked from Access and
  // MM Containers. The item is no longer marked as exclusive and it's
  // ref count is 0 - it's available for recycling.
  void unlinkItemForEviction(Item& it);

  // Implementation to find a suitable eviction from the container. The
  // two parameters together identify a single container.
  //
  // @param  pid  the id of the pool to look for evictions inside
  // @param  cid  the id of the class to look for evictions inside
  // @return An evicted item or nullptr  if there is no suitable candidate found
  // within the configured number of attempts.
  Item* findEviction(PoolId pid, ClassId cid);

  // Get next eviction candidate from MMContainer, remove from AccessContainer,
  // MMContainer and insert into NVMCache if enabled.
  //
  // @param pid  the id of the pool to look for evictions inside
  // @param cid  the id of the class to look for evictions inside
  // @param searchTries number of search attempts so far.
  //
  // @return pair of [candidate, toRecycle]. Pair of null if reached the end of
  // the eviction queue or no suitable candidate found
  // within the configured number of attempts
  std::pair<Item*, Item*> getNextCandidate(PoolId pid,
                                           ClassId cid,
                                           unsigned int& searchTries);

  using EvictionIterator = typename MMContainer::LockedIterator;

  // Wakes up waiters if there are any
  //
  // @param item    wakes waiters that are waiting on that item
  // @param handle  handle to pass to the waiters
  void wakeUpWaiters(folly::StringPiece key, WriteHandle handle);

  // Unmarks item as moving and wakes up any waiters waiting on that item
  //
  // @param item    wakes waiters that are waiting on that item
  // @param handle  handle to pass to the waiters
  typename RefcountWithFlags::Value unmarkMovingAndWakeUpWaiters(
      Item& item, WriteHandle handle);

  // Deserializer CacheAllocatorMetadata and verify the version
  //
  // @param  deserializer   Deserializer object
  // @throws                runtime_error
  serialization::CacheAllocatorMetadata deserializeCacheAllocatorMetadata(
      Deserializer& deserializer);

  MMContainers deserializeMMContainers(
      Deserializer& deserializer,
      const typename Item::PtrCompressor& compressor);

  // Throws if any restored (pool, class) is missing some of its shards.
  void validateMMContainerShards(const MMContainers& mmContainers) const;

  unsigned int reclaimSlabs(PoolId id, size_t numSlabs) final {
    return allocator_->reclaimSlabsAndGrow(id, numSlabs);
  }

  FOLLY_ALWAYS_INLINE LegacyEventTracker* getLegacyEventTracker() const {
    return config_.legacyEventTracker.get();
  }

  // Helper function to calculate time to expire
  static uint32_t calculateTimeToExpire(time_t expiryTime, time_t currentTime) {
    if (expiryTime > currentTime) {
      return static_cast<uint32_t>(expiryTime - currentTime);
    }
    return 0;
  }

  // Returns true iff a configured tracker would consume this event. Hot-path
  // gate that lets callers skip both the sampling-related work and any
  // EventRecordParams construction (e.g., getAllocInfo) when no tracker is
  // configured or sampling rejects the key.
  FOLLY_ALWAYS_INLINE bool shouldRecordEvent(AllocatorApiEvent event,
                                             Key key) const {
    if (auto* eventTracker = getEventTracker()) {
      return eventTracker->shouldRecordEvent(event, key);
    }
    return getLegacyEventTracker() != nullptr;
  }

  /**
   * Record event, key, result and info from a struct of type EventRecordParams.
   *
   * Usage:
   *  recordEvent(event, key, result);
   *  recordEvent(event, key, result, {.size = itemSize});
   *  recordEvent(event, key, result, {.size=itemSize, .expiryTime=expiry});
   *
   * @param event            The event of type AllocatorApiEvent.
   * @param key              The key associated with the event.
   * @param result           The result of type AllocatorApiResult.
   * @param params           Optional struct of type EventRecordParams.
   *
   * @return                 void
   */
  void recordEvent(AllocatorApiEvent event,
                   Key key,
                   AllocatorApiResult result,
                   EventRecordParams params = {}) const {
    if (shouldRecordEvent(event, key)) {
      recordEventWithoutSampling(event, key, result, std::move(params));
    }
  }

  // Record event without calling shouldRecordEvent(). Use when the caller has
  // already determined that this event and key should be sampled (e.g.,
  // NvmCache::recordEvent() checks once and uses this to avoid
  // double-sampling).
  void recordEventWithoutSampling(AllocatorApiEvent event,
                                  Key key,
                                  AllocatorApiResult result,
                                  EventRecordParams params = {}) const {
    if (auto* eventTracker = getEventTracker()) {
      EventInfo eventInfo;
      eventInfo.eventTimestamp = util::getCurrentTimeSec();
      eventInfo.event = event;
      eventInfo.result = result;
      eventInfo.key = key;
      if (params.size) {
        eventInfo.size = *params.size;
      }
      if (params.expiryTime) {
        eventInfo.expiryTime = *params.expiryTime;
        eventInfo.timeToExpire =
            calculateTimeToExpire(*params.expiryTime, eventInfo.eventTimestamp);
      }
      if (params.ttlSecs && *params.ttlSecs > 0) {
        eventInfo.ttlSecs = *params.ttlSecs;
      }
      if (params.allocSize) {
        eventInfo.allocSize = *params.allocSize;
      }
      if (params.poolId) {
        eventInfo.poolId = *params.poolId;
      }

      eventTracker->recordWithoutSampling(std::move(eventInfo));
    } else if (auto legacyEventTracker = getLegacyEventTracker()) {
      folly::Optional<uint32_t> size =
          params.size
              ? folly::Optional<uint32_t>(static_cast<uint32_t>(*params.size))
              : folly::none;
      uint32_t ttl = params.ttlSecs ? *params.ttlSecs : 0;
      legacyEventTracker->record(event, key, result, size, ttl);
    }
  }

  template <typename HandleT>
  EventRecordParams makeEventRecordParams(const HandleT& handle) const {
    const auto allocInfo = allocator_->getAllocInfo(handle->getMemory());
    return EventRecordParams{
        .size = handle->getSize(),
        .ttlSecs = static_cast<uint32_t>(handle->getConfiguredTTL().count()),
        .expiryTime = handle->getExpiryTime(),
        .allocSize = allocInfo.allocSize,
        .poolId = allocInfo.poolId};
  }

  /**
   * Record event, key, result and info from a cache handle.
   *
   * @tparam HandleT         Cache handle type (e.g., ReadHandle, WriteHandle).
   * @param event            The event of type AllocatorApiEvent.
   * @param key              The key associated with the event.
   * @param result           The result of type AllocatorApiResult.
   * @param handle           The cache handle to extract metadata from.
   *
   * @return                 void
   */
  template <typename HandleT>
  void recordEvent(AllocatorApiEvent event,
                   Key key,
                   AllocatorApiResult result,
                   const HandleT& handle) const {
    if (!shouldRecordEvent(event, key)) {
      return;
    }
    if (handle) {
      recordEventWithoutSampling(event, key, result,
                                 makeEventRecordParams(handle));
    } else {
      recordEventWithoutSampling(event, key, result);
    }
  }

 public:
  // Releases a slab from a pool into its corresponding memory pool
  // or back to the slab allocator, depending on SlabReleaseMode.
  //  SlabReleaseMode::kRebalance -> back to the pool
  //  SlabReleaseMode::kResize -> back to the slab allocator
  //
  // @param pid        Pool that will make make one slab available
  // @param cid        Class that will release a slab back to its pool
  //                   or slab allocator
  // @param mode       the mode for slab release (rebalance/resize)
  // @param hint       hint referring to the slab. this can be an allocation
  //                   that the user knows to exist in the slab. If this is
  //                   nullptr, a random slab is selected from the pool and
  //                   allocation class.
  //
  // @throw std::invalid_argument if the hint is invalid or if the pid or cid
  //        is invalid.
  // @throw std::runtime_error if fail to release a slab due to internal error
  void releaseSlab(PoolId pid,
                   ClassId cid,
                   SlabReleaseMode mode,
                   const void* hint = nullptr) final;

  // Releasing a slab from this allocation class id and pool id. The release
  // could be for a pool resizing or allocation class rebalancing.
  //
  // All active allocations will be evicted in the process.
  //
  // This function will be blocked until a slab is freed
  //
  // @param pid        Pool that will make one slab available
  // @param victim     Class that will release a slab back to its pool
  //                   or slab allocator or a receiver if defined
  // @param receiver   Class that will receive when the mode is set to rebalance
  // @param mode  the mode for slab release (rebalance/resize)
  //              the mode must be rebalance if an valid receiver is specified
  // @param hint  hint referring to the slab. this can be an allocation
  //              that the user knows to exist in the slab. If this is
  //              nullptr, a random slab is selected from the pool and
  //              allocation class.
  //
  // @throw std::invalid_argument if the hint is invalid or if the pid or cid
  //        is invalid. Or if the mode is set to kResize but the receiver is
  //        also specified. Receiver class id can only be specified if the mode
  //        is set to kRebalance.
  // @throw std::runtime_error if fail to release a slab due to internal error
  void releaseSlab(PoolId pid,
                   ClassId victim,
                   ClassId receiver,
                   SlabReleaseMode mode,
                   const void* hint = nullptr) final;

 private:
  // @param releaseContext  slab release context
  void releaseSlabImpl(const SlabReleaseContext& releaseContext);

  // @return  true when successfully marked as moving,
  //          fasle when this item has already been freed
  bool markMovingForSlabRelease(const SlabReleaseContext& ctx,
                                void* alloc,
                                util::Throttler& throttler);

  // "Move" (by copying) the content in this item to another memory
  // location by invoking the move callback.
  // @param item        old item to be moved elsewhere
  // @return    true  if the item has been moved
  //            false if we have exhausted moving attempts
  bool moveForSlabRelease(Item& item);

  // Evict an item from access and mm containers and
  // ensure it is safe for freeing.
  //
  // @param item        old item to be moved elsewhere
  void evictForSlabRelease(Item& item);

  // Helper function to create PutToken
  //
  // @return valid token if the item should be written to NVM cache.
  typename NvmCacheT::PutToken createPutToken(Item& item);

  // Helper function to remove a item if expired.
  //
  // @return true if it item expire and removed successfully.
  bool removeIfExpired(const ReadHandle& handle);

  // exposed for the Reaper to iterate through the memory and find items to
  // reap under the super charged mode. This is faster if there are lots of
  // items in cache and only a small fraction of them are expired at any given
  // time.
  template <typename Fn>
  void traverseAndExpireItems(Fn&& f) {
    // The intent here is to scan the memory to identify candidates for reaping
    // without holding any locks. Candidates that are identified as potential
    // ones are further processed by holding the right synchronization
    // primitives. So we consciously exempt ourselves here from TSAN data race
    // detection.
    folly::annotate_ignore_thread_sanitizer_guard g(__FILE__, __LINE__);
    auto slabsSkipped = allocator_->forEachAllocation(std::forward<Fn>(f));
    stats().numReaperSkippedSlabs.add(slabsSkipped);
  }

  // exposed for the background evictor to iterate through the memory and evict
  // in batch. This should improve insertion path for tiered memory config
  size_t traverseAndEvictItems(unsigned int /* pid */,
                               unsigned int /* cid */,
                               size_t /* batch */) {
    throw std::runtime_error("Not supported yet!");
  }

  // exposed for the background promoter to iterate through the memory and
  // promote in batch. This should improve find latency
  size_t traverseAndPromoteItems(unsigned int /* pid */,
                                 unsigned int /* cid */,
                                 size_t /* batch */) {
    throw std::runtime_error("Not supported yet!");
  }

  // returns true if nvmcache is enabled and we should write this item to
  // nvmcache.
  bool shouldWriteToNvmCache(const Item& item);

  // (this should be only called when we're the only thread accessing item)
  // returns true if nvmcache is enabled and we should write this item.
  bool shouldWriteToNvmCacheExclusive(const Item& item);

  // Returns true if the item has an unmodified copy in BlockCache whose
  // access time should be updated in the Access Time Map on eviction.
  bool shouldUpdateAccessTimeMap(const Item& item) const;

  // Serialize the metadata for the cache into an IOBUf. The caller can now
  // use this to serialize into a serializer by estimating the size and
  // calling writeToBuffer.
  folly::IOBufQueue saveStateToIOBuf();

  static typename MemoryAllocator::Config getAllocatorConfig(
      const Config& config) {
    return MemoryAllocator::Config{
        config.defaultAllocSizes.empty()
            ? util::generateAllocSizes(
                  config.allocationClassSizeFactor,
                  config.maxAllocationClassSize,
                  config.minAllocationClassSize,
                  config.reduceFragmentationInAllocationClass)
            : config.defaultAllocSizes,
        config.enableZeroedSlabAllocs,
        config.disableFullCoredump,
        config.lockMemory,
        config.isSlabAsanPoisoningEnabled(),
        config.slabHugePageSize};
  }

  // starts one of the cache workers passing the current instance and the args
  template <typename T, typename... Args>
  bool startNewWorker(folly::StringPiece name,
                      std::unique_ptr<T>& worker,
                      std::chrono::milliseconds interval,
                      Args&&... args);

  // stops one of the workers belonging to this instance.
  template <typename T>
  bool stopWorker(folly::StringPiece name,
                  std::unique_ptr<T>& worker,
                  std::chrono::seconds timeout = std::chrono::seconds{0});

  ShmSegmentOpts createShmCacheOpts();

  // Builds the shm options for an access-container (hash table) segment,
  ShmSegmentOpts createShmAccessOpts();

  // Validates and applies a requested huge page size (bytes; 0 == normal) onto
  // opts, throwing if the kernel does not support the size.
  void applyHugePageOpts(ShmSegmentOpts& opts, const PageSize& pageSize) const;

  std::unique_ptr<MemoryAllocator> createNewMemoryAllocator();
  std::unique_ptr<MemoryAllocator> restoreMemoryAllocator();
  std::unique_ptr<CCacheManager> restoreCCacheManager();

  void reclaimUnmonitoredSlabs(const MemoryMonitor::Config& config,
                               std::chrono::milliseconds interval);

  PoolIds filterCompactCachePools(const PoolIds& poolIds) const;

  // returns a list of pools excluding compact cache pools that are over the
  // limit
  PoolIds getRegularPoolIdsForResize() const override final;

  // return whether a pool participates in auto-resizing
  bool autoResizeEnabledForPool(PoolId) const override final;

  // resize all compact caches to their configured size
  void resizeCompactCaches() override;

  std::map<std::string, std::string> serializeConfigParams()
      const override final {
    auto configMap = [this] {
      // startNewMemMonitor()/stopMemMonitor() update the memory monitor
      // fields of config_ under workersMutex_. The other fields don't need
      // synchronization.
      std::lock_guard<std::mutex> l(workersMutex_);
      return config_.serialize();
    }();

    auto exportRebalanceStrategyConfig = [this](PoolId poolId) {
      auto strategy = getRebalanceStrategy(poolId);
      if (!strategy) {
        return std::map<std::string, std::string>{};
      }
      return strategy->exportConfig();
    };

    auto regularPoolIds = getRegularPoolIds();
    std::map<std::string, std::map<std::string, std::string>>
        mapRebalancePolicyConfigs;
    for (const auto poolId : regularPoolIds) {
      auto poolName = getPoolName(poolId);
      mapRebalancePolicyConfigs[poolName] =
          exportRebalanceStrategyConfig(poolId);
    }
    configMap["rebalance_policy_configs"] =
        folly::json::serialize(folly::toDynamic(mapRebalancePolicyConfigs), {});

    return configMap;
  }

  typename Item::PtrCompressor createPtrCompressor() const {
    return allocator_
        ->createPtrCompressor<Item, typename Item::CompressedPtrType>();
  }

  // helper utility to throttle and optionally log.
  static void throttleWith(util::Throttler& t, std::function<void()> fn);

  // Write the item to nvm cache if enabled. This is called on eviction.
  void pushToNvmCache(const Item& item);

  // Test utility function to move a key from ram into nvmcache.
  bool pushToNvmCacheFromRamForTesting(Key key);
  // Test utility functions to remove things from individual caches.
  bool removeFromRamForTesting(Key key);
  void removeFromNvmForTesting(Key key);

  // @param dramCacheAttached   boolean indicating if the dram cache was
  //                            restored from previous state
  void initCommon(bool dramCacheAttached);
  void initNvmCache(bool dramCacheAttached);
  void initWorkers();

  // @param type        the type of initialization
  // @return nullptr if the type is invalid
  // @return pointer to memory allocator
  // @throw std::runtime_error if type is invalid
  std::unique_ptr<MemoryAllocator> initAllocator(InitMemType type);
  // @param type        the type of initialization
  // @return nullptr if the type is invalid
  // @return pointer to access container
  // @throw std::runtime_error if type is invalid
  std::unique_ptr<AccessContainer> initAccessContainer(InitMemType type,
                                                       const std::string name,
                                                       AccessConfig config);

  std::optional<bool> saveNvmCache();
  void saveRamCache();

  static bool itemExclusivePredicate(const Item& item) {
    return item.getRefCount() == 0;
  }

  static bool itemExpiryPredicate(const Item& item) {
    return item.getRefCount() == 1 && item.isExpired();
  }

  static bool parentEvictForSlabReleasePredicate(const Item& item) {
    return item.getRefCount() == 1 && !item.isMoving();
  }

  std::unique_ptr<Deserializer> createDeserializer();

  // Execute func on each item. `func` can throw exception but must ensure
  // the item remains in a valid state
  //
  // @param item      Parent item
  // @param func      Function that gets executed on each chained item
  void forEachChainedItem(const Item& item,
                          std::function<void(ChainedItem&)> func);

  // @param item    Record the item has been accessed in its mmContainer
  // @param mode    the mode of access
  // @param stats   stats object to avoid a thread local lookup.
  // @return true   if successfully recorded in MMContainer
  bool recordAccessInMMContainer(Item& item, AccessMode mode);

  WriteHandle findChainedItem(const Item& parent) const;

  // Get the thread local version of the Stats
  detail::Stats& stats() const noexcept { return stats_; }

  void initStats();

  folly::Range<ChainedItemIter> viewAsChainedAllocsRange(
      const Item& parent) const {
    return viewAsChainedAllocsRangeT<ChainedItemIter>(parent);
  }

  folly::Range<WritableChainedItemIter> viewAsWritableChainedAllocsRange(
      const Item& parent) const {
    return viewAsChainedAllocsRangeT<WritableChainedItemIter>(parent);
  }

  // updates the maxWriteRate for DynamicRandomAdmissionPolicy
  // returns true if update successfully
  //         false if no AdmissionPolicy is set or it is not DynamicRandom
  bool updateMaxRateForDynamicRandomAP(uint64_t maxRate) {
    return nvmCache_ ? nvmCache_->updateMaxRateForDynamicRandomAP(maxRate)
                     : false;
  }

  // returns the background mover stats
  BackgroundMoverStats getBackgroundMoverStats(MoverDir direction) const {
    auto stats = BackgroundMoverStats{};
    if (direction == MoverDir::Evict) {
      for (auto& bg : backgroundEvictor_) {
        stats += bg->getStats();
      }
    } else if (direction == MoverDir::Promote) {
      for (auto& bg : backgroundPromoter_) {
        stats += bg->getStats();
      }
    }
    return stats;
  }

  std::map<PoolId, std::map<ClassId, uint64_t>> getBackgroundMoverClassStats(
      MoverDir direction) const {
    std::map<PoolId, std::map<ClassId, uint64_t>> stats;

    if (direction == MoverDir::Evict) {
      for (auto& bg : backgroundEvictor_) {
        for (auto& pid : bg->getClassStats()) {
          for (auto& cid : pid.second) {
            stats[pid.first][cid.first] += cid.second;
          }
        }
      }
    } else if (direction == MoverDir::Promote) {
      for (auto& bg : backgroundPromoter_) {
        for (auto& pid : bg->getClassStats()) {
          for (auto& cid : pid.second) {
            stats[pid.first][cid.first] += cid.second;
          }
        }
      }
    }

    return stats;
  }

  bool tryGetHandleWithWaitContextForMovingItem(Item& item,
                                                WriteHandle& handle);

  class MoveCtx {
   public:
    MoveCtx() {}

    ~MoveCtx() {
      // prevent any further enqueue to waiters
      // Note: we don't need to hold locks since no one can enqueue
      // after this point.
      wakeUpWaiters();
    }

    // record the item handle. Upon destruction we will wake up the waiters
    // and pass a clone of the handle to the callBack. By default we pass
    // a null handle
    void setItemHandle(WriteHandle _it) { it = std::move(_it); }

    // enqueue a waiter into the waiter list
    // @param  waiter       WaitContext
    void addWaiter(std::shared_ptr<WaitContext<ReadHandle>> waiter) {
      XDCHECK(waiter);
      waiters.push_back(std::move(waiter));
    }

    size_t numWaiters() const { return waiters.size(); }

   private:
    // notify all pending waiters that are waiting for the fetch.
    void wakeUpWaiters() {
      bool refcountOverflowed = false;
      for (auto& w : waiters) {
        // If refcount overflowed earlier, then we will return miss to
        // all subsequent waiters.
        if (refcountOverflowed) {
          w->set(WriteHandle{});
          continue;
        }

        try {
          w->set(it.clone());
        } catch (const exception::RefcountOverflow&) {
          // We'll return a miss to the user's pending read,
          // so we should enqueue a delete via NvmCache.
          // TODO: cache.remove(it);
          refcountOverflowed = true;
        }
      }
    }

    WriteHandle it; // will be set when Context is being filled
    std::vector<std::shared_ptr<WaitContext<ReadHandle>>> waiters; // list of
                                                                   // waiters
  };
  using MoveMap =
      folly::F14ValueMap<folly::StringPiece,
                         std::unique_ptr<MoveCtx>,
                         folly::HeterogeneousAccessHash<folly::StringPiece>>;

  size_t getShardForKey(folly::StringPiece key) {
    return folly::Hash()(key) % shards_;
  }

  MoveMap& getMoveMapForShard(size_t shard) {
    return movesMap_[shard].movesMap_;
  }

  MoveMap& getMoveMap(folly::StringPiece key) {
    return getMoveMapForShard(getShardForKey(key));
  }

  std::unique_lock<std::mutex> acquireMoveLockForShard(size_t shard) {
    return std::unique_lock<std::mutex>(moveLock_[shard].moveLock_);
  }

  // Bump the number of times handle wait blocks. This is called from
  // ItemHandle's wait context logic.
  void bumpHandleWaitBlocks() { stats().numHandleWaitBlocks.inc(); }

  // BEGIN private members

  // Whether the memory allocator for this cache allocator was created on shared
  // memory. The hash table, chained item hash table etc is also created on
  // shared memory except for temporary shared memory mode when they're created
  // on heap.
  const bool isOnShm_{false};

  Config config_{};

  // Manages the temporary shared memory segment for memory allocator that
  // is not persisted when cache process exits.
  std::unique_ptr<TempShmMapping> tempShm_;

  std::unique_ptr<ShmManager> shmManager_;

  // Deserialize data to restore cache allocator. Used only while attaching to
  // existing shared memory.
  std::unique_ptr<Deserializer> deserializer_;

  // used only while attaching to existing shared memory.
  serialization::CacheAllocatorMetadata metadata_{};

  // configs for the access container and the mm container.
  const MMConfig mmConfig_{};

  // the memory allocator for allocating out of the available memory.
  std::unique_ptr<MemoryAllocator> allocator_;

  // compact cache allocator manager
  std::unique_ptr<CCacheManager> compactCacheManager_;

  // compact cache instances reside here when user "add" or "attach" compact
  // caches. The lifetime is tied to CacheAllocator.
  // The bool is an indicator for whether the compact cache participates in
  // size optimization in PoolOptimizer
  std::unordered_map<PoolId, std::unique_ptr<ICompactCache>> compactCaches_;

  // check whether a pool is enabled for optimizing
  std::array<std::atomic<bool>, MemoryPoolManager::kMaxPools>
      optimizerEnabled_{};

  // ptr compressor
  typename Item::PtrCompressor compressor_;

  // Lock to synchronize addition of a new pool and its resizing/rebalancing
  mutable folly::SharedMutex poolsResizeAndRebalanceLock_;

  // Extra MM container shards, null unless config_.mmContainerShards > 1, so
  // an unsharded cache keeps its layout and lookup path. Declared before
  // mmContainers_ because deserializeMMContainers() fills it in.
  std::unique_ptr<MMContainerShardSets> mmContainerShards_;

  // container for the allocations which are currently being memory managed by
  // the cache allocator.
  // we need mmcontainer per allocator pool/allocation class.
  MMContainers mmContainers_;

  // container that is used for accessing the allocations by their key.
  std::unique_ptr<AccessContainer> accessContainer_;

  // container that is used for accessing the chained allocation
  std::unique_ptr<AccessContainer> chainedItemAccessContainer_{nullptr};

  friend ChainedAllocs;
  friend WritableChainedAllocs;
  // ensure any modification to a chain of chained items are synchronized
  using ChainedItemLock = RWBucketLocks<
      trace::Profiled<folly::SharedMutex, "cachelib:chained_item">>;
  ChainedItemLock chainedItemLocks_;

  // nvmCache
  std::unique_ptr<NvmCacheT> nvmCache_;

  // Wake the pool rebalancer after an allocation failure, unless forced
  // wake-ups are disabled or no rebalancer is running.
  void wakeUpPoolRebalancerOnAllocFailure() {
    if (!config_.poolRebalancerDisableForcedWakeUp && poolRebalancer_) {
      poolRebalancer_->wakeUpOnAllocationFailure();
    }
  }

  // rebalancer for the pools
  std::unique_ptr<PoolRebalancer> poolRebalancer_;

  // resizer for the pools.
  std::unique_ptr<PoolResizer> poolResizer_;

  // automatic arena resizing i.e. pool optimization
  std::unique_ptr<PoolOptimizer> poolOptimizer_;

  // free memory monitor
  std::unique_ptr<MemoryMonitor> memMonitor_;
  // memMonitor_->getMaxAdvisePct(), or 0 without a monitor. Readable without
  // workersMutex_, which getCacheMemoryStats() cannot take: the monitor calls
  // it while stopWorker() holds the mutex and waits for the monitor.
  std::atomic<size_t> memMonitorMaxAdvisePct_{0};

  // background evictor
  std::vector<std::unique_ptr<BackgroundMover<CacheT>>> backgroundEvictor_;
  std::vector<std::unique_ptr<BackgroundMover<CacheT>>> backgroundPromoter_;

  // check whether a pool is a slabs pool
  std::array<bool, MemoryPoolManager::kMaxPools> isCompactCachePool_{};

  // lock to serilize access of isCompactCachePool_ array, including creation of
  // compact cache pools
  mutable folly::SharedMutex compactCachePoolsLock_;

  // mutex protecting the creation and destruction of workers poolRebalancer_,
  // poolResizer_, poolOptimizer_, memMonitor_, reaper_
  mutable std::mutex workersMutex_;

  const size_t shards_;

  struct MovesMapShard {
    alignas(folly::hardware_destructive_interference_size) MoveMap movesMap_;
  };

  struct MoveLock {
    alignas(folly::hardware_destructive_interference_size) std::mutex moveLock_;
  };

  // a map of all pending moves
  std::vector<MovesMapShard> movesMap_;

  // a map of move locks for each shard
  std::vector<MoveLock> moveLock_;

  // time when the ram cache was first created
  const uint32_t cacheCreationTime_{0};

  // time when CacheAllocator structure is created. Whenever a process restarts
  // and even if cache content is persisted, this will be reset. It's similar
  // to process uptime. (But alternatively if user explicitly shuts down and
  // re-attach cache, this will be reset as well)
  const uint32_t cacheInstanceCreationTime_{0};

  // thread local accumulation of handle counts
  mutable util::FastStats<int64_t> handleCount_{};

  mutable detail::Stats stats_{};
  // allocator's items reaper to evict expired items in bg checking
  std::unique_ptr<Reaper<CacheT>> reaper_;

  class DummyTlsActiveItemRingTag {};
  folly::ThreadLocal<TlsActiveItemRing, DummyTlsActiveItemRingTag> ring_;

  enum class StartTruncateReason : uint8_t {
    kNone = 0,
    kNoCacheDir = 1,
    // shouldStartFresh(): covers unclean shutdown, ice roll, nvm version or
    // encryption change, corrupt metadata, and first boot. Not the same as
    // "the previous shutdown crashed".
    kNoUsableState = 2,
    kDramCacheNew = 3,
  };
  StartTruncateReason startTruncateReason_{StartTruncateReason::kNone};

  // state for the nvmcache
  NvmCacheState nvmCacheState_;

  // admission policy for nvmcache
  std::shared_ptr<NvmAdmissionPolicy<CacheT>> nvmAdmissionPolicy_;

  // set when a fast shutdown is requested; signals background workers to abort
  // in-progress work so shutDown() can complete quickly
  std::atomic<bool> triggerFastShutdown_{false};

  // shutDown() is one-shot. The first caller to flip this false->true owns the
  // (destructive, non-idempotent) teardown. This is never reset.
  std::atomic<bool> shutDownStarted_{false};

  // END private members

  // Make this friend to give access to acquire and release
  friend ReadHandle;
  friend ReaperAPIWrapper<CacheT>;
  friend BackgroundMoverAPIWrapper<CacheT>;
  friend class CacheAPIWrapperForNvm<CacheT>;
  friend class FbInternalRuntimeUpdateWrapper<CacheT>;
  friend class objcache2::ObjectCache<CacheT>;
  friend class objcache2::ObjectCacheBase<CacheT>;
  template <typename K, typename V, typename C>
  friend class ReadOnlyMap;

  // tests
  friend class facebook::cachelib::tests::NvmCacheTest;
  FRIEND_TEST(CachelibAdminCoreTest, WorkingSetAnalysisLoggingTest);
  template <typename AllocatorT>
  friend class facebook::cachelib::tests::BaseAllocatorTest;
  friend class facebook::cachelib::tests::HugePageMatrixTest;
  template <typename AllocatorT>
  friend class facebook::cachelib::tests::AllocatorHitStatsTest;
  template <typename AllocatorT>
  friend class facebook::cachelib::tests::AllocatorResizeTest;
  template <typename AllocatorT>
  friend class facebook::cachelib::tests::PoolOptimizeStrategyTest;
  friend class facebook::cachelib::tests::NvmAdmissionPolicyTest;
  friend class facebook::cachelib::tests::CacheAllocatorTestWrapper;
  friend class facebook::cachelib::tests::PersistenceCache;
  template <typename AllocatorT>
  friend class facebook::cachelib::tests::FixedSizeArrayTest;
  template <typename AllocatorT>
  friend class facebook::cachelib::tests::MapTest;

  // benchmarks
  template <typename Allocator>
  friend class facebook::cachelib::cachebench::Cache;
  friend class facebook::cachelib::cachebench::tests::CacheTest;
  friend void lookupCachelibBufferManagerOnly();
  friend void lookupCachelibMap();
  friend void benchCachelibMap();
  friend void benchCachelibRangeMap();

  // objectCache
  template <typename CacheDescriptor, typename AllocatorRes>
  friend class facebook::cachelib::objcache::deprecated_ObjectCache;
  friend class GET_DECORATED_CLASS_NAME(objcache::test,
                                        ObjectCache,
                                        ObjectHandleInvalid);

  // interface
  friend class interface::RAMCacheComponent;
  friend class interface::RAMCacheItem;
};

template <typename CacheTrait>
CacheAllocator<CacheTrait>::CacheAllocator(Config config)
    : CacheAllocator(InitMemType::kNone, config) {
  initCommon(false);
}

template <typename CacheTrait>
CacheAllocator<CacheTrait>::CacheAllocator(SharedMemNewT, Config config)
    : CacheAllocator(InitMemType::kMemNew, config) {
  initCommon(false);
  shmManager_->removeShm(detail::kShmInfoName);
}

template <typename CacheTrait>
CacheAllocator<CacheTrait>::CacheAllocator(SharedMemAttachT, Config config)
    : CacheAllocator(InitMemType::kMemAttach, config) {
  for (auto pid : *metadata_.compactCachePools()) {
    isCompactCachePool_[pid] = true;
  }

  initCommon(true);

  // We will create a new info shm segment on shutDown(). If we don't remove
  // this info shm segment here and the new info shm segment's size is larger
  // than this one, creating new one will fail.
  shmManager_->removeShm(detail::kShmInfoName);
}

template <typename CacheTrait>
CacheAllocator<CacheTrait>::CacheAllocator(
    typename CacheAllocator<CacheTrait>::InitMemType type, Config config)
    : isOnShm_{type != InitMemType::kNone ? true
                                          : config.memMonitoringEnabled()},
      config_(config.validate()),
      tempShm_(type == InitMemType::kNone && isOnShm_
                   ? std::make_unique<TempShmMapping>(config_.getCacheSize(),
                                                      config_.slabHugePageSize)
                   : nullptr),
      shmManager_(type != InitMemType::kNone
                      ? std::make_unique<ShmManager>(config_.cacheDir,
                                                     config_.isUsingPosixShm(),
                                                     config_.hugePageMountDir,
                                                     config_.posixShmDirectory)
                      : nullptr),
      deserializer_(type == InitMemType::kMemAttach ? createDeserializer()
                                                    : nullptr),
      metadata_{type == InitMemType::kMemAttach
                    ? deserializeCacheAllocatorMetadata(*deserializer_)
                    : serialization::CacheAllocatorMetadata{}},
      allocator_(initAllocator(type)),
      compactCacheManager_(type != InitMemType::kMemAttach
                               ? std::make_unique<CCacheManager>(*allocator_)
                               : restoreCCacheManager()),
      compressor_(createPtrCompressor()),
      mmContainerShards_(config_.mmContainerShards > 1
                             ? std::make_unique<MMContainerShardSets>()
                             : nullptr),
      mmContainers_(type == InitMemType::kMemAttach
                        ? deserializeMMContainers(*deserializer_, compressor_)
                        : MMContainers{}),
      accessContainer_(initAccessContainer(
          type, detail::kShmHashTableName, config.accessConfig)),
      chainedItemAccessContainer_(
          initAccessContainer(type,
                              detail::kShmChainedItemHashTableName,
                              config.chainedItemAccessConfig)),
      chainedItemLocks_(config_.chainedItemsLockPower,
                        std::make_shared<MurmurHash2>()),
      shards_{config_.numShards},
      movesMap_(shards_),
      moveLock_(shards_),
      cacheCreationTime_{
          type != InitMemType::kMemAttach
              ? util::getCurrentTimeSec()
              : static_cast<uint32_t>(*metadata_.cacheCreationTime())},
      cacheInstanceCreationTime_{type != InitMemType::kMemAttach
                                     ? cacheCreationTime_
                                     : util::getCurrentTimeSec()},
      // Pass in cacheInstnaceCreationTime_ as the current time to keep
      // nvmCacheState's current time in sync
      nvmCacheState_{cacheInstanceCreationTime_, config_.cacheDir,
                     config_.isNvmCacheEncryptionEnabled(),
                     config_.isNvmCacheTruncateAllocSizeEnabled()} {}

template <typename CacheTrait>
CacheAllocator<CacheTrait>::~CacheAllocator() {
  XLOG(DBG, "destructing CacheAllocator");
  // Stop all workers. In case user didn't call shutDown, we want to
  // terminate all background workers and nvmCache before member variables
  // go out of scope.
  stopWorkers();
  nvmCache_.reset();
}

template <typename CacheTrait>
ShmSegmentOpts CacheAllocator<CacheTrait>::createShmCacheOpts() {
  ShmSegmentOpts opts;
  opts.alignment = sizeof(Slab);
  // TODO: we support single tier so far
  if (config_.memoryTierConfigs.size() > 1) {
    throw std::invalid_argument("CacheLib only supports a single memory tier");
  }
  opts.memBindNumaNodes = config_.memoryTierConfigs[0].getMemBind();
  applyHugePageOpts(opts, config_.slabHugePageSize);
  return opts;
}

template <typename CacheTrait>
ShmSegmentOpts CacheAllocator<CacheTrait>::createShmAccessOpts() {
  ShmSegmentOpts opts;
  applyHugePageOpts(opts, config_.accessHugePageSize);
  return opts;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::applyHugePageOpts(
    ShmSegmentOpts& opts, const PageSize& pageSize) const {
  if (!pageSize.isHugePage()) {
    return;
  } else if (!PageSize::supportedHugePageSizes().contains(
                 pageSize.getPageSize())) {
    throw std::invalid_argument(fmt::format(
        "Requested huge page size {} is not supported by the kernel",
        pageSize.getPageSize()));
  } else if (config_.hugePageMountDir.empty() && config_.usePosixShm) {
    throw std::invalid_argument(
        "Requested huge pages for POSIX shared memory regions but didn't "
        "supply a hugetlbfs mount");
  }
  opts.pageSize = pageSize;
  // The segment is attached at a page-aligned address, so its alignment must be
  // at least the huge page size or the kernel rejects the mapping
  opts.alignment = std::max(opts.alignment, pageSize.getPageSize());
}

template <typename CacheTrait>
std::unique_ptr<MemoryAllocator>
CacheAllocator<CacheTrait>::createNewMemoryAllocator() {
  return std::make_unique<MemoryAllocator>(
      getAllocatorConfig(config_),
      shmManager_
          ->createShm(detail::kShmCacheName, config_.getCacheSize(),
                      config_.slabMemoryBaseAddr, createShmCacheOpts())
          .addr,
      config_.getCacheSize());
}

template <typename CacheTrait>
std::unique_ptr<MemoryAllocator>
CacheAllocator<CacheTrait>::restoreMemoryAllocator() {
  return std::make_unique<MemoryAllocator>(
      deserializer_->deserialize<MemoryAllocator::SerializationType>(),
      shmManager_
          ->attachShm(detail::kShmCacheName, config_.slabMemoryBaseAddr,
                      createShmCacheOpts())
          .addr,
      config_.getCacheSize(), config_.disableFullCoredump,
      config_.isSlabAsanPoisoningEnabled(), config_.slabHugePageSize);
}

template <typename CacheTrait>
std::unique_ptr<CCacheManager>
CacheAllocator<CacheTrait>::restoreCCacheManager() {
  return std::make_unique<CCacheManager>(
      deserializer_->deserialize<CCacheManager::SerializationType>(),
      *allocator_);
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::initCommon(bool dramCacheAttached) {
  // Initialize aggregate pool stats from config
  aggregatePoolStats_ = config_.isAggregatePoolStatsEnabled();

  if (config_.nvmConfig.has_value()) {
    if (config_.nvmCacheAP) {
      nvmAdmissionPolicy_ = config_.nvmCacheAP;
    } else if (config_.rejectFirstAPNumEntries) {
      nvmAdmissionPolicy_ = std::make_shared<RejectFirstAP<CacheT>>(
          config_.rejectFirstAPNumEntries, config_.rejectFirstAPNumSplits,
          config_.rejectFirstSuffixIgnoreLength,
          config_.rejectFirstUseDramHitSignal);
    }
    if (config_.nvmAdmissionMinTTL > 0) {
      if (!nvmAdmissionPolicy_) {
        nvmAdmissionPolicy_ = std::make_shared<NvmAdmissionPolicy<CacheT>>();
      }
      nvmAdmissionPolicy_->initMinTTL(config_.nvmAdmissionMinTTL);
    }
    if (config_.allowLargeKeys) {
      config_.nvmConfig->navyConfig.setMaxKeySize(KAllocation::kKeyMaxLen);
    }
  }
  initStats();
  initNvmCache(dramCacheAttached);
  if (config_.eventTrackerConfigFactory) {
    try {
      setEventTracker(config_.eventTrackerConfigFactory());
    } catch (const std::exception& ex) {
      XLOG(ERR, "Failed to initialize EventTracker, disabling: {}", ex.what());
    }
  }

  if (!config_.delayCacheWorkersStart) {
    initWorkers();
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::initNvmCache(bool dramCacheAttached) {
  if (!config_.nvmConfig.has_value()) {
    return;
  }

  // for some usecases that create pools, restoring nvmcache when dram cache
  // is not persisted is not supported.
  const bool shouldDrop = config_.dropNvmCacheOnShmNew && !dramCacheAttached;

  // if we are dealing with persistency, cache directory should be enabled
  const bool truncate = config_.cacheDir.empty() ||
                        nvmCacheState_.shouldStartFresh() || shouldDrop;
  // Recorded before markTruncated(), which clears wasCleanShutDown_.
  if (!truncate) {
    startTruncateReason_ = StartTruncateReason::kNone;
  } else if (config_.cacheDir.empty()) {
    startTruncateReason_ = StartTruncateReason::kNoCacheDir;
  } else if (nvmCacheState_.shouldStartFresh()) {
    startTruncateReason_ = StartTruncateReason::kNoUsableState;
  } else {
    startTruncateReason_ = StartTruncateReason::kDramCacheNew;
  }
  if (truncate) {
    nvmCacheState_.markTruncated();
  }

  auto legacyEventTracker = getLegacyEventTracker();
  if (legacyEventTracker) {
    XLOG(INFO) << "Set legacy event tracker in block cache.";
    config_.nvmConfig->navyConfig.blockCache().setLegacyEventTracker(
        *legacyEventTracker);
  }

  navy::NavyPersistParams persistParam{
      config_.nvmConfig->navyConfig.blockCache()
          .getIndexConfig()
          .useShmToPersist(),
      shmManager_ != nullptr
          ? *shmManager_
          : std::optional<std::reference_wrapper<ShmManager>>{}};
  nvmCache_ = std::make_unique<NvmCacheT>(*this, *config_.nvmConfig, truncate,
                                          config_.itemDestructor, persistParam);

  // Set EventTracker dynamically after NvmCache creation
  if (auto* et = getEventTracker()) {
    XLOG(INFO) << "Setting event tracker in NVM cache engines.";
    nvmCache_->setEventTracker(et);
  }
  if (!config_.cacheDir.empty()) {
    nvmCacheState_.clearPrevState();
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::initWorkers() {
  if (config_.poolResizingEnabled() && !poolResizer_) {
    startNewPoolResizer(config_.poolResizeInterval,
                        config_.poolResizeSlabsPerIter,
                        config_.poolResizeStrategy);
  }

  if (config_.poolRebalancingEnabled() && !poolRebalancer_) {
    startNewPoolRebalancer(config_.poolRebalanceInterval,
                           config_.defaultPoolRebalanceStrategy,
                           config_.poolRebalancerFreeAllocThreshold);
  }

  if (config_.memMonitoringEnabled() && !memMonitor_) {
    startNewMemMonitor(config_.memMonitorInterval,
                       config_.memMonitorConfig,
                       config_.poolAdviseStrategy);
  }

  if (config_.itemsReaperEnabled() && !reaper_) {
    startNewReaper(config_.reaperInterval, config_.reaperConfig);
  }

  if (config_.poolOptimizerEnabled() && !poolOptimizer_) {
    startNewPoolOptimizer(config_.regularPoolOptimizeInterval,
                          config_.compactCacheOptimizeInterval,
                          config_.poolOptimizeStrategy,
                          config_.ccacheOptimizeStepSizePercent);
  }

  if (config_.backgroundEvictorEnabled()) {
    startNewBackgroundEvictor(config_.backgroundEvictorInterval,
                              config_.backgroundEvictorStrategy,
                              config_.backgroundEvictorThreads);
  }

  if (config_.backgroundPromoterEnabled()) {
    startNewBackgroundPromoter(config_.backgroundPromoterInterval,
                               config_.backgroundPromoterStrategy,
                               config_.backgroundPromoterThreads);
  }
}

template <typename CacheTrait>
std::unique_ptr<MemoryAllocator> CacheAllocator<CacheTrait>::initAllocator(
    InitMemType type) {
  if (type == InitMemType::kNone) {
    if (isOnShm_ == true) {
      return std::make_unique<MemoryAllocator>(getAllocatorConfig(config_),
                                               tempShm_->getAddr(),
                                               config_.getCacheSize());
    } else {
      return std::make_unique<MemoryAllocator>(getAllocatorConfig(config_),
                                               config_.getCacheSize());
    }
  } else if (type == InitMemType::kMemNew) {
    return createNewMemoryAllocator();
  } else if (type == InitMemType::kMemAttach) {
    return restoreMemoryAllocator();
  }

  // Invalid type
  throw std::runtime_error(fmt::format(
      "Cannot initialize memory allocator, unknown InitMemType: {}.",
      static_cast<int>(type)));
}

template <typename CacheTrait>
std::unique_ptr<typename CacheAllocator<CacheTrait>::AccessContainer>
CacheAllocator<CacheTrait>::initAccessContainer(InitMemType type,
                                                const std::string name,
                                                AccessConfig config) {
  if (type == InitMemType::kNone) {
    return std::make_unique<AccessContainer>(
        config, compressor_,
        [this](Item* it) -> WriteHandle { return acquire(it); },
        config_.accessHugePageSize);
  } else if (type == InitMemType::kMemNew) {
    return std::make_unique<AccessContainer>(
        config,
        shmManager_
            ->createShm(
                name,
                AccessContainer::getRequiredSize(config.getNumBuckets()),
                nullptr,
                createShmAccessOpts())
            .addr,
        compressor_,
        [this](Item* it) -> WriteHandle { return acquire(it); });
  } else if (type == InitMemType::kMemAttach) {
    return std::make_unique<AccessContainer>(
        deserializer_->deserialize<AccessSerializationType>(),
        config,
        shmManager_->attachShm(name, nullptr, createShmAccessOpts()),
        compressor_,
        [this](Item* it) -> WriteHandle { return acquire(it); },
        config_.accessHugePageSize);
  }

  // Invalid type
  throw std::runtime_error(fmt::format(
      "Cannot initialize access container, unknown InitMemType: {}.",
      static_cast<int>(type)));
}

template <typename CacheTrait>
std::unique_ptr<Deserializer> CacheAllocator<CacheTrait>::createDeserializer() {
  auto infoAddr = shmManager_->attachShm(detail::kShmInfoName);
  return std::make_unique<Deserializer>(
      reinterpret_cast<uint8_t*>(infoAddr.addr),
      reinterpret_cast<uint8_t*>(infoAddr.addr) + infoAddr.size);
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::allocate(PoolId poolId,
                                     typename Item::Key key,
                                     uint32_t size,
                                     uint32_t ttlSecs,
                                     uint32_t creationTime) {
  if (creationTime == 0) {
    creationTime = util::getCurrentTimeSec();
  }
  return allocateInternal(poolId, key, size, creationTime,
                          ttlSecs == 0 ? 0 : creationTime + ttlSecs);
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::allocateInternal(PoolId pid,
                                             typename Item::Key key,
                                             uint32_t size,
                                             uint32_t creationTime,
                                             uint32_t expiryTime,
                                             bool fromBgThread) {
  util::LatencyTracker tracker{stats().allocateLatency_};

  SCOPE_FAIL { stats_.invalidAllocs.inc(); };

  // number of bytes required for this item
  const auto requiredSize = Item::getRequiredSize(key, size);

  // the allocation class in our memory allocator.
  const auto cid = allocator_->getAllocationClassId(pid, requiredSize);

  (*stats_.allocAttempts)[pid][cid].inc();

  void* memory = allocator_->allocate(pid, requiredSize);

  if (backgroundEvictor_.size() && !fromBgThread && memory == nullptr) {
    backgroundEvictor_[BackgroundMover<CacheT>::workerId(
                           pid, cid, backgroundEvictor_.size())]
        ->wakeUp();
  }

  if (memory == nullptr) {
    memory = findEviction(pid, cid);
  }

  WriteHandle handle;
  if (memory != nullptr) {
    // At this point, we have a valid memory allocation that is ready for use.
    // Ensure that when we abort from here under any circumstances, we free up
    // the memory. Item's handle could throw because the key size was invalid
    // for example.
    SCOPE_FAIL {
      // free back the memory to the allocator since we failed.
      allocator_->free(memory);
    };

    // Disallow large keys if not enabled in the config
    if (!config_.allowLargeKeys && key.size() > KAllocation::kKeyMaxLenSmall) {
      auto badKey =
          (key.start()) ? std::string(key.start(), key.size()) : std::string{};
      throw std::invalid_argument{fmt::format(
          "Invalid cache key - large key (> {} bytes) but large keys not "
          "enabled : {} (size = {})",
          KAllocation::kKeyMaxLenSmall, folly::humanify(badKey), key.size())};
    }

    handle = acquire(new (memory) Item(key, size, creationTime, expiryTime));
    if (handle) {
      handle.markNascent();
      (*stats_.fragmentationSize)[pid][cid].add(
          util::getFragmentation(*this, *handle));
    }

  } else { // failed to allocate memory.
    (*stats_.allocFailures)[pid][cid].inc();
    // wake up rebalancer
    wakeUpPoolRebalancerOnAllocFailure();
  }

  if (shouldRecordEvent(AllocatorApiEvent::ALLOCATE, key)) {
    if (handle) {
      auto eventParams = makeEventRecordParams(handle);
      eventParams.size = size;
      recordEventWithoutSampling(AllocatorApiEvent::ALLOCATE, key,
                                 AllocatorApiResult::ALLOCATED, eventParams);
    } else {
      const uint32_t ttl =
          expiryTime > creationTime ? (expiryTime - creationTime) : 0;
      recordEventWithoutSampling(
          AllocatorApiEvent::ALLOCATE, key, AllocatorApiResult::FAILED,
          EventRecordParams{
              .size = size, .ttlSecs = ttl, .expiryTime = expiryTime});
    }
  }

  return handle;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::allocateChainedItem(const ReadHandle& parent,
                                                uint32_t size) {
  if (!parent) {
    throw std::invalid_argument(
        "Cannot call allocate chained item with a empty parent handle!");
  }

  auto it = allocateChainedItemInternal(*parent, size);
  const auto result =
      it ? AllocatorApiResult::ALLOCATED : AllocatorApiResult::FAILED;
  // Log the parent's information since chained items share metadata with parent
  recordEvent(AllocatorApiEvent::ALLOCATE_CHAINED, parent->getKey(), result,
              parent);
  return it;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::allocateChainedItemInternal(const Item& parent,
                                                        uint32_t size) {
  util::LatencyTracker tracker{stats().allocateLatency_};

  SCOPE_FAIL { stats_.invalidAllocs.inc(); };

  // number of bytes required for this item
  const auto requiredSize = ChainedItem::getRequiredSize(size);

  const auto pid = allocator_->getAllocInfo(parent.getMemory()).poolId;
  const auto cid = allocator_->getAllocationClassId(pid, requiredSize);

  (*stats_.allocAttempts)[pid][cid].inc();

  void* memory = allocator_->allocate(pid, requiredSize);
  if (memory == nullptr) {
    memory = findEviction(pid, cid);
  }
  if (memory == nullptr) {
    (*stats_.allocFailures)[pid][cid].inc();
    wakeUpPoolRebalancerOnAllocFailure();
    return WriteHandle{};
  }

  SCOPE_FAIL { allocator_->free(memory); };

  auto child = acquire(new (memory) ChainedItem(
      compressor_.compress(&parent), size, util::getCurrentTimeSec()));

  if (child) {
    child.markNascent();
    (*stats_.fragmentationSize)[pid][cid].add(
        util::getFragmentation(*this, *child));
  }

  return child;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::addChainedItem(WriteHandle& parent,
                                                WriteHandle child) {
  if (!parent || !child || !child->isChainedItem()) {
    throw std::invalid_argument(
        fmt::format("Invalid parent or child. parent: {}, child: {}",
                    parent ? parent->toString() : "nullptr",
                    child ? child->toString() : "nullptr"));
  }

  auto l = chainedItemLocks_.lockExclusive(parent->getKey());

  // Insert into secondary lookup table for chained allocation
  auto oldHead = chainedItemAccessContainer_->insertOrReplace(*child);
  if (oldHead) {
    child->asChainedItem().appendChain(oldHead->asChainedItem(), compressor_);
  }

  // Count an item that just became a new parent
  if (!parent->hasChainedItem()) {
    stats_.numChainedParentItems.inc();
  }
  // Parent needs to be marked before inserting child into MM container
  // so the parent-child relationship is established before an eviction
  // can be triggered from the child
  parent->markHasChainedItem();
  // Count a new child
  stats_.numChainedChildItems.inc();

  // Increment refcount since this chained item is now owned by the parent
  // Parent will decrement the refcount upon release. Since this is an
  // internal refcount, we dont include it in active handle tracking. The
  // reason a chained item's refcount must be at least 1 is that we will not
  // free a chained item's memory back to the allocator when we drop its
  // item handle.
  auto ret = child->incRef();
  XDCHECK(ret == RefcountWithFlags::IncResult::kIncOk);
  XDCHECK_EQ(2u, child->getRefCount());

  insertInMMContainer(*child);

  invalidateNvm(*parent);
  recordEvent(AllocatorApiEvent::ADD_CHAINED, parent->getKey(),
              AllocatorApiResult::INSERTED, child.get());
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::popChainedItem(WriteHandle& parent) {
  if (!parent || !parent->hasChainedItem()) {
    throw std::invalid_argument(
        fmt::format("Invalid parent {}", parent ? parent->toString() : "null"));
  }

  WriteHandle head;
  { // scope of chained item lock.
    auto l = chainedItemLocks_.lockExclusive(parent->getKey());

    head = findChainedItem(*parent);
    if (head->asChainedItem().getNext(compressor_) != nullptr) {
      chainedItemAccessContainer_->insertOrReplace(
          *head->asChainedItem().getNext(compressor_));
    } else {
      chainedItemAccessContainer_->remove(*head);
      parent->unmarkHasChainedItem();
      stats_.numChainedParentItems.dec();
    }
    head->asChainedItem().setNext(nullptr, compressor_);

    invalidateNvm(*parent);
  }
  const auto res = removeFromMMContainer(*head);
  XDCHECK(res == true);

  // decrement the refcount to indicate this item is unlinked from its parent
  head->decRef();
  stats_.numChainedChildItems.dec();
  recordEvent(AllocatorApiEvent::POP_CHAINED, parent->getKey(),
              AllocatorApiResult::REMOVED, head.get());

  return head;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::Key
CacheAllocator<CacheTrait>::getParentKey(const Item& chainedItem) {
  XDCHECK(chainedItem.isChainedItem());
  if (!chainedItem.isChainedItem()) {
    throw std::invalid_argument(fmt::format(
        "Item must be chained item! Item: {}", chainedItem.toString()));
  }
  return reinterpret_cast<const ChainedItem&>(chainedItem)
      .getParentItem(compressor_)
      .getKey();
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::transferChainLocked(Item& parent,
                                                     Item& newParent) {
  // parent must be in a state to not have concurrent readers. Eviction code
  // paths rely on holding the last item handle.
  XDCHECK_EQ(parent.getKey(), newParent.getKey());
  XDCHECK(parent.hasChainedItem());

  if (newParent.hasChainedItem()) {
    throw std::invalid_argument(
        fmt::format("New Parent {} has invalid state", newParent.toString()));
  }

  auto headHandle = findChainedItem(parent);
  XDCHECK(headHandle);

  // remove from the access container since we are changing the key
  chainedItemAccessContainer_->remove(*headHandle);

  // change the key of the chain to have them belong to the new parent.
  ChainedItem* curr = &headHandle->asChainedItem();
  const auto newParentPtr = compressor_.compress(&newParent);
  while (curr) {
    XDCHECK_EQ(curr == headHandle.get() ? 2u : 1u, curr->getRefCount());
    XDCHECK(curr->isInMMContainer());
    XDCHECK(!newParent.isMoving());
    curr->changeKey(newParentPtr);
    curr = curr->getNext(compressor_);
  }

  newParent.markHasChainedItem();
  auto oldHead = chainedItemAccessContainer_->insertOrReplace(*headHandle);
  if (oldHead) {
    throw std::logic_error(
        fmt::format("Did not expect to find an existing chain for {}",
                    newParent.toString(), oldHead->toString()));
  }
  parent.unmarkHasChainedItem();
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::transferChainAndReplace(
    WriteHandle& parent, WriteHandle& newParent) {
  if (!parent || !newParent) {
    throw std::invalid_argument("invalid parent or new parent");
  }
  { // scope for chained item lock
    auto l = chainedItemLocks_.lockExclusive(parent->getKey());
    transferChainLocked(*parent, *newParent);
  }

  if (replaceIfAccessible(*parent, *newParent)) {
    newParent.unmarkNascent();
  }
  invalidateNvm(*parent);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::replaceIfAccessible(Item& oldItem,
                                                     Item& newItem) {
  XDCHECK(!newItem.isAccessible());

  // Inside the access container's lock, this checks if the old item is
  // accessible, and only in that case replaces it. If the old item is not
  // accessible anymore, it may have been replaced or removed earlier and there
  // is no point in proceeding with a move.
  if (!accessContainer_->replaceIfAccessible(oldItem, newItem)) {
    return false;
  }

  // Inside the MM container's lock, this checks if the old item exists to
  // make sure that no other thread removed it, and only then replaces it.
  if (!replaceInMMContainer(oldItem, newItem)) {
    accessContainer_->remove(newItem);
    return false;
  }

  // Replacing into the MM container was successful, but someone could have
  // called insertOrReplace() or remove() before or after the
  // replaceInMMContainer() operation, which would invalidate newItem.
  if (!newItem.isAccessible()) {
    removeFromMMContainer(newItem);
    return false;
  }
  return true;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::replaceChainedItem(Item& oldItem,
                                               WriteHandle newItemHandle,
                                               Item& parent) {
  if (!newItemHandle) {
    throw std::invalid_argument("Empty handle for newItem");
  }
  auto l = chainedItemLocks_.lockExclusive(parent.getKey());

  if (!oldItem.isChainedItem() || !newItemHandle->isChainedItem() ||
      &oldItem.asChainedItem().getParentItem(compressor_) !=
          &newItemHandle->asChainedItem().getParentItem(compressor_) ||
      &oldItem.asChainedItem().getParentItem(compressor_) != &parent ||
      newItemHandle->isInMMContainer() || !oldItem.isInMMContainer()) {
    throw std::invalid_argument(fmt::format(
        "Invalid args for replaceChainedItem. oldItem={}, newItem={}, "
        "parent={}",
        oldItem.toString(), newItemHandle->toString(), parent.toString()));
  }

  auto oldItemHdl =
      replaceChainedItemLocked(oldItem, std::move(newItemHandle), parent);
  invalidateNvm(parent);
  return oldItemHdl;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::replaceInChainLocked(Item& oldItem,
                                                      WriteHandle& newItemHdl,
                                                      const Item& parent,
                                                      bool fromMove) {
  auto head = findChainedItem(parent);
  XDCHECK(head != nullptr);
  XDCHECK_EQ(reinterpret_cast<uintptr_t>(
                 &head->asChainedItem().getParentItem(compressor_)),
             reinterpret_cast<uintptr_t>(&parent));

  // if old item is the head, replace the head in the chain and insert into
  // the access container and append its chain.
  if (head.get() == &oldItem) {
    chainedItemAccessContainer_->insertOrReplace(*newItemHdl);
  } else {
    // oldItem is in the middle of the chain, find its previous and fix the
    // links
    auto* prev = &head->asChainedItem();
    auto* curr = prev->getNext(compressor_);
    while (curr != nullptr && curr != &oldItem) {
      prev = curr;
      curr = curr->getNext(compressor_);
    }

    XDCHECK(curr != nullptr);
    prev->setNext(&newItemHdl->asChainedItem(), compressor_);
  }

  newItemHdl->asChainedItem().setNext(
      oldItem.asChainedItem().getNext(compressor_), compressor_);
  oldItem.asChainedItem().setNext(nullptr, compressor_);

  // if called from moveChainedItem then ref will be zero, else
  // greater than 0
  if (fromMove) {
    // Release the head of the chain here instead of at the end of the function.
    // The reason is that if the oldItem is the head of the chain, we need to
    // release it now while refCount > 1 so that the destructor does not call
    // releaseBackToAllocator since we want to recycle it.
    if (head) {
      head.reset();
      XDCHECK_EQ(1u, oldItem.getRefCount());
    }
    oldItem.decRef();
    XDCHECK_EQ(0u, oldItem.getRefCount()) << oldItem.toString();
  } else {
    oldItem.decRef();
    XDCHECK_LT(0u, oldItem.getRefCount()) << oldItem.toString();
  }

  // increment refcount to indicate parent owns this similar to addChainedItem
  // Since this is an internal refcount, we dont include it in active handle
  // tracking.

  auto ret = newItemHdl->incRef();
  XDCHECK(ret == RefcountWithFlags::IncResult::kIncOk);
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::replaceChainedItemLocked(Item& oldItem,
                                                     WriteHandle newItemHdl,
                                                     const Item& parent) {
  XDCHECK(newItemHdl != nullptr);
  XDCHECK_GE(1u, oldItem.getRefCount());

  // grab the handle to the old item so that we can return this. Also, we need
  // to drop the refcount the parent holds on oldItem by manually calling
  // decRef.  To do that safely we need to have a proper outstanding handle.
  auto oldItemHdl = acquire(&oldItem);
  XDCHECK_GE(2u, oldItem.getRefCount());

  // Replace the old chained item with new item in the MMContainer before we
  // actually replace the old item in the chain

  if (!replaceChainedItemInMMContainer(oldItem, *newItemHdl)) {
    // This should never happen since we currently hold an valid
    // parent handle. None of its chained items can be removed
    throw std::runtime_error(fmt::format(
        "chained item cannot be replaced in MM container, oldItem={}, "
        "newItem={}, parent={}",
        oldItem.toString(), newItemHdl->toString(), parent.toString()));
  }

  XDCHECK(!oldItem.isInMMContainer());
  XDCHECK(newItemHdl->isInMMContainer());

  replaceInChainLocked(oldItem, newItemHdl, parent, false /* fromMove */);

  return oldItemHdl;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::ReleaseRes
CacheAllocator<CacheTrait>::releaseBackToAllocator(Item& it,
                                                   RemoveContext ctx,
                                                   bool nascent,
                                                   const Item* toRecycle) {
  if (!it.isDrained()) {
    throw std::runtime_error(
        fmt::format("cannot release this item: {}", it.toString()));
  }

  const auto allocInfo = allocator_->getAllocInfo(it.getMemory());

  if (ctx == RemoveContext::kEviction) {
    const auto timeNow = util::getCurrentTimeSec();
    const auto refreshTime = timeNow - it.getLastAccessTime();
    const auto lifeTime = timeNow - it.getCreationTime();
    stats_.ramEvictionAgeSecs_.trackValue(refreshTime);
    stats_.ramItemLifeTimeSecs_.trackValue(lifeTime);
    stats_.perPoolEvictionAgeSecs_[allocInfo.poolId].trackValue(refreshTime);
  }

  (*stats_.fragmentationSize)[allocInfo.poolId][allocInfo.classId].sub(
      util::getFragmentation(*this, it));

  // Chained items can only end up in this place if the user has allocated
  // memory for a chained item but has decided not to insert the chained item
  // to a parent item and instead drop the chained item handle. In this case,
  // we free the chained item directly without calling remove callback.
  if (it.isChainedItem()) {
    if (toRecycle) {
      throw std::runtime_error(
          fmt::format("Can not recycle a chained item {}, toRecycle {}",
                      it.toString(), toRecycle->toString()));
    }

    allocator_->free(&it);
    return ReleaseRes::kReleased;
  }

  // nascent items represent items that were allocated but never inserted into
  // the cache. We should not be executing removeCB for them since they were
  // not initialized from the user perspective and never part of the cache.
  if (!nascent && config_.removeCb) {
    config_.removeCb(RemoveCbData{ctx, it, viewAsChainedAllocsRange(it)});
  }

  // only skip destructor for evicted items that are either in the queue to put
  // into nvm or already in nvm
  bool skipDestructor =
      nascent || (ctx == RemoveContext::kEviction &&
                  // When this item is queued for NvmCache, it will be marked
                  // as clean and the NvmEvicted bit will also be set to false.
                  // Refer to NvmCache::put()
                  it.isNvmClean() && !it.isNvmEvicted());
  if (!skipDestructor) {
    if (ctx == RemoveContext::kEviction) {
      stats().numCacheEvictions.inc();
    }
    // execute ItemDestructor
    if (config_.itemDestructor) {
      try {
        config_.itemDestructor(DestructorData{
            ctx, it, viewAsChainedAllocsRange(it), allocInfo.poolId});
        stats().numRamDestructorCalls.inc();
      } catch (const std::exception& e) {
        stats().numDestructorExceptions.inc();
        XLOG_EVERY_N(INFO, 100)
            << "Catch exception from user's item destructor: " << e.what();
      }
    }
  }

  // If no `toRecycle` is set, then the result is kReleased
  // Because this function cannot fail to release "it"
  ReleaseRes res =
      toRecycle == nullptr ? ReleaseRes::kReleased : ReleaseRes::kNotRecycled;

  // Free chained allocs if there are any
  if (it.hasChainedItem()) {
    // At this point, the parent is only accessible within this thread
    // and thus no one else can add or remove any chained items associated
    // with this parent. So we're free to go through the list and free
    // chained items one by one.
    auto headHandle = findChainedItem(it);
    ChainedItem* head =
        headHandle ? &headHandle.get()->asChainedItem() : nullptr;
    headHandle.reset();

    if (head == nullptr || &head->getParentItem(compressor_) != &it) {
      throw exception::ChainedItemInvalid(fmt::format(
          "Mismatch parent pointer. This should not happen. Key: {}",
          it.getKey()));
    }

    if (!chainedItemAccessContainer_->remove(*head)) {
      throw exception::ChainedItemInvalid(fmt::format(
          "Chained item associated with {} cannot be removed from hash table "
          "This should not happen here.",
          it.getKey()));
    }

    while (head) {
      auto next = head->getNext(compressor_);

      const auto childInfo =
          allocator_->getAllocInfo(static_cast<const void*>(head));
      (*stats_.fragmentationSize)[childInfo.poolId][childInfo.classId].sub(
          util::getFragmentation(*this, *head));

      removeFromMMContainer(*head);

      // No other thread can access any of the chained items by this point,
      // so the refcount for each chained item must be equal to 1. Since
      // we use 1 to mark an item as being linked to a parent item.
      const auto childRef = head->decRef();
      XDCHECK_EQ(0u, childRef);

      if (head == toRecycle) {
        XDCHECK(ReleaseRes::kReleased != res);
        res = ReleaseRes::kRecycled;
      } else {
        allocator_->free(head);
      }

      stats_.numChainedChildItems.dec();
      head = next;
    }
    stats_.numChainedParentItems.dec();
  }

  if (&it == toRecycle) {
    XDCHECK(ReleaseRes::kReleased != res);
    res = ReleaseRes::kRecycled;
  } else {
    XDCHECK(it.isDrained());
    allocator_->free(&it);
  }

  return res;
}

template <typename CacheTrait>
RefcountWithFlags::IncResult CacheAllocator<CacheTrait>::incRef(Item& it) {
  auto ret = it.incRef();
  if (ret == RefcountWithFlags::IncResult::kIncOk) {
    ++handleCount_.tlStats();
  }
  return ret;
}

template <typename CacheTrait>
RefcountWithFlags::Value CacheAllocator<CacheTrait>::decRef(Item& it) {
  const auto ret = it.decRef();
  // do this after we ensured that we incremented a reference.
  --handleCount_.tlStats();
  return ret;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::acquire(Item* it) {
  if (UNLIKELY(!it)) {
    return WriteHandle{};
  }

  SCOPE_FAIL { stats_.numRefcountOverflow.inc(); };

  while (true) {
    auto incRes = incRef(*it);
    if (LIKELY(incRes == RefcountWithFlags::IncResult::kIncOk)) {
      return WriteHandle{it, *this};
    } else if (incRes == RefcountWithFlags::IncResult::kIncFailedEviction) {
      // item is being evicted
      return WriteHandle{};
    } else {
      // item is being moved - wait for completion
      WriteHandle handle;
      if (tryGetHandleWithWaitContextForMovingItem(*it, handle)) {
        return handle;
      }
    }
  }
}

template <typename CacheTrait>
std::pair<typename CacheAllocator<CacheTrait>::WriteHandle,
          typename CacheAllocator<CacheTrait>::TryAcquireResult>
CacheAllocator<CacheTrait>::tryAcquire(Item* it) {
  XDCHECK(it);

  SCOPE_FAIL { stats_.numRefcountOverflow.inc(); };

  auto incRes = incRef(*it);
  if (LIKELY(incRes == RefcountWithFlags::IncResult::kIncOk)) {
    return {WriteHandle{it, *this}, TryAcquireResult::kSuccess};
  }
  if (incRes == RefcountWithFlags::IncResult::kIncFailedMoving) {
    return {WriteHandle{}, TryAcquireResult::kMoving};
  }
  return {WriteHandle{}, TryAcquireResult::kSkip};
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::release(Item* it, bool isNascent) {
  // decrement the reference and if it drops to 0, release it back to the
  // memory allocator, and invoke the removal callback if there is one.
  if (UNLIKELY(!it)) {
    return;
  }

  const auto ref = decRef(*it);

  if (UNLIKELY(ref == 0)) {
    const auto res =
        releaseBackToAllocator(*it, RemoveContext::kNormal, isNascent);
    XDCHECK(res == ReleaseRes::kReleased);
  }
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::removeFromMMContainer(Item& item) {
  // remove it from the mm container.
  if (item.isInMMContainer()) {
    auto& mmContainer = getMMContainer(item);
    return mmContainer.remove(item);
  }
  return false;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::replaceInMMContainer(Item& oldItem,
                                                      Item& newItem) {
  auto& oldContainer = getMMContainer(oldItem);
  auto& newContainer = getMMContainer(newItem);
  if (&oldContainer == &newContainer) {
    return oldContainer.replace(oldItem, newItem);
  } else {
    return oldContainer.remove(oldItem) && newContainer.add(newItem);
  }
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::replaceChainedItemInMMContainer(
    Item& oldItem, Item& newItem) {
  auto& oldMMContainer = getMMContainer(oldItem);
  auto& newMMContainer = getMMContainer(newItem);
  if (&oldMMContainer == &newMMContainer) {
    return oldMMContainer.replace(oldItem, newItem);
  } else {
    if (!oldMMContainer.remove(oldItem)) {
      return false;
    }

    // This cannot fail because a new item should not have been inserted
    const auto newRes = newMMContainer.add(newItem);
    XDCHECK(newRes);
    return true;
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::insertInMMContainer(Item& item) {
  XDCHECK(!item.isInMMContainer());
  auto& mmContainer = getMMContainer(item);
  if (!mmContainer.add(item)) {
    throw std::runtime_error(
        fmt::format("Invalid state. Node {} was already in the container.",
                    fmt::ptr(&item)));
  }
}

/**
 * There is a potential race with inserts and removes that. While T1 inserts
 * the key, there is T2 that removes the key. There can be an interleaving of
 * inserts and removes into the MM and Access Conatainers.It does not matter
 * what the outcome of this race is (ie  key can be present or not present).
 * However, if the key is not accessible, it should also not be in
 * MMContainer. To ensure that, we always add to MMContainer on inserts before
 * adding to the AccessContainer. Both the code paths on success/failure,
 * preserve the appropriate state in the MMContainer. Note that this insert
 * will also race with the removes we do in SlabRebalancing code paths.
 */

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::insert(const WriteHandle& handle) {
  return insertImpl(handle, AllocatorApiEvent::INSERT);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::insertImpl(const WriteHandle& handle,
                                            AllocatorApiEvent event) {
  XDCHECK(handle);
  XDCHECK(event == AllocatorApiEvent::INSERT ||
          event == AllocatorApiEvent::INSERT_FROM_NVM);
  if (handle->isAccessible()) {
    throw std::invalid_argument("Handle is already accessible");
  }

  if (nvmCache_ != nullptr && !handle->isNvmClean()) {
    throw std::invalid_argument("Can't use insert API with nvmCache enabled");
  }

  // insert into the MM container before we make it accessible. Find will
  // return this item as soon as it is accessible.
  insertInMMContainer(*(handle.getInternal()));

  AllocatorApiResult result;
  if (!accessContainer_->insert(*(handle.getInternal()))) {
    // this should destroy the handle and release it back to the allocator.
    removeFromMMContainer(*(handle.getInternal()));
    result = AllocatorApiResult::FAILED;
  } else {
    handle.unmarkNascent();
    result = AllocatorApiResult::INSERTED;
  }

  recordEvent(event, handle->getKey(), result, handle);

  return result == AllocatorApiResult::INSERTED;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::insertOrReplace(const WriteHandle& handle) {
  XDCHECK(handle);
  if (handle->isAccessible()) {
    throw std::invalid_argument("Handle is already accessible");
  }

  HashedKey hk{handle->getKey()};

  insertInMMContainer(*(handle.getInternal()));
  WriteHandle replaced;
  try {
    auto lock =
        nvmCache_ ? nvmCache_->getItemDestructorLock(hk)
                  : std::unique_lock<typename NvmCacheT::ItemDestructorMutex>();

    replaced = accessContainer_->insertOrReplace(*(handle.getInternal()));

    if (replaced && replaced->isNvmClean() && !replaced->isNvmEvicted()) {
      // item is to be replaced and the destructor will be executed
      // upon memory released, mark it in nvm to avoid destructor
      // executed from nvm
      nvmCache_->markNvmItemRemovedLocked(hk);
    }
  } catch (const std::exception&) {
    removeFromMMContainer(*(handle.getInternal()));
    recordEvent(AllocatorApiEvent::INSERT_OR_REPLACE, handle->getKey(),
                AllocatorApiResult::FAILED, handle);
    throw;
  }

  // Remove from LRU as well if we do have a handle of old item
  if (replaced) {
    stats_.numInsertOrReplaceReplaced.inc();
    removeFromMMContainer(*replaced);
  } else {
    stats_.numInsertOrReplaceInserted.inc();
  }

  if (UNLIKELY(nvmCache_ != nullptr)) {
    // We can avoid nvm delete only if we have non nvm clean item in cache.
    // In all other cases we must enqueue delete.
    if (!replaced || replaced->isNvmClean()) {
      nvmCache_->remove(hk, nvmCache_->createDeleteTombStone(hk));
    }
  }

  handle.unmarkNascent();

  XDCHECK(handle);
  const auto result =
      replaced ? AllocatorApiResult::REPLACED : AllocatorApiResult::INSERTED;
  recordEvent(AllocatorApiEvent::INSERT_OR_REPLACE, handle->getKey(), result,
              handle);

  return replaced;
}

/* Next two methods are used to asynchronously move Item between Slabs.
 *
 * The thread, which moves Item, allocates new Item in the tier we are moving to
 * and calls moveRegularItem() method. This method does the following:
 *  1. Update the access container with the new item from the tier we are
 *     moving to. This Item has moving flag set.
 *  2. Copy data from the old Item to the new one.
 *
 * Concurrent threads which are getting handle to the same key:
 *  1. When a handle is created it checks if the moving flag is set
 *  2. If so, Handle implementation creates waitContext and adds it to the
 *     MoveCtx by calling handleWithWaitContextForMovingItem() method.
 *  3. Wait until the moving thread will complete its job.
 */
template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::tryGetHandleWithWaitContextForMovingItem(
    Item& item, WriteHandle& handle) {
  auto shard = getShardForKey(item.getKey());
  auto& movesMap = getMoveMapForShard(shard);
  {
    auto lock = acquireMoveLockForShard(shard);

    // item might have been evicted or moved before the lock was acquired
    if (!item.isMoving()) {
      return false;
    }

    WriteHandle hdl{*this};
    auto waitContext = hdl.getItemWaitContext();

    auto ret = movesMap.try_emplace(item.getKey(), std::make_unique<MoveCtx>());
    ret.first->second->addWaiter(std::move(waitContext));

    handle = std::move(hdl);
    return true;
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::wakeUpWaiters(folly::StringPiece key,
                                               WriteHandle handle) {
  std::unique_ptr<MoveCtx> ctx;
  auto shard = getShardForKey(key);
  auto& movesMap = getMoveMapForShard(shard);
  {
    auto lock = acquireMoveLockForShard(shard);
    movesMap.eraseInto(
        key, [&](auto&& /* key */, auto&& value) { ctx = std::move(value); });
  }

  if (ctx) {
    ctx->setItemHandle(std::move(handle));
  }
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::moveRegularItem(Item& oldItem,
                                                 WriteHandle& newItemHdl) {
  XDCHECK(oldItem.isMoving());
  // If an item is expired, proceed to eviction.
  if (oldItem.isExpired()) {
    return false;
  }

  util::LatencyTracker tracker{stats_.moveRegularLatency_};

  XDCHECK_EQ(newItemHdl->getSize(), oldItem.getSize());

  // take care of the flags before we expose the item to be accessed. this
  // will ensure that when another thread removes the item from RAM, we issue
  // a delete accordingly. See D7859775 for an example
  if (oldItem.isNvmClean()) {
    newItemHdl->markNvmClean();
    // the NVM copy may already be gone; don't let the new item claim it.
    if (oldItem.isNvmEvicted()) {
      newItemHdl->markNvmEvicted();
    }
  }

  // Execute the move callback. We cannot make any guarantees about the
  // consistency of the old item beyond this point, because the callback can
  // do more than a simple memcpy() e.g. update external references. If there
  // are any remaining handles to the old item, it is the caller's
  // responsibility to invalidate them. The move can only fail after this
  // statement if the old item has been removed or replaced, in which case it
  // should be fine for it to be left in an inconsistent state.
  config_.moveCb(oldItem, *newItemHdl, nullptr);

  // Adding the item to mmContainer has to succeed since no one can remove the
  // item
  auto& newContainer = getMMContainer(*newItemHdl);
  auto mmContainerAdded = newContainer.add(*newItemHdl);
  XDCHECK(mmContainerAdded);

  if (oldItem.hasChainedItem()) {
    XDCHECK(!newItemHdl->hasChainedItem()) << newItemHdl->toString();
    try {
      auto l = chainedItemLocks_.lockExclusive(oldItem.getKey());
      transferChainLocked(oldItem, *newItemHdl);
    } catch (const std::exception& e) {
      // this should never happen because we drained all the handles.
      XLOGF(DFATAL, "{}", e.what());
      throw;
    }

    XDCHECK(!oldItem.hasChainedItem());
    XDCHECK(newItemHdl->hasChainedItem());
  }

  if (!accessContainer_->replaceIfAccessible(oldItem, *newItemHdl)) {
    newContainer.remove(*newItemHdl);
    return false;
  }

  newItemHdl.unmarkNascent();
  return true;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::moveChainedItem(ChainedItem& oldItem,
                                                 WriteHandle& newItemHdl) {
  Item& parentItem = oldItem.getParentItem(compressor_);
  XDCHECK(parentItem.isMoving());
  util::LatencyTracker tracker{stats_.moveChainedLatency_};

  const auto parentKey = parentItem.getKey();
  auto l = chainedItemLocks_.lockExclusive(parentKey);

  XDCHECK_EQ(reinterpret_cast<uintptr_t>(
                 &newItemHdl->asChainedItem().getParentItem(compressor_)),
             reinterpret_cast<uintptr_t>(&parentItem.asChainedItem()));

  auto parentPtr = &parentItem;

  // Execute the move callback. We cannot make any guarantees about the
  // consistency of the old item beyond this point, because the callback can
  // do more than a simple memcpy() e.g. update external references. If there
  // are any remaining handles to the old item, it is the caller's
  // responsibility to invalidate them. The move can only fail after this
  // statement if the old item has been removed or replaced, in which case it
  // should be fine for it to be left in an inconsistent state.
  config_.moveCb(oldItem, *newItemHdl, parentPtr);

  // Replace the new item in the position of the old one before both in the
  // parent's chain and the MMContainer.
  XDCHECK_EQ(parentItem.getRefCount(), 0ul);
  auto& newContainer = getMMContainer(*newItemHdl);
  auto mmContainerAdded = newContainer.add(*newItemHdl);
  XDCHECK(mmContainerAdded);

  replaceInChainLocked(oldItem, newItemHdl, parentItem, true);

  return true;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::NvmCacheT::PutToken
CacheAllocator<CacheTrait>::createPutToken(Item& item) {
  const bool evictToNvmCache = shouldWriteToNvmCache(item);
  if (evictToNvmCache) {
    auto putTokenRv =
        nvmCache_->createPutToken(item.getKey(), []() { return true; });
    if (putTokenRv) {
      return std::move(*putTokenRv);
    }
  }

  return {};
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::unlinkItemForEviction(Item& it) {
  XDCHECK(it.isMarkedForEviction());
  XDCHECK_EQ(0u, it.getRefCount());
  accessContainer_->remove(it, RemoveContext::kEviction, config_.preRemoveCb);
  removeFromMMContainer(it);

  // Since we managed to mark the item for eviction we must be the only
  // owner of the item.
  const auto ref = it.unmarkForEviction();
  XDCHECK_EQ(0u, ref);
}

template <typename CacheTrait>
std::pair<typename CacheAllocator<CacheTrait>::Item*,
          typename CacheAllocator<CacheTrait>::Item*>
CacheAllocator<CacheTrait>::getNextCandidate(PoolId pid,
                                             ClassId cid,
                                             unsigned int& searchTries) {
  typename NvmCacheT::PutToken token;
  Item* toRecycle = nullptr;
  Item* candidate = nullptr;
  unsigned int searchLimit = config_.evictionSearchTries;
  std::atomic<uint32_t>* tailTime = nullptr;
  uint32_t shard = 0;
  if (isMMContainerSharded()) {
    shard = pickEvictionShard(pid, cid);
    tailTime = &(*mmContainerShards_)[pid][cid].tailTimes[shard];
    // Scan at most half of the remaining budget in one shard, so a shard
    // whose tail is pinned leaves tries for the others.
    if (searchLimit != 0) {
      searchLimit = searchTries + std::max(1u, (searchLimit - searchTries) / 2);
    }
  }
  auto& mmContainer = getMMContainerShard(pid, cid, shard);

  mmContainer.withEvictionIterator([this, pid, cid, &candidate, &toRecycle,
                                    &searchTries, &mmContainer, &token,
                                    searchLimit, tailTime](auto&& itr) {
    if (!itr) {
      ++searchTries;
      (*stats_.evictionAttempts)[pid][cid].inc();
      if (tailTime) {
        updateTailTimeHint(*tailTime, util::getCurrentTimeSec());
      }
      return;
    }

    while ((searchLimit == 0 || searchLimit > searchTries) && itr) {
      ++searchTries;
      (*stats_.evictionAttempts)[pid][cid].inc();

      auto* toRecycle_ = itr.get();
      auto* candidate_ =
          toRecycle_->isChainedItem()
              ? &toRecycle_->asChainedItem().getParentItem(compressor_)
              : toRecycle_;

      if (config_.evictionPredicate &&
          !config_.evictionPredicate(*candidate_)) {
        ++itr;
        stats_.evictFiltered.inc();
        continue;
      }

      typename NvmCacheT::PutToken putToken{};
      const bool evictToNvmCache = shouldWriteToNvmCache(*candidate_);

      auto markForEviction = [&candidate_, this]() {
        auto markForEvictionResult = candidate_->markForEviction();
        if (markForEvictionResult != MarkForEvictionResult::kSuccess) {
          if (candidate_->hasChainedItem()) {
            stats_.evictFailParentAC.inc();
            switch (markForEvictionResult) {
            case MarkForEvictionResult::kUnlinked:
              stats_.evictFailParentUnlinkedAC.inc();
              break;
            case MarkForEvictionResult::kExclusive:
              stats_.evictFailParentExclusiveAC.inc();
              break;
            case MarkForEvictionResult::kRefHeld:
              stats_.evictFailParentRefHeldAC.inc();
              break;
            default:
              break;
            }
          } else {
            stats_.evictFailAC.inc();
          }
          return false;
        }
        return true;
      };

      if (evictToNvmCache) {
        auto putTokenRv = nvmCache_->createPutToken(
            candidate_->getKey(),
            [&markForEviction]() { return markForEviction(); });

        if (!putTokenRv) {
          switch (putTokenRv.error()) {
          case InFlightPuts::PutTokenError::TRY_LOCK_FAIL:
            stats_.evictFailPutTokenLock.inc();
            break;
          case InFlightPuts::PutTokenError::TOKEN_EXISTS:
            stats_.evictFailConcurrentFill.inc();
            break;
          case InFlightPuts::PutTokenError::CALLBACK_FAILED:
            stats_.evictFailConcurrentAccess.inc();
            break;
          }
          ++itr;
          continue;
        }
        putToken = std::move(*putTokenRv);
        XDCHECK(putToken.isValid());
      } else {
        if (!markForEviction()) {
          ++itr;
          continue;
        }
      }

      // markForEviction to make sure no other thead is evicting the item
      // nor holding a handle to that item
      toRecycle = toRecycle_;
      candidate = candidate_;
      token = std::move(putToken);
      if (tailTime) {
        updateTailTimeHint(*tailTime, toRecycle_->getLastAccessTime());
      }

      // Check if parent changed for chained items - if yes, we cannot
      // remove the child from the mmContainer as we will not be evicting
      // it. We could abort right here, but we need to cleanup in case
      // unmarkForEviction() returns 0 - so just go through normal path.
      if (!toRecycle_->isChainedItem() ||
          &toRecycle->asChainedItem().getParentItem(compressor_) == candidate) {
        mmContainer.remove(itr);
      }
      return;
    }

    if (tailTime) {
      updateTailTimeHint(*tailTime, itr ? itr->getLastAccessTime()
                                        : util::getCurrentTimeSec());
    }
  });

  if (!toRecycle) {
    return {candidate, toRecycle};
  }

  XDCHECK(toRecycle);
  XDCHECK(candidate);
  XDCHECK(candidate->isMarkedForEviction());

  unlinkItemForEviction(*candidate);

  // track DRAM eviction and its result
  if (token.isValid() && shouldWriteToNvmCacheExclusive(*candidate)) {
    recordEvent(AllocatorApiEvent::DRAM_EVICT, candidate->getKey(),
                AllocatorApiResult::NVM_ADMITTED, candidate);
    nvmCache_->put(*candidate, std::move(token));
  } else {
    recordEvent(AllocatorApiEvent::DRAM_EVICT, candidate->getKey(),
                AllocatorApiResult::EVICTED, candidate);
    // When this item has an unmodified copy still present in BlockCache
    // (large items only), record its latest DRAM access time in the Access
    // Time Map as the value in the copy in BlockCache can be stale.
    if (shouldUpdateAccessTimeMap(*candidate)) {
      HashedKey hk{candidate->getKey()};
      nvmCache_->updateAccessTime(hk, candidate->getLastAccessTime());
    }
  }
  return {candidate, toRecycle};
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::Item*
CacheAllocator<CacheTrait>::findEviction(PoolId pid, ClassId cid) {
  // Keep searching for a candidate until we were able to evict it
  // or until the search limit has been exhausted
  unsigned int searchTries = 0;
  while (config_.evictionSearchTries == 0 ||
         config_.evictionSearchTries > searchTries) {
    auto [candidate, toRecycle] = getNextCandidate(pid, cid, searchTries);

    // Reached the end of the eviction queue but couldn't find a candidate,
    // start again.
    if (!toRecycle) {
      continue;
    }
    // recycle the item. it's safe to do so, even if toReleaseHandle was
    // NULL. If `ref` == 0 then it means that we are the last holder of
    // that item.
    if (candidate->hasChainedItem()) {
      (*stats_.chainedItemEvictions)[pid][cid].inc();
    } else {
      (*stats_.regularItemEvictions)[pid][cid].inc();
    }

    // check if by releasing the item we intend to, we actually
    // recycle the candidate.
    auto ret = releaseBackToAllocator(*candidate, RemoveContext::kEviction,
                                      /* isNascent */ false, toRecycle);
    if (ret == ReleaseRes::kRecycled) {
      return toRecycle;
    }
  }
  return nullptr;
}

template <typename CacheTrait>
template <typename Iter>
folly::Range<Iter> CacheAllocator<CacheTrait>::viewAsChainedAllocsRangeT(
    const Item& parent) const {
  return parent.hasChainedItem()
             ? folly::Range<Iter>{Iter{findChainedItem(parent).get(),
                                       compressor_},
                                  Iter{}}
             : folly::Range<Iter>{};
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::shouldWriteToNvmCache(const Item& item) {
  // write to nvmcache when it is enabled and the item says that it is not
  // nvmclean or evicted by nvm while present in DRAM.
  bool doWrite = nvmCache_ && nvmCache_->isEnabled();
  if (!doWrite) {
    return false;
  }

  doWrite = !item.isExpired();
  if (!doWrite) {
    stats_.numNvmRejectsByExpiry.inc();
    return false;
  }

  doWrite = (!item.isNvmClean() || item.isNvmEvicted());
  if (!doWrite) {
    stats_.numNvmRejectsByClean.inc();
    return false;
  }
  return true;
}
template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::shouldWriteToNvmCacheExclusive(
    const Item& item) {
  auto chainedItemRange = viewAsChainedAllocsRange(item);

  if (nvmAdmissionPolicy_) {
    AllocatorApiResult admissionResult = AllocatorApiResult::ACCEPTED;
    const bool accepted = nvmAdmissionPolicy_->accept(item, chainedItemRange);
    if (!accepted) {
      admissionResult = AllocatorApiResult::REJECTED;
      stats_.numNvmRejectsByAP.inc();
    }
    recordEvent(AllocatorApiEvent::NVM_ADMIT, item.getKey(), admissionResult,
                &item);
    return accepted;
  }

  return true;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::shouldUpdateAccessTimeMap(
    const Item& item) const {
  return nvmCache_ && item.isNvmClean() && !item.isNvmEvicted() &&
         item.isNvmLargeItem();
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::RemoveRes
CacheAllocator<CacheTrait>::remove(typename Item::Key key) {
  // While we issue this delete, there can be potential races that change the
  // state of the cache between ram and nvm. If we find the item in RAM and
  // obtain a handle, the situation is simpler. The complicated ones are the
  // following scenarios where when the delete checks RAM, we don't find
  // anything in RAM. The end scenario is that in the absence of any
  // concurrent inserts, after delete, there should be nothing in nvm and ram.
  //
  // == Racing async fill from nvm with delete ==
  // 1. T1 finds nothing in ram and issues a nvmcache look that is async. We
  // enqueue the get holding the fill lock and drop it.
  // 2. T2 finds nothing in ram, enqueues delete to nvmcache.
  // 3. T1's async fetch finishes and fills the item in cache, but right
  // before the delete is enqueued above
  //
  // To deal with this race, we first enqueue the nvmcache delete tombstone
  // and  when we finish the async fetch, we check if a tombstone was enqueued
  // meanwhile and cancel the fill.
  //
  // == Racing async fill from nvm with delete ==
  // there is a key in nvmcache and nothing in RAM.
  // 1.  T1 issues delete while nothing is in RAM and enqueues nvm cache
  // remove
  // 2. before the nvmcache remove gets enqueued, T2 does a find() that
  // fetches from nvm.
  // 3. T2 inserts in cache from nvmcache and T1 observes that item and tries
  // to remove it only from RAM.
  //
  //  to fix this, we do the nvmcache remove always the last thing and enqueue
  //  a tombstone to avoid concurrent fills while we are in the process of
  //  doing the nvmcache remove.
  //
  // == Racing eviction with delete ==
  // 1. T1 is evicting an item, trying to remove from the hashtable and is in
  // the process  of enqueing the put to nvmcache.
  // 2. T2 is removing and finds nothing in ram, enqueue the nvmcache delete.
  // The delete to nvmcache gets enqueued after T1 fills in ram.
  //
  // If T2 finds the item in ram, eviction can not proceed and the race does
  // not exist. If T2 does not find anything in RAM, it is likely that T1 is
  // in the process of issuing an nvmcache put. In this case, T1's nvmcache
  // put will check if there was a delete enqueued while the eviction was in
  // flight after removing from the hashtable.
  //
  stats_.numCacheRemoves.inc();
  HashedKey hk{key};

  using Guard = typename NvmCacheT::DeleteTombStoneGuard;
  auto tombStone = nvmCache_ ? nvmCache_->createDeleteTombStone(hk) : Guard{};

  auto handle = findInternal(key);
  if (!handle) {
    if (nvmCache_) {
      nvmCache_->remove(hk, std::move(tombStone));
    }
    recordEvent(AllocatorApiEvent::REMOVE, key, AllocatorApiResult::NOT_FOUND);
    return RemoveRes::kNotFoundInRam;
  }

  return removeImpl(hk, *handle, std::move(tombStone));
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::removeFromRamForTesting(
    typename Item::Key key) {
  return removeImpl(HashedKey{key}, *findInternal(key), DeleteTombStoneGuard{},
                    false /* removeFromNvm */) == RemoveRes::kSuccess;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::removeFromNvmForTesting(
    typename Item::Key key) {
  if (nvmCache_) {
    HashedKey hk{key};
    nvmCache_->remove(hk, nvmCache_->createDeleteTombStone(hk));
  }
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::pushToNvmCacheFromRamForTesting(
    typename Item::Key key) {
  return pushToNvmCacheFromRam(key);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::pushToNvmCacheFromRam(typename Item::Key key) {
  auto handle = findInternal(key);

  if (!handle || !nvmCache_ || !shouldWriteToNvmCache(*handle) ||
      !shouldWriteToNvmCacheExclusive(*handle)) {
    return false;
  }
  // put() writes nothing without a valid token.
  auto putTokenRv =
      nvmCache_->createPutToken(handle->getKey(), []() { return true; });
  if (!putTokenRv) {
    return false;
  }
  nvmCache_->put(*handle, std::move(*putTokenRv));
  return true;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::flushNvmCache() {
  if (nvmCache_) {
    nvmCache_->flushPendingOps();
  }
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::RemoveRes
CacheAllocator<CacheTrait>::remove(AccessIterator& it) {
  stats_.numCacheRemoves.inc();
  recordEvent(AllocatorApiEvent::REMOVE, it->getKey(),
              AllocatorApiResult::REMOVED, it.operator->());
  HashedKey hk{it->getKey()};
  auto tombstone =
      nvmCache_ ? nvmCache_->createDeleteTombStone(hk) : DeleteTombStoneGuard{};
  return removeImpl(hk, *it, std::move(tombstone), true /* removeFromNvm */,
                    false /* recordApiEvent */);
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::RemoveRes
CacheAllocator<CacheTrait>::remove(const ReadHandle& it) {
  stats_.numCacheRemoves.inc();
  if (!it) {
    throw std::invalid_argument("Trying to remove a null item handle");
  }
  HashedKey hk{it->getKey()};
  auto tombstone =
      nvmCache_ ? nvmCache_->createDeleteTombStone(hk) : DeleteTombStoneGuard{};
  return removeImpl(hk, *(it.getInternal()), std::move(tombstone));
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::RemoveRes
CacheAllocator<CacheTrait>::removeImpl(HashedKey hk,
                                       Item& item,
                                       DeleteTombStoneGuard tombstone,
                                       bool removeFromNvm,
                                       bool recordApiEvent) {
  bool success = false;
  {
    auto lock =
        nvmCache_ ? nvmCache_->getItemDestructorLock(hk)
                  : std::unique_lock<typename NvmCacheT::ItemDestructorMutex>();

    success = accessContainer_->remove(item, RemoveContext::kNormal,
                                       config_.preRemoveCb);

    if (removeFromNvm && success && item.isNvmClean() && !item.isNvmEvicted()) {
      // item is to be removed and the destructor will be executed
      // upon memory released, mark it in nvm to avoid destructor
      // executed from nvm
      nvmCache_->markNvmItemRemovedLocked(hk);
    }
  }
  XDCHECK(!item.isAccessible());

  // remove it from the mm container. this will be no-op if it is already
  // removed.
  removeFromMMContainer(item);

  // Enqueue delete to nvmCache if we know from the item that it was pulled in
  // from NVM. If the item was not pulled in from NVM, it is not possible to
  // have it be written to NVM.
  if (removeFromNvm && item.isNvmClean()) {
    XDCHECK(tombstone);
    nvmCache_->remove(hk, std::move(tombstone));
  }

  if (recordApiEvent) {
    const auto result =
        success ? AllocatorApiResult::REMOVED : AllocatorApiResult::NOT_FOUND;
    recordEvent(AllocatorApiEvent::REMOVE, item.getKey(), result, &item);
  }

  // the last guy with reference to the item will release it back to the
  // allocator.
  if (success) {
    stats_.numCacheRemoveRamHits.inc();
    return RemoveRes::kSuccess;
  }
  return RemoveRes::kNotFoundInRam;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::invalidateNvm(Item& item) {
  if (nvmCache_ != nullptr && item.isAccessible() && item.isNvmClean()) {
    HashedKey hk{item.getKey()};
    {
      auto lock = nvmCache_->getItemDestructorLock(hk);
      if (!item.isNvmEvicted() && item.isNvmClean() && item.isAccessible()) {
        // item is being updated and invalidated in nvm. Mark the item to avoid
        // destructor to be executed from nvm
        nvmCache_->markNvmItemRemovedLocked(hk);
      }
      item.unmarkNvmClean();
    }
    nvmCache_->remove(hk, nvmCache_->createDeleteTombStone(hk));
  }
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::MMContainer&
CacheAllocator<CacheTrait>::getMMContainer(const Item& item) const noexcept {
  const auto allocInfo =
      allocator_->getAllocInfo(static_cast<const void*>(&item));
  return getMMContainer(allocInfo.poolId, allocInfo.classId, item);
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::MMContainer&
CacheAllocator<CacheTrait>::getMMContainer(PoolId pid,
                                           ClassId cid,
                                           const Item& item) const noexcept {
  if (!isMMContainerSharded()) [[likely]] {
    return getMMContainer(pid, cid);
  }
  return getMMContainerShard(pid, cid, getMMShardIndex(item));
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::MMContainer&
CacheAllocator<CacheTrait>::getMMContainer(PoolId pid,
                                           ClassId cid) const noexcept {
  XDCHECK_LT(static_cast<size_t>(pid), mmContainers_.size());
  XDCHECK_LT(static_cast<size_t>(cid), mmContainers_[pid].size());
  return *mmContainers_[pid][cid];
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::MMContainer&
CacheAllocator<CacheTrait>::getMMContainerShard(PoolId pid,
                                                ClassId cid,
                                                uint32_t shard) const noexcept {
  if (shard == 0) {
    return getMMContainer(pid, cid);
  }
  XDCHECK_LT(shard, config_.mmContainerShards);
  return *(*mmContainerShards_)[pid][cid].extraShards[shard - 1];
}

template <typename CacheTrait>
uint32_t CacheAllocator<CacheTrait>::getMMShardIndex(
    const Item& item) const noexcept {
  XDCHECK(isMMContainerSharded());
  // Fibonacci hash of the item's offset from the start of slab memory. Unlike
  // the key (chained items are rekeyed when their parent moves), the offset is
  // fixed while the memory is allocated, is safe to compute for an item that is
  // concurrently being freed, and survives a warm roll remapping the slabs.
  constexpr uint64_t kFibonacciMultiplier = 0x9E3779B97F4A7C15ULL;
  const uint64_t offset =
      reinterpret_cast<uintptr_t>(&item) -
      reinterpret_cast<uintptr_t>(allocator_->getSlabMemoryInfo().first);
  return static_cast<uint32_t>(
      (offset * kFibonacciMultiplier) >>
      (64 - std::countr_zero(config_.mmContainerShards)));
}

template <typename CacheTrait>
template <typename Fn>
void CacheAllocator<CacheTrait>::forEachMMContainerShard(PoolId pid,
                                                         ClassId cid,
                                                         Fn&& fn) const {
  fn(getMMContainer(pid, cid));
  if (isMMContainerSharded()) {
    for (const auto& shard : (*mmContainerShards_)[pid][cid].extraShards) {
      fn(*shard);
    }
  }
}

template <typename CacheTrait>
uint32_t CacheAllocator<CacheTrait>::pickEvictionShard(
    PoolId pid, ClassId cid) const noexcept {
  // xorshift64, per thread so concurrent evictors spread over the shards
  thread_local uint64_t rng = folly::Random::rand64() | 1;
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;

  const uint32_t mask = config_.mmContainerShards - 1;
  const auto first = static_cast<uint32_t>(rng) & mask;
  // uniform over the other N-1 shards
  const auto offset = static_cast<uint32_t>(((rng >> 32) * mask) >> 32);
  const auto second = (first + 1 + offset) & mask;

  const auto& tailTimes = (*mmContainerShards_)[pid][cid].tailTimes;
  return tailTimes[second].load(std::memory_order_relaxed) <
                 tailTimes[first].load(std::memory_order_relaxed)
             ? second
             : first;
}

template <typename CacheTrait>
MMContainerStat CacheAllocator<CacheTrait>::getMMContainerStat(
    PoolId pid, ClassId cid) const noexcept {
  if (!isMMContainerSharded()) {
    return getMMContainer(pid, cid).getStats();
  }
  MMContainerStat total{};
  forEachMMContainerShard(pid, cid, [&total](const MMContainer& mm) {
    const auto stat = mm.getStats();
    total.size += stat.size;
    // an empty shard reports 0
    if (stat.oldestTimeSec != 0 && (total.oldestTimeSec == 0 ||
                                    stat.oldestTimeSec < total.oldestTimeSec)) {
      total.oldestTimeSec = stat.oldestTimeSec;
    }
    total.lruRefreshTime = std::max(total.lruRefreshTime, stat.lruRefreshTime);
    total.numHotAccesses += stat.numHotAccesses;
    total.numColdAccesses += stat.numColdAccesses;
    total.numWarmAccesses += stat.numWarmAccesses;
    total.numTailAccesses += stat.numTailAccesses;
  });
  return total;
}

template <typename CacheTrait>
EvictionAgeStat CacheAllocator<CacheTrait>::getMMContainerEvictionAgeStat(
    PoolId pid, ClassId cid, uint64_t projectedLength) const noexcept {
  if (!isMMContainerSharded()) {
    return getMMContainer(pid, cid).getEvictionAgeStat(projectedLength);
  }
  // Items spread evenly over the shards, so removing projectedLength items
  // from the merged tail removes about projectedLength / N from each shard,
  // and the oldest remaining item is the oldest of the shards' projected tails.
  const uint32_t numShards = config_.mmContainerShards;
  EvictionAgeStat total{};
  auto merge = [](EvictionStatPerType& acc, const EvictionStatPerType& stat) {
    acc.oldestElementAge =
        std::max(acc.oldestElementAge, stat.oldestElementAge);
    acc.size += stat.size;
    acc.projectedAge = std::max(acc.projectedAge, stat.projectedAge);
  };
  forEachMMContainerShard(
      pid, cid,
      [&total, &merge, projectedLength, numShards](const MMContainer& mm) {
        const auto stat = mm.getEvictionAgeStat(projectedLength / numShards);
        merge(total.warmQueueStat, stat.warmQueueStat);
        merge(total.hotQueueStat, stat.hotQueueStat);
        merge(total.coldQueueStat, stat.coldQueueStat);
      });
  return total;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::ReadHandle
CacheAllocator<CacheTrait>::peek(typename Item::Key key) {
  return findInternalWithExpiration(key, AllocatorApiEvent::PEEK);
}

template <typename CacheTrait>
StorageMedium CacheAllocator<CacheTrait>::existFast(typename Item::Key key) {
  // At this point, a key either definitely exists or does NOT exist in cache

  // We treat this as a peek, since couldExist() shouldn't actually promote
  // an item as we expect the caller to issue a regular find soon afterwards.
  auto handle = findInternalWithExpiration(key, AllocatorApiEvent::PEEK);
  if (handle) {
    return StorageMedium::DRAM;
  }

  // When we have to go to NvmCache, we can only probalistically determine
  // if a key could possibly exist in cache, or definitely NOT exist.
  if (nvmCache_ && nvmCache_->couldExistFast(HashedKey{key})) {
    return StorageMedium::NVM;
  } else {
    return StorageMedium::NONE;
  }
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::couldExistFast(typename Item::Key key) {
  if (existFast(key) == StorageMedium::NONE) {
    return false;
  } else {
    return true;
  }
}

template <typename CacheTrait>
std::pair<typename CacheAllocator<CacheTrait>::ReadHandle,
          typename CacheAllocator<CacheTrait>::ReadHandle>
CacheAllocator<CacheTrait>::inspectCache(typename Item::Key key) {
  std::pair<ReadHandle, ReadHandle> res;
  res.first = findInternal(key);
  res.second = nvmCache_ ? nvmCache_->peek(key) : nullptr;
  return res;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::findInternalWithExpiration(
    Key key, AllocatorApiEvent event) {
  bool needToBumpStats =
      event == AllocatorApiEvent::FIND || event == AllocatorApiEvent::FIND_FAST;
  if (needToBumpStats) {
    stats_.numCacheGets.inc();
  }

  XDCHECK(event == AllocatorApiEvent::FIND ||
          event == AllocatorApiEvent::FIND_FAST ||
          event == AllocatorApiEvent::PEEK)
      << magic_enum::enum_name(event);

  auto handle = findInternal(key);
  if (UNLIKELY(!handle)) {
    if (needToBumpStats) {
      stats_.numCacheGetMiss.inc();
      recordEvent(event, key, AllocatorApiResult::NOT_FOUND);
    }
    return handle;
  }

  if (UNLIKELY(handle->isExpired())) {
    // update cache miss stats if the item has already been expired.
    if (needToBumpStats) {
      stats_.numCacheGetMiss.inc();
      stats_.numCacheGetExpiries.inc();
      recordEvent(event, key, AllocatorApiResult::EXPIRED, handle);
    }
    WriteHandle ret;
    ret.markExpired();
    return ret;
  }

  if (needToBumpStats) {
    recordEvent(event, key, AllocatorApiResult::FOUND, handle);
  }
  auto lastAccess = handle->getLastAccessTime();
  if (lastAccess > 0) {
    handle.setTTASecs(util::getCurrentTimeSec() - lastAccess);
  }
  return handle;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::findFastImpl(typename Item::Key key,
                                         AccessMode mode) {
  auto handle = findInternalWithExpiration(key, AllocatorApiEvent::FIND_FAST);
  if (!handle) {
    return handle;
  }

  markUseful(handle, mode);
  return handle;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::ReadHandle
CacheAllocator<CacheTrait>::findFast(typename Item::Key key) {
  return findFastImpl(key, AccessMode::kRead);
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::findFastToWrite(typename Item::Key key) {
  auto handle = findFastImpl(key, AccessMode::kWrite);
  if (handle == nullptr) {
    return nullptr;
  }

  invalidateNvm(*handle);
  return handle;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::findImpl(typename Item::Key key, AccessMode mode) {
  auto handle = findInternalWithExpiration(key, AllocatorApiEvent::FIND);
  if (handle) {
    markUseful(handle, mode);
    return handle;
  }

  if (!nvmCache_) {
    return handle;
  }

  // Hybrid-cache's dram miss-path. Handle becomes async once we look up from
  // nvm-cache. Naively accessing the memory directly after this can be slow.
  // We also don't need to call `markUseful()` as if we have a hit, we will
  // have promoted this item into DRAM cache at the front of eviction queue.
  return nvmCache_->find(HashedKey{key});
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::findToWrite(typename Item::Key key) {
  auto handle = findImpl(key, AccessMode::kWrite);
  if (handle == nullptr) {
    return nullptr;
  }
  invalidateNvm(*handle);
  return handle;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::ReadHandle
CacheAllocator<CacheTrait>::find(typename Item::Key key) {
  return findImpl(key, AccessMode::kRead);
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::markUseful(const ReadHandle& handle,
                                            AccessMode mode) {
  if (!handle) {
    return;
  }

  auto& item = *(handle.getInternal());
  bool recorded = recordAccessInMMContainer(item, mode);

  // if parent is not recorded, skip children as well when the config is set
  if (LIKELY(!item.hasChainedItem() ||
             (!recorded && config_.isSkipPromoteChildrenWhenParentFailed()))) {
    return;
  }

  forEachChainedItem(item, [this, mode](ChainedItem& chainedItem) {
    recordAccessInMMContainer(chainedItem, mode);
  });
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::recordAccessInMMContainer(Item& item,
                                                           AccessMode mode) {
  const auto allocInfo =
      allocator_->getAllocInfo(static_cast<const void*>(&item));
  (*stats_.cacheHits)[allocInfo.poolId][allocInfo.classId].inc();

  // track recently accessed items if needed
  if (UNLIKELY(config_.trackRecentItemsForDump)) {
    ring_->trackItem(reinterpret_cast<uintptr_t>(&item), item.getSize());
  }

  auto& mmContainer = getMMContainer(allocInfo.poolId, allocInfo.classId, item);
  return mmContainer.recordAccess(item, mode);
}

template <typename CacheTrait>
uint32_t CacheAllocator<CacheTrait>::getUsableSize(const Item& item) const {
  const auto allocSize =
      allocator_->getAllocInfo(static_cast<const void*>(&item)).allocSize;
  return item.isChainedItem()
             ? allocSize - ChainedItem::getRequiredSize(0)
             : allocSize - Item::getRequiredSize(item.getKey(), 0);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::isKeyValid(Key key) const {
  return config_.allowLargeKeys ? KAllocation::isKeyValid(key)
                                : KAllocation::isSmallKeyValid(key);
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::throwIfKeyInvalid(Key key) const {
  if (config_.allowLargeKeys) {
    KAllocation::throwIfKeyInvalid(key);
  } else {
    KAllocation::throwIfSmallKeyInvalid(key);
  }
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::SampleItem
CacheAllocator<CacheTrait>::getSampleItem() {
  size_t nvmCacheSize = nvmCache_ ? nvmCache_->getUsableSize() : 0;
  size_t ramCacheSize = allocator_->getMemorySizeInclAdvised();

  bool fromNvm =
      folly::Random::rand64(0, nvmCacheSize + ramCacheSize) >= ramCacheSize;
  if (fromNvm) {
    return nvmCache_->getSampleItem();
  }

  // Sampling from DRAM cache
  auto [allocSize, rawItem] = allocator_->getRandomAlloc();
  auto handle = findFromRawAlloc(rawItem, allocSize);
  if (!handle) {
    return SampleItem{false /* fromNvm */};
  }

  auto item = handle.get();
  auto sharedHdl = std::make_shared<ReadHandle>(std::move(handle));

  const auto allocInfo = allocator_->getAllocInfo(item->getMemory());

  // Convert the Item to IOBuf to make SampleItem
  auto iobuf = folly::IOBuf{
      folly::IOBuf::TAKE_OWNERSHIP, sharedHdl->getInternal(),
      item->getOffsetForMemory() + item->getSize(),
      [](void* /*unused*/, void* userData) {
        auto* hdl = reinterpret_cast<std::shared_ptr<ReadHandle>*>(userData);
        delete hdl;
      } /* freeFunc */,
      new std::shared_ptr<ReadHandle>{sharedHdl} /* userData for freeFunc */};

  iobuf.markExternallySharedOne();

  return SampleItem(std::move(iobuf), allocInfo, false /* fromNvm */);
}

template <typename CacheTrait>
std::vector<std::string> CacheAllocator<CacheTrait>::dumpEvictionIterator(
    PoolId pid, ClassId cid, size_t numItems) {
  if (numItems == 0) {
    return {};
  }

  if (static_cast<size_t>(pid) >= mmContainers_.size() ||
      static_cast<size_t>(cid) >= mmContainers_[pid].size()) {
    throw std::invalid_argument(
        fmt::format("Invalid PoolId: {} and ClassId: {}.", pid, cid));
  }

  std::vector<std::string> content;

  // a sharded container is dumped one shard after another, not in global LRU
  // order
  forEachMMContainerShard(pid, cid, [&content, numItems](MMContainer& mm) {
    auto evictItr = mm.getEvictionIterator();
    while (evictItr && content.size() < numItems) {
      content.push_back(evictItr->toString());
      ++evictItr;
    }
  });

  return content;
}

template <typename CacheTrait>
template <typename Handle>
folly::IOBuf CacheAllocator<CacheTrait>::convertToIOBufT(Handle& handle) {
  if (!handle) {
    throw std::invalid_argument("null item handle for converting to IOBUf");
  }

  Item* item = handle.getInternal();
  const uint32_t dataOffset = item->getOffsetForMemory();

  using ConvertChainedItem = std::function<std::unique_ptr<folly::IOBuf>(
      Item * item, ChainedItem & chainedItem)>;
  folly::IOBuf iobuf;
  ConvertChainedItem converter;

  // based on current refcount and threshold from config
  // determine to use a new Item Handle for each chain items
  // or use shared Item Handle for all chain items
  if (item->getRefCount() > config_.thresholdForConvertingToIOBuf) {
    auto sharedHdl = std::make_shared<Handle>(std::move(handle));

    iobuf = folly::IOBuf{
        folly::IOBuf::TAKE_OWNERSHIP, item,

        // Since we'll be moving the IOBuf data pointer forward
        // by dataOffset, we need to adjust the IOBuf length
        // accordingly
        dataOffset + item->getSize(),

        [](void* /*unused*/, void* userData) {
          auto* hdl = reinterpret_cast<std::shared_ptr<Handle>*>(userData);
          delete hdl;
        } /* freeFunc */,
        new std::shared_ptr<Handle>{sharedHdl} /* userData for freeFunc */};

    if (item->hasChainedItem()) {
      converter = [sharedHdl](Item*, ChainedItem& chainedItem) {
        const uint32_t chainedItemDataOffset = chainedItem.getOffsetForMemory();

        return folly::IOBuf::takeOwnership(
            &chainedItem,

            // Since we'll be moving the IOBuf data pointer forward by
            // dataOffset,
            // we need to adjust the IOBuf length accordingly
            chainedItemDataOffset + chainedItem.getSize(),

            [](void*, void* userData) {
              auto* hdl = reinterpret_cast<std::shared_ptr<Handle>*>(userData);
              delete hdl;
            } /* freeFunc */,
            new std::shared_ptr<Handle>{sharedHdl} /* userData for freeFunc */);
      };
    }

  } else {
    // following IOBuf will take the item's ownership and trigger freeFunc to
    // release the reference count.
    handle.release();
    iobuf = folly::IOBuf{folly::IOBuf::TAKE_OWNERSHIP, item,

                         // Since we'll be moving the IOBuf data pointer forward
                         // by dataOffset, we need to adjust the IOBuf length
                         // accordingly
                         dataOffset + item->getSize(),

                         [](void* buf, void* userData) {
                           Handle{reinterpret_cast<Item*>(buf),
                                  *reinterpret_cast<decltype(this)>(userData)}
                               .reset();
                         } /* freeFunc */,
                         this /* userData for freeFunc */};

    if (item->hasChainedItem()) {
      converter = [this](Item* parentItem, ChainedItem& chainedItem) {
        const uint32_t chainedItemDataOffset = chainedItem.getOffsetForMemory();

        // Each IOBuf converted from a child item will hold one additional
        // refcount on the parent item. This ensures that as long as the user
        // holds any IOBuf pointing anywhere in the chain, the whole chain
        // will not be evicted from cache.
        //
        // We can safely bump the refcount on the parent here only because
        // we already have an item handle on the parent (which has just been
        // moved into the IOBuf above). Normally, the only place we can
        // bump an item handle safely is through the AccessContainer.
        acquire(parentItem).release();

        return folly::IOBuf::takeOwnership(
            &chainedItem,

            // Since we'll be moving the IOBuf data pointer forward by
            // dataOffset,
            // we need to adjust the IOBuf length accordingly
            chainedItemDataOffset + chainedItem.getSize(),

            [](void* buf, void* userData) {
              auto* cache = reinterpret_cast<decltype(this)>(userData);
              auto* child = reinterpret_cast<ChainedItem*>(buf);
              auto* parent = &child->getParentItem(cache->compressor_);
              Handle{parent, *cache}.reset();
            } /* freeFunc */,
            this /* userData for freeFunc */);
      };
    }
  }

  iobuf.trimStart(dataOffset);
  iobuf.markExternallySharedOne();

  if (item->hasChainedItem()) {
    auto appendHelper = [&](ChainedItem& chainedItem) {
      const uint32_t chainedItemDataOffset = chainedItem.getOffsetForMemory();

      auto nextChain = converter(item, chainedItem);

      nextChain->trimStart(chainedItemDataOffset);
      nextChain->markExternallySharedOne();

      // Append immediately after the parent, IOBuf will present the data
      // in the original insertion order.
      //
      // i.e. 1. Allocate parent
      //      2. add A, add B, add C
      //
      //      In memory: parent -> C -> B -> A
      //      In IOBuf:  parent -> A -> B -> C
      iobuf.appendChain(std::move(nextChain));
    };

    forEachChainedItem(*item, std::move(appendHelper));
  }

  return iobuf;
}

template <typename CacheTrait>
folly::IOBuf CacheAllocator<CacheTrait>::wrapAsIOBuf(const Item& item) {
  folly::IOBuf ioBuf{folly::IOBuf::WRAP_BUFFER, item.getMemory(),
                     item.getSize()};

  if (item.hasChainedItem()) {
    auto appendHelper = [&](ChainedItem& chainedItem) {
      auto nextChain = folly::IOBuf::wrapBuffer(chainedItem.getMemory(),
                                                chainedItem.getSize());

      // Append immediately after the parent, IOBuf will present the data
      // in the original insertion order.
      //
      // i.e. 1. Allocate parent
      //      2. add A, add B, add C
      //
      //      In memory: parent -> C -> B -> A
      //      In IOBuf:  parent -> A -> B -> C
      ioBuf.appendChain(std::move(nextChain));
    };

    forEachChainedItem(item, std::move(appendHelper));
  }
  return ioBuf;
}

template <typename CacheTrait>
PoolId CacheAllocator<CacheTrait>::addPool(
    folly::StringPiece name,
    size_t size,
    const std::set<uint32_t>& allocSizes,
    MMConfig config,
    std::shared_ptr<RebalanceStrategy> rebalanceStrategy,
    std::shared_ptr<RebalanceStrategy> resizeStrategy,
    bool ensureProvisionable) {
  std::unique_lock w(poolsResizeAndRebalanceLock_);
  auto pid = allocator_->addPool(name, size, allocSizes, ensureProvisionable);
  createMMContainers(pid, std::move(config));
  setRebalanceStrategy(pid, std::move(rebalanceStrategy));
  setResizeStrategy(pid, std::move(resizeStrategy));

  if (backgroundEvictor_.size()) {
    auto memoryAssignments =
        createBgWorkerMemoryAssignments(backgroundEvictor_.size());
    for (size_t id = 0; id < backgroundEvictor_.size(); id++) {
      backgroundEvictor_[id]->setAssignedMemory(
          std::move(memoryAssignments[id]));
    }
  }

  if (backgroundPromoter_.size()) {
    auto memoryAssignments =
        createBgWorkerMemoryAssignments(backgroundPromoter_.size());
    for (size_t id = 0; id < backgroundPromoter_.size(); id++) {
      backgroundPromoter_[id]->setAssignedMemory(
          std::move(memoryAssignments[id]));
    }
  }

  return pid;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::provisionPool(
    PoolId poolId, const std::vector<uint32_t>& slabsDistribution) {
  std::unique_lock w(poolsResizeAndRebalanceLock_);
  return allocator_->provisionPool(poolId, slabsDistribution);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::provisionPoolWithPowerLaw(
    PoolId poolId, double power, uint32_t minSlabsPerAC) {
  const auto& poolSize = allocator_->getPool(poolId).getPoolSize();
  const uint32_t numACs =
      allocator_->getPool(poolId).getStats().classIds.size();
  const uint32_t numSlabs = poolSize / Slab::kSize;
  const uint32_t minSlabsRequired = numACs * minSlabsPerAC;
  if (numSlabs < minSlabsRequired) {
    XLOGF(ERR,
          "Insufficinet slabs to satisfy minSlabPerAC. PoolID: {}, Need: {}, "
          "Actual: {}",
          poolId, minSlabsRequired, numSlabs);
    return false;
  }

  std::vector<uint32_t> slabsDistribution(numACs, minSlabsPerAC);
  const uint32_t remainingSlabs = numSlabs - minSlabsRequired;

  auto calcPowerLawSum = [](int n, double p) {
    double sum = 0;
    for (int i = 1; i <= n; ++i) {
      sum += std::pow(i, -p);
    }
    return sum;
  };

  const double powerLawSum = calcPowerLawSum(numACs, power);
  for (uint32_t i = 0, allocatedSlabs = 0;
       i < numACs && allocatedSlabs < remainingSlabs; i++) {
    const uint32_t slabsToAllocate =
        std::min(static_cast<uint32_t>(remainingSlabs *
                                       std::pow(i + 1, -power) / powerLawSum),
                 remainingSlabs - allocatedSlabs);
    slabsDistribution[i] += slabsToAllocate;
    allocatedSlabs += slabsToAllocate;
  }

  return provisionPool(poolId, slabsDistribution);
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::overridePoolRebalanceStrategy(
    PoolId pid, std::shared_ptr<RebalanceStrategy> rebalanceStrategy) {
  if (static_cast<size_t>(pid) >= mmContainers_.size()) {
    throw std::invalid_argument(fmt::format(
        "Invalid PoolId: {}, size of pools: {}", pid, mmContainers_.size()));
  }
  setRebalanceStrategy(pid, std::move(rebalanceStrategy));
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::overridePoolResizeStrategy(
    PoolId pid, std::shared_ptr<RebalanceStrategy> resizeStrategy) {
  if (static_cast<size_t>(pid) >= mmContainers_.size()) {
    throw std::invalid_argument(fmt::format(
        "Invalid PoolId: {}, size of pools: {}", pid, mmContainers_.size()));
  }
  setResizeStrategy(pid, std::move(resizeStrategy));
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::overridePoolOptimizeStrategy(
    std::shared_ptr<PoolOptimizeStrategy> optimizeStrategy) {
  setPoolOptimizeStrategy(std::move(optimizeStrategy));
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::overridePoolConfig(PoolId pid,
                                                    const MMConfig& config) {
  if (static_cast<size_t>(pid) >= mmContainers_.size()) {
    throw std::invalid_argument(fmt::format(
        "Invalid PoolId: {}, size of pools: {}", pid, mmContainers_.size()));
  }

  auto& pool = allocator_->getPool(pid);
  for (unsigned int cid = 0; cid < pool.getNumClassId(); ++cid) {
    DCHECK_NOTNULL(mmContainers_[pid][cid].get());
    for (uint32_t shard = 0; shard < config_.mmContainerShards; ++shard) {
      MMConfig mmConfig = config;
      mmConfig.addExtraConfig(
          getMMContainerTailSize(pool, static_cast<ClassId>(cid), shard));
      getMMContainerShard(pid, static_cast<ClassId>(cid), shard)
          .setConfig(mmConfig);
    }
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::createMMContainers(const PoolId pid,
                                                    MMConfig config) {
  auto& pool = allocator_->getPool(pid);
  for (unsigned int cid = 0; cid < pool.getNumClassId(); ++cid) {
    config.addExtraConfig(
        getMMContainerTailSize(pool, static_cast<ClassId>(cid), 0));
    mmContainers_[pid][cid].reset(new MMContainer(config, compressor_));
    if (isMMContainerSharded()) {
      initMMContainerShardSet(pid, static_cast<ClassId>(cid));
      auto& extraShards = (*mmContainerShards_)[pid][cid].extraShards;
      for (uint32_t shard = 1; shard < config_.mmContainerShards; ++shard) {
        config.addExtraConfig(
            getMMContainerTailSize(pool, static_cast<ClassId>(cid), shard));
        extraShards[shard - 1].reset(new MMContainer(config, compressor_));
      }
    }
  }
}

template <typename CacheTrait>
size_t CacheAllocator<CacheTrait>::getMMContainerTailSize(
    const MemoryPool& pool, ClassId cid, uint32_t shard) const {
  if (!config_.trackTailHits) {
    return 0;
  }
  // Split one slab's items evenly across the shards; a shard may get 0 and then
  // doesn't track tail hits. Keep the split stable: each shard persists whether
  // tracking is on, and attach fails if that changes.
  const size_t allocsPerSlab = pool.getAllocationClass(cid).getAllocsPerSlab();
  const uint32_t numShards = config_.mmContainerShards;
  return allocsPerSlab / numShards +
         (shard < allocsPerSlab % numShards ? 1 : 0);
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::initMMContainerShardSet(PoolId pid,
                                                         ClassId cid) {
  auto& shardSet = (*mmContainerShards_)[pid][cid];
  shardSet.extraShards.resize(config_.mmContainerShards - 1);
  shardSet.tailTimes =
      std::make_unique<std::atomic<uint32_t>[]>(config_.mmContainerShards);
}

template <typename CacheTrait>
PoolId CacheAllocator<CacheTrait>::getPoolId(
    folly::StringPiece name) const noexcept {
  return allocator_->getPoolId(name);
}

// The Function returns a consolidated vector of Release Slab
// events from Pool Workers { Pool rebalancer, Pool Resizer and
// Memory Monitor}.
template <typename CacheTrait>
AllSlabReleaseEvents CacheAllocator<CacheTrait>::getAllSlabReleaseEvents(
    PoolId poolId) const {
  AllSlabReleaseEvents res;
  // lock protects against workers being restarted
  {
    std::lock_guard<std::mutex> l(workersMutex_);
    if (poolRebalancer_) {
      res.rebalancerEvents = poolRebalancer_->getSlabReleaseEvents(poolId);
    }
    if (poolResizer_) {
      res.resizerEvents = poolResizer_->getSlabReleaseEvents(poolId);
    }
    if (memMonitor_) {
      res.monitorEvents = memMonitor_->getSlabReleaseEvents(poolId);
    }
  }
  return res;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::reclaimUnmonitoredSlabs(
    const MemoryMonitor::Config& config, std::chrono::milliseconds interval) {
  std::set<PoolId> monitoredPools;
  if (!config.disabled() && interval.count() > 0) {
    if (config.poolIds.empty()) {
      return; // monitoring enabled for all pools, no need to reclaim
    }
    monitoredPools = config.poolIds;
  }
  for (const auto pid : getRegularPoolIds()) {
    if (monitoredPools.contains(pid)) {
      continue;
    }
    const auto numAdvised = getPool(pid).getNumSlabsAdvised();
    if (numAdvised == 0) {
      continue;
    }
    const auto numReclaimed = reclaimSlabs(pid, numAdvised);
    if (numReclaimed != numAdvised) {
      throw std::logic_error(fmt::format(
          "Reclaimed {} of {} advised slabs from unmonitored pool {}",
          numReclaimed, numAdvised, static_cast<int>(pid)));
    }
    XLOG(INFO) << "Reclaimed " << numReclaimed
               << " advised slabs from unmonitored pool "
               << static_cast<int>(pid);
  }
}

template <typename CacheTrait>
std::set<PoolId> CacheAllocator<CacheTrait>::filterCompactCachePools(
    const PoolIds& poolIds) const {
  PoolIds ret;
  std::shared_lock lock(compactCachePoolsLock_);
  for (auto poolId : poolIds) {
    if (!isCompactCachePool_[poolId]) {
      // filter out slab pools backing the compact caches.
      ret.insert(poolId);
    }
  }
  return ret;
}

template <typename CacheTrait>
std::set<PoolId> CacheAllocator<CacheTrait>::getRegularPoolIds() const {
  std::shared_lock r(poolsResizeAndRebalanceLock_);
  return filterCompactCachePools(allocator_->getPoolIds());
}

template <typename CacheTrait>
std::set<PoolId> CacheAllocator<CacheTrait>::getCCachePoolIds() const {
  PoolIds ret;
  std::shared_lock lock(compactCachePoolsLock_);
  for (PoolId id = 0; id < static_cast<PoolId>(MemoryPoolManager::kMaxPools);
       id++) {
    if (isCompactCachePool_[id]) {
      // filter out slab pools backing the compact caches.
      ret.insert(id);
    }
  }
  return ret;
}

template <typename CacheTrait>
std::set<PoolId> CacheAllocator<CacheTrait>::getRegularPoolIdsForResize()
    const {
  std::shared_lock r(poolsResizeAndRebalanceLock_);
  // If Slabs are getting advised away - as indicated by non-zero
  // getAdvisedMemorySize - then pools may be overLimit even when
  // all slabs are not allocated. Otherwise, pools may be overLimit
  // only after all slabs are allocated.
  //
  return (allocator_->allSlabsAllocated()) ||
                 (allocator_->getAdvisedMemorySize() != 0)
             ? filterCompactCachePools(allocator_->getPoolsOverLimit())
             : std::set<PoolId>{};
}

template <typename CacheTrait>
const std::string CacheAllocator<CacheTrait>::getCacheName() const {
  return config_.cacheName;
}

template <typename CacheTrait>
PoolStats CacheAllocator<CacheTrait>::getPoolStats(PoolId poolId) const {
  stats().numExpensiveStatsPolled.inc();

  const auto& pool = allocator_->getPool(poolId);
  const auto& allocSizes = pool.getAllocSizes();
  auto mpStats = pool.getStats();
  const auto& classIds = mpStats.classIds;

  // check if this is a compact cache.
  bool isCompactCache = false;
  {
    std::shared_lock lock(compactCachePoolsLock_);
    isCompactCache = isCompactCachePool_[poolId];
  }

  folly::F14FastMap<ClassId, CacheStat> cacheStats;
  uint64_t totalHits = 0;
  // cacheStats is only menaningful for pools that are not compact caches.
  // TODO export evictions, numItems etc from compact cache directly.
  if (!isCompactCache) {
    for (const ClassId cid : classIds) {
      uint64_t classHits = (*stats_.cacheHits)[poolId][cid].get();
      XDCHECK(mmContainers_[poolId][cid],
              fmt::format("Pid {}, Cid {} not initialized.", poolId, cid));
      cacheStats.insert(
          {cid,
           {allocSizes[cid], (*stats_.allocAttempts)[poolId][cid].get(),
            (*stats_.evictionAttempts)[poolId][cid].get(),
            (*stats_.allocFailures)[poolId][cid].get(),
            fragmentationSizeStat(poolId, cid), classHits,
            (*stats_.chainedItemEvictions)[poolId][cid].get(),
            (*stats_.regularItemEvictions)[poolId][cid].get(),
            getMMContainerStat(poolId, cid)}

          });
      totalHits += classHits;
    }
  }

  PoolStats ret;
  ret.isCompactCache = isCompactCache;
  ret.poolName = allocator_->getPoolName(poolId);
  ret.poolSize = pool.getPoolSize();
  ret.poolUsableSize = pool.getPoolUsableSize();
  ret.poolAdvisedSize = pool.getPoolAdvisedSize();
  ret.cacheStats = std::move(cacheStats);
  ret.mpStats = std::move(mpStats);
  ret.numPoolGetHits = totalHits;
  ret.evictionAgeSecs = stats_.perPoolEvictionAgeSecs_[poolId].estimate();

  return ret;
}

template <typename CacheTrait>
PoolEvictionAgeStats CacheAllocator<CacheTrait>::getPoolEvictionAgeStats(
    PoolId pid, unsigned int slabProjectionLength) const {
  stats().numExpensiveStatsPolled.inc();

  PoolEvictionAgeStats stats;

  const auto& pool = allocator_->getPool(pid);
  const auto& allocSizes = pool.getAllocSizes();
  for (ClassId cid = 0; static_cast<size_t>(cid) < allocSizes.size(); ++cid) {
    const auto numItemsPerSlab =
        allocator_->getPool(pid).getAllocationClass(cid).getAllocsPerSlab();
    const auto projectionLength = numItemsPerSlab * slabProjectionLength;
    stats.classEvictionAgeStats[cid] =
        getMMContainerEvictionAgeStat(pid, cid, projectionLength);
  }

  return stats;
}

template <typename CacheTrait>
CacheMetadata CacheAllocator<CacheTrait>::getCacheMetadata() const noexcept {
  return CacheMetadata{kCachelibVersion, kCacheRamFormatVersion,
                       kCacheNvmFormatVersion, config_.getCacheSize()};
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::releaseSlab(PoolId pid,
                                             ClassId cid,
                                             SlabReleaseMode mode,
                                             const void* hint) {
  releaseSlab(pid, cid, Slab::kInvalidClassId, mode, hint);
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::releaseSlab(PoolId pid,
                                             ClassId victim,
                                             ClassId receiver,
                                             SlabReleaseMode mode,
                                             const void* hint) {
  stats_.numActiveSlabReleases.inc();
  SCOPE_EXIT { stats_.numActiveSlabReleases.dec(); };

  auto incReleaseStats = [this, mode]() {
    switch (mode) {
    case SlabReleaseMode::kRebalance:
      stats_.numReleasedForRebalance.inc();
      break;
    case SlabReleaseMode::kResize:
      stats_.numReleasedForResize.inc();
      break;
    case SlabReleaseMode::kAdvise:
      stats_.numReleasedForAdvise.inc();
      break;
    default:
      break;
    }
  };

  try {
    auto releaseContext = allocator_->startSlabRelease(
        pid, victim, receiver, mode, hint,
        [this]() -> bool { return isFastShutdownTriggered(); });

    // No work needed if the slab is already released
    if (releaseContext.isReleased()) {
      incReleaseStats();
      return;
    }

    releaseSlabImpl(releaseContext);
    if (!allocator_->allAllocsFreed(releaseContext)) {
      throw std::runtime_error(
          fmt::format("Was not able to free all allocs. PoolId: {}, AC: {}",
                      releaseContext.getPoolId(),
                      releaseContext.getClassId()));
    }

    allocator_->completeSlabRelease(releaseContext);
    incReleaseStats();
  } catch (const exception::SlabReleaseAborted& e) {
    incrementAbortedSlabReleases();
    throw exception::SlabReleaseAborted(fmt::format(
        "Slab release aborted while releasing "
        "a slab in pool {} victim {} receiver {}. Original ex msg: {}",
        pid, static_cast<int>(victim), static_cast<int>(receiver), e.what()));
  }
}

template <typename CacheTrait>
SlabReleaseStats CacheAllocator<CacheTrait>::getSlabReleaseStats()
    const noexcept {
  std::lock_guard<std::mutex> l(workersMutex_);
  return SlabReleaseStats{stats_.numActiveSlabReleases.get(),
                          stats_.numReleasedForRebalance.get(),
                          stats_.numReleasedForResize.get(),
                          stats_.numReleasedForAdvise.get(),
                          poolRebalancer_ ? poolRebalancer_->getRunCount()
                                          : 0ULL,
                          poolResizer_ ? poolResizer_->getRunCount() : 0ULL,
                          memMonitor_ ? memMonitor_->getRunCount() : 0ULL,
                          stats_.numMoveAttempts.get(),
                          stats_.numMoveSuccesses.get(),
                          stats_.numEvictionAttempts.get(),
                          stats_.numEvictionSuccesses.get(),
                          stats_.numSlabReleaseStuck.get(),
                          stats_.numAbortedSlabReleases.get()};
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::releaseSlabImpl(
    const SlabReleaseContext& releaseContext) {
  auto startTime = std::chrono::milliseconds(util::getCurrentTimeMs());
  bool releaseStuck = false;

  SCOPE_EXIT {
    if (releaseStuck) {
      stats_.numSlabReleaseStuck.dec();
    }
  };

  util::Throttler throttler(
      config_.throttleConfig,
      [this, &startTime, &releaseStuck](std::chrono::milliseconds curTime) {
        if (!releaseStuck &&
            curTime >= startTime + config_.slabReleaseStuckThreshold) {
          stats().numSlabReleaseStuck.inc();
          releaseStuck = true;
        }
      });

  // Active allocations need to be freed before we can release this slab
  // The idea is:
  //  1. Iterate through each active allocation
  //  2. Under AC lock, acquire ownership of this active allocation
  //  3. If 2 is successful, Move or Evict
  //  4. Move on to the next item if current item is freed
  for (auto alloc : releaseContext.getActiveAllocations()) {
    Item& item = *static_cast<Item*>(alloc);

    // Need to mark an item for release before proceeding
    // If we can't mark as moving, it means the item is already freed
    const bool isAlreadyFreed =
        !markMovingForSlabRelease(releaseContext, alloc, throttler);
    if (isAlreadyFreed) {
      continue;
    }

    // Try to move this item and make sure we can free the memory
    if (!moveForSlabRelease(item)) {
      // If moving fails, evict it
      evictForSlabRelease(item);
    }
    XDCHECK(allocator_->isAllocFreed(releaseContext, alloc));
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::throttleWith(util::Throttler& t,
                                              std::function<void()> fn) {
  const unsigned int rateLimit = 1024;
  // execute every 1024 times we have actually throttled
  if (t.throttle() && (t.numThrottles() % rateLimit) == 0) {
    fn();
  }
}

template <typename CacheTrait>
typename RefcountWithFlags::Value
CacheAllocator<CacheTrait>::unmarkMovingAndWakeUpWaiters(Item& item,
                                                         WriteHandle handle) {
  auto ret = item.unmarkMoving();
  wakeUpWaiters(item.getKey(), std::move(handle));
  return ret;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::moveForSlabRelease(Item& oldItem) {
  if (!config_.moveCb) {
    return false;
  }

  Item* parentItem;
  bool chainedItem = oldItem.isChainedItem();

  stats_.numMoveAttempts.inc();

  if (chainedItem) {
    parentItem = &oldItem.asChainedItem().getParentItem(compressor_);
    XDCHECK(parentItem->isMoving());
    XDCHECK_EQ(1ul, oldItem.getRefCount());
    XDCHECK_EQ(0ul, parentItem->getRefCount());
  } else {
    XDCHECK(oldItem.isMoving());
  }
  WriteHandle newItemHdl = allocateNewItemForOldItem(oldItem);

  // if we have a valid handle, try to move, if not, we attempt to evict.
  if (newItemHdl) {
    // move can fail if another thread calls insertOrReplace
    // in this case oldItem is no longer valid (not accessible,
    // it gets removed from MMContainer and evictForSlabRelease
    // will send it back to the allocator
    bool isMoved = chainedItem
                       ? moveChainedItem(oldItem.asChainedItem(), newItemHdl)
                       : moveRegularItem(oldItem, newItemHdl);
    if (!isMoved) {
      return false;
    }
    removeFromMMContainer(oldItem);
  } else {
    return false;
  }

  const auto allocInfo = allocator_->getAllocInfo(oldItem.getMemory());
  if (chainedItem) {
    newItemHdl.reset();
    // Copy the parent key before unmarkMoving because once we unmark,
    // another thread is free to remove/evict and free the parent item,
    // which would make parentItem->getKey() a dangling StringPiece.
    std::string parentKey(parentItem->getKey().data(),
                          parentItem->getKey().size());
    const auto parentKeyView = Key{folly::StringPiece{parentKey}};
    parentItem->unmarkMoving();
    // We do another lookup here because once we unmark moving, another thread
    // is free to remove/evict the parent item. So its unsafe to increment
    // refcount on the parent item's memory. Instead we rely on a proper lookup.
    auto parentHdl = findInternal(parentKeyView);
    if (!parentHdl) {
      // Parent is gone, so we wake up waiting threads with a null handle.
      wakeUpWaiters(parentKeyView, {});
    } else {
      if (!parentHdl.isReady()) {
        // Parent handle isn't ready. This can be due to the parent got evicted
        // into NvmCache, or another thread is moving the slab that the parent
        // handle is on (e.g. the parent got replaced and the new parent's slab
        // is being moved). In this case, we must wait synchronously and block
        // the current slab moving thread until parent is ready. This is
        // expected to be very rare.
        parentHdl.wait();
      }
      wakeUpWaiters(parentKeyView, std::move(parentHdl));
    }
  } else {
    auto ref = unmarkMovingAndWakeUpWaiters(oldItem, std::move(newItemHdl));
    XDCHECK_EQ(0u, ref);
  }
  (*stats_.fragmentationSize)[allocInfo.poolId][allocInfo.classId].sub(
      util::getFragmentation(*this, oldItem));
  allocator_->free(&oldItem);
  stats_.numMoveSuccesses.inc();
  return true;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::allocateNewItemForOldItem(const Item& oldItem) {
  if (oldItem.isChainedItem()) {
    const Item& parentItem = oldItem.asChainedItem().getParentItem(compressor_);

    auto newItemHdl =
        allocateChainedItemInternal(parentItem, oldItem.getSize());
    if (!newItemHdl) {
      return {};
    }

    const auto& oldChainedItem = oldItem.asChainedItem();
    XDCHECK_EQ(newItemHdl->getSize(), oldChainedItem.getSize());
    XDCHECK_EQ(reinterpret_cast<uintptr_t>(&parentItem),
               reinterpret_cast<uintptr_t>(
                   &oldChainedItem.getParentItem(compressor_)));

    return newItemHdl;
  }

  const auto allocInfo =
      allocator_->getAllocInfo(static_cast<const void*>(&oldItem));

  // Set up the destination for the move. Since oldItem would have the moving
  // bit set, it won't be picked for eviction.
  auto newItemHdl = allocateInternal(allocInfo.poolId,
                                     oldItem.getKey(),
                                     oldItem.getSize(),
                                     oldItem.getCreationTime(),
                                     oldItem.getExpiryTime(),
                                     false);
  if (!newItemHdl) {
    return {};
  }

  XDCHECK_EQ(newItemHdl->getSize(), oldItem.getSize());
  // same (pool, class) MM container; a sharded one may differ in shard
  XDCHECK_EQ(reinterpret_cast<uintptr_t>(
                 &getMMContainer(allocInfo.poolId, allocInfo.classId)),
             reinterpret_cast<uintptr_t>(&getMMContainer(
                 allocator_->getAllocInfo(newItemHdl->getMemory()).poolId,
                 allocator_->getAllocInfo(newItemHdl->getMemory()).classId)));

  return newItemHdl;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::evictForSlabRelease(Item& item) {
  stats_.numEvictionAttempts.inc();

  typename NvmCacheT::PutToken token;
  bool isChainedItem = item.isChainedItem();
  Item* evicted =
      isChainedItem ? &item.asChainedItem().getParentItem(compressor_) : &item;

  XDCHECK(evicted->isMoving());
  token = createPutToken(*evicted);
  auto ret = evicted->markForEvictionWhenMoving();
  XDCHECK(ret);
  XDCHECK(!item.isMoving());
  unlinkItemForEviction(*evicted);
  // wake up any readers that wait for the move to complete
  // it's safe to do now, as we have the item marked exclusive and
  // no other reader can be added to the waiters list
  wakeUpWaiters(evicted->getKey(), {});

  if (token.isValid() && shouldWriteToNvmCacheExclusive(*evicted)) {
    nvmCache_->put(*evicted, std::move(token));
  } else if (shouldUpdateAccessTimeMap(*evicted)) {
    HashedKey hk{evicted->getKey()};
    nvmCache_->updateAccessTime(hk, evicted->getLastAccessTime());
  }

  const auto allocInfo =
      allocator_->getAllocInfo(static_cast<const void*>(&item));
  if (evicted->hasChainedItem()) {
    (*stats_.chainedItemEvictions)[allocInfo.poolId][allocInfo.classId].inc();
  } else {
    (*stats_.regularItemEvictions)[allocInfo.poolId][allocInfo.classId].inc();
  }

  stats_.numEvictionSuccesses.inc();

  XDCHECK(evicted->getRefCount() == 0);
  const auto res =
      releaseBackToAllocator(*evicted, RemoveContext::kEviction, false);
  XDCHECK(res == ReleaseRes::kReleased);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::removeIfExpired(const ReadHandle& handle) {
  if (!handle) {
    return false;
  }

  // We remove the item from both access and mm containers.
  // We want to make sure the caller is the only one holding the handle.
  auto removedHandle =
      accessContainer_->removeIf(*(handle.getInternal()), itemExpiryPredicate,
                                 RemoveContext::kExpired, config_.preRemoveCb);
  if (removedHandle) {
    removeFromMMContainer(*(handle.getInternal()));
    return true;
  }

  return false;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::markMovingForSlabRelease(
    const SlabReleaseContext& ctx, void* alloc, util::Throttler& throttler) {
  // MemoryAllocator::processAllocForRelease will execute the callback
  // if the item is not already free. So there are three outcomes here:
  //  1. Item not freed yet and marked as moving
  //  2. Item not freed yet but could not be marked as moving
  //  3. Item freed already
  //
  // For 1), return true
  // For 2), retry
  // For 3), return false to abort since no action is required

  // At first, we assume this item was already freed
  bool itemFreed = true;
  bool markedMoving = false;
  const auto fn = [this, &markedMoving, &itemFreed](void* memory) {
    // Since this callback is executed, the item is not yet freed
    itemFreed = false;
    Item* item = static_cast<Item*>(memory);
    auto& mmContainer = getMMContainer(*item);
    mmContainer.withContainerLock([this, &mmContainer, &item, &markedMoving]() {
      // we rely on the mmContainer lock to safely check that the item is
      // currently in the mmContainer (no other threads are currently
      // allocating this item). This is needed to sync on the case where a
      // chained item is being released back to allocator and it's parent
      // ref could be invalid. We need a valid parent ref in order to mark a
      // chained item as moving since we sync on the parent by marking it as
      // moving.
      if (!item->isInMMContainer()) {
        return;
      }

      XDCHECK_EQ(&getMMContainer(*item), &mmContainer);
      if (!item->isChainedItem()) {
        if (item->markMoving()) {
          markedMoving = true;
        }
        return;
      }

      // For chained items we mark moving on its parent item.
      Item* parentItem = &item->asChainedItem().getParentItem(compressor_);
      auto l = chainedItemLocks_.tryLockExclusive(parentItem->getKey());
      if (!l ||
          parentItem != &item->asChainedItem().getParentItem(compressor_)) {
        // Fail moving if we either couldn't acquire the chained item lock,
        // or if the parent had already been replaced in the meanwhile.
        return;
      }
      if (parentItem->markMoving()) {
        markedMoving = true;
      }
    });
  };

  // Snapshot the item string under processAllocForRelease() protection.
  // Reading alloc directly here races with concurrent free(), while doing it
  // after abortSlabRelease() can race with the slab being advised away.
  auto captureItemStringForAbort = [&]() {
    std::string itemStr = "item <already freed before abort>";
    try {
      allocator_->processAllocForRelease(ctx, alloc, [&](void* memory) {
        itemStr = static_cast<Item*>(memory)->toString();
      });
    } catch (const std::exception& e) {
      itemStr = fmt::format("<failed to capture item for abort: {}>", e.what());
    }
    return itemStr;
  };

  auto abortWithMessage = [&](const std::string& reason) {
    auto itemStr = captureItemStringForAbort();
    allocator_->abortSlabRelease(ctx);
    throw exception::SlabReleaseAborted(
        fmt::format("Slab Release aborted {} while still trying to mark"
                    " as moving for Item: {}. Pool: {}, Class: {}.",
                    reason, itemStr, ctx.getPoolId(), ctx.getClassId()));
  };

  auto startTime = util::getCurrentTimeMs();
  while (true) {
    allocator_->processAllocForRelease(ctx, alloc, fn);

    // If item is already freed we give up trying to mark the item moving
    // and return false, otherwise if marked as moving, we return true.
    if (itemFreed) {
      return false;
    } else if (markedMoving) {
      return true;
    }

    // Reset this to true, since we always assume an item is freed
    // when checking with the AllocationClass
    itemFreed = true;

    if (isFastShutdownTriggered()) {
      abortWithMessage("due to shutdown");
    }

    if (config_.slabRebalanceTimeout.count() > 0) {
      auto elapsedTime = util::getCurrentTimeMs() - startTime;
      if (elapsedTime >
          static_cast<uint64_t>(config_.slabRebalanceTimeout.count())) {
        abortWithMessage(fmt::format("after {} ms", elapsedTime));
      }
    }

    stats_.numMoveAttempts.inc();
    throttleWith(throttler, [&] {
      XLOGF(WARN,
            "Spent {} seconds, slab release still trying to mark as moving for "
            "Item: {}. Pool: {}, Class: {}.",
            (util::getCurrentTimeMs() - startTime) / 1000,
            static_cast<Item*>(alloc)->toString(), ctx.getPoolId(),
            ctx.getClassId());
    });
  }
}

template <typename CacheTrait>
template <typename CCacheT, typename... Args>
CCacheT* CacheAllocator<CacheTrait>::addCompactCache(folly::StringPiece name,
                                                     size_t size,
                                                     Args&&... args) {
  if (!config_.isCompactCacheEnabled()) {
    throw std::logic_error("Compact cache is not enabled");
  }

  std::unique_lock lock(compactCachePoolsLock_);
  auto poolId = allocator_->addPool(name, size, {Slab::kSize});
  isCompactCachePool_[poolId] = true;

  auto ptr = std::make_unique<CCacheT>(
      compactCacheManager_->addAllocator(name.str(), poolId),
      std::forward<Args>(args)...);
  auto it = compactCaches_.emplace(poolId, std::move(ptr));
  XDCHECK(it.second);
  return static_cast<CCacheT*>(it.first->second.get());
}

template <typename CacheTrait>
template <typename CCacheT, typename... Args>
CCacheT* CacheAllocator<CacheTrait>::attachCompactCache(folly::StringPiece name,
                                                        Args&&... args) {
  auto& allocator = compactCacheManager_->getAllocator(name.str());
  auto poolId = allocator.getPoolId();
  // if a compact cache with this name already exists, return without creating
  // new instance
  std::unique_lock lock(compactCachePoolsLock_);
  if (compactCaches_.contains(poolId)) {
    return static_cast<CCacheT*>(compactCaches_[poolId].get());
  }

  auto ptr = std::make_unique<CCacheT>(allocator, std::forward<Args>(args)...);
  auto it = compactCaches_.emplace(poolId, std::move(ptr));
  XDCHECK(it.second);
  return static_cast<CCacheT*>(it.first->second.get());
}

template <typename CacheTrait>
const ICompactCache& CacheAllocator<CacheTrait>::getCompactCache(
    PoolId pid) const {
  std::shared_lock lock(compactCachePoolsLock_);
  if (!isCompactCachePool_[pid]) {
    throw std::invalid_argument(
        fmt::format("PoolId {} is not a compact cache", pid));
  }

  auto it = compactCaches_.find(pid);
  if (it == compactCaches_.end()) {
    throw std::invalid_argument(
        fmt::format("PoolId {} belongs to an un-attached compact cache", pid));
  }
  return *it->second;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::setPoolOptimizerFor(PoolId poolId,
                                                     bool enableAutoResizing) {
  optimizerEnabled_[poolId] = enableAutoResizing;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::resizeCompactCaches() {
  compactCacheManager_->resizeAll();
}

template <typename CacheTrait>
typename CacheTrait::MMType::LruType CacheAllocator<CacheTrait>::getItemLruType(
    const Item& item) const {
  return getMMContainer(item).getLruType(item);
}

// The order of the serialization is as follows:
//
// This is also the order of deserialization in the constructor, when
// we restore the cache allocator.
//
// ---------------------------------
// | accessContainer_              |
// | mmContainers_                 |
// | compactCacheManager_          |
// | allocator_                    |
// | metadata_                     |
// ---------------------------------
template <typename CacheTrait>
folly::IOBufQueue CacheAllocator<CacheTrait>::saveStateToIOBuf() {
  if (stats_.numActiveSlabReleases.get() != 0) {
    throw std::logic_error(
        "There are still slabs being released at the moment");
  }

  *metadata_.allocatorVersion() = kCachelibVersion;
  *metadata_.ramFormatVersion() = kCacheRamFormatVersion;
  *metadata_.cacheCreationTime() = static_cast<int64_t>(cacheCreationTime_);
  *metadata_.mmType() = isMMContainerSharded()
                            ? MMType::kId | (int64_t{config_.mmContainerShards}
                                             << kMMTypeIdShardShift)
                            : MMType::kId;
  *metadata_.accessType() = AccessType::kId;
  if (config_.slabHugePageSize.isHugePage()) {
    *metadata_.slabPageSize() =
        static_cast<int64_t>(config_.slabHugePageSize.getPageSize());
  }
  if (config_.accessHugePageSize.isHugePage()) {
    *metadata_.accessPageSize() =
        static_cast<int64_t>(config_.accessHugePageSize.getPageSize());
  }

  metadata_.compactCachePools()->clear();
  const auto pools = getPoolIds();
  {
    std::shared_lock lock(compactCachePoolsLock_);
    for (PoolId pid : pools) {
      for (unsigned int cid = 0; cid < (*stats_.fragmentationSize)[pid].size();
           ++cid) {
        metadata_.fragmentationSize()[pid][static_cast<ClassId>(cid)] =
            (*stats_.fragmentationSize)[pid][cid].get();
      }
      if (isCompactCachePool_[pid]) {
        metadata_.compactCachePools()->push_back(pid);
      }
    }
  }

  *metadata_.numChainedParentItems() = stats_.numChainedParentItems.get();
  *metadata_.numChainedChildItems() = stats_.numChainedChildItems.get();
  *metadata_.numAbortedSlabReleases() = stats_.numAbortedSlabReleases.get();

  MMSerializationTypeContainer mmContainersState;
  for (unsigned int i = 0; i < mmContainers_.size(); ++i) {
    for (unsigned int j = 0; j < mmContainers_[i].size(); ++j) {
      if (mmContainers_[i][j]) {
        mmContainersState.pools_ref()[i][j] = mmContainers_[i][j]->saveState();
        if (!isMMContainerSharded()) {
          continue;
        }
        // shard s of class j is keyed j + s * kMaxClasses
        const auto& extraShards = (*mmContainerShards_)[i][j].extraShards;
        for (unsigned int s = 1; s <= extraShards.size(); ++s) {
          mmContainersState
              .pools_ref()[i][j + s * MemoryAllocator::kMaxClasses] =
              extraShards[s - 1]->saveState();
        }
      }
    }
  }
  AccessSerializationType accessContainerState = accessContainer_->saveState();
  MemoryAllocator::SerializationType allocatorState = allocator_->saveState();
  CCacheManager::SerializationType ccState = compactCacheManager_->saveState();
  AccessSerializationType chainedItemAccessContainerState =
      chainedItemAccessContainer_->saveState();

  // serialize to an iobuf queue. The caller can then copy over the serialized
  // results into a single buffer.
  folly::IOBufQueue queue;
  Serializer::serializeToIOBufQueue(queue, metadata_);
  Serializer::serializeToIOBufQueue(queue, allocatorState);
  Serializer::serializeToIOBufQueue(queue, ccState);
  Serializer::serializeToIOBufQueue(queue, mmContainersState);
  Serializer::serializeToIOBufQueue(queue, accessContainerState);
  Serializer::serializeToIOBufQueue(queue, chainedItemAccessContainerState);
  return queue;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopWorkers(std::chrono::seconds timeout) {
  bool success = true;
  success &= stopPoolOptimizer(timeout);
  success &= stopPoolRebalancer(timeout);
  success &= stopPoolResizer(timeout);
  success &= stopMemMonitor(timeout);
  success &= stopReaper(timeout);
  success &= stopBackgroundEvictor(timeout);
  success &= stopBackgroundPromoter(timeout);
  return success;
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::ShutDownStatus
CacheAllocator<CacheTrait>::shutDown() {
  using ShmShutDownRes = typename ShmManager::ShutDownRes;
  XLOG(DBG, "shutting down CacheAllocator");
  if (shmManager_ == nullptr) {
    throw std::invalid_argument(
        "shutDown can only be called once from a cached manager created on "
        "shared memory. You may also be incorrectly constructing your "
        "allocator. Are you passing in "
        "AllocatorType::SharedMem* ?");
  }
  XDCHECK(!config_.cacheDir.empty());

  if (config_.enableFastShutdown) {
    triggerFastShutdown_ = true;
  }

  if (!stopWorkers()) {
    XLOG(ERR) << "Failed to stop workers during shutdown, aborting";
    return ShutDownStatus::kFailed;
  }

  const auto handleCount = getNumActiveHandles();
  if (handleCount != 0) {
    XLOGF(ERR, "Found {} active handles while shutting down cache. aborting",
          handleCount);
    return ShutDownStatus::kFailed;
  }

  // The rest of the procedure is one-shot, the first caller owns the
  // destructive teardown below.
  if (shutDownStarted_.exchange(true)) {
    XLOG(WARN) << "Duplicate shutDown() call ignored";
    return ShutDownStatus::kSkipped;
  }

  const auto nvmShutDownStatusOpt = saveNvmCache();
  saveRamCache();
  const auto shmShutDownStatus = shmManager_->shutDown();
  const auto shmShutDownSucceeded =
      (shmShutDownStatus == ShmShutDownRes::kSuccess);
  shmManager_.reset();

  if (shmShutDownSucceeded) {
    if (!nvmShutDownStatusOpt || *nvmShutDownStatusOpt) {
      return ShutDownStatus::kSuccess;
    }

    if (nvmShutDownStatusOpt && !*nvmShutDownStatusOpt) {
      return ShutDownStatus::kSavedOnlyDRAM;
    }
  }

  XLOGF(ERR, "Could not shutdown DRAM cache cleanly. ShutDownRes={}",
        (shmShutDownStatus == ShmShutDownRes::kFailedWrite ? "kFailedWrite"
                                                           : "kFileDeleted"));

  if (nvmShutDownStatusOpt && *nvmShutDownStatusOpt) {
    return ShutDownStatus::kSavedOnlyNvmCache;
  }

  return ShutDownStatus::kFailed;
}

template <typename CacheTrait>
std::optional<bool> CacheAllocator<CacheTrait>::saveNvmCache() {
  if (!nvmCache_) {
    return std::nullopt;
  }

  // throw any exceptions from shutting down nvmcache since we dont know the
  // state of RAM as well.
  if (!nvmCache_->isEnabled()) {
    nvmCache_->shutDown();
    return std::nullopt;
  }

  if (!nvmCache_->shutDown()) {
    XLOG(ERR, "Could not shutdown nvmcache cleanly");
    return false;
  }

  nvmCacheState_.markSafeShutDown(nvmCache_->getLastPersistTimeMs());
  return true;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::saveRamCache() {
  // serialize the cache state
  auto serializedBuf = saveStateToIOBuf();
  std::unique_ptr<folly::IOBuf> ioBuf = serializedBuf.move();
  ioBuf->coalesce();

  void* infoAddr =
      shmManager_->createShm(detail::kShmInfoName, ioBuf->length()).addr;
  Serializer serializer(reinterpret_cast<uint8_t*>(infoAddr),
                        reinterpret_cast<uint8_t*>(infoAddr) + ioBuf->length());
  serializer.writeToBuffer(std::move(ioBuf));
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::validateMMContainerShards(
    const MMContainers& mmContainers) const {
  for (unsigned int i = 0; i < mmContainers.size(); ++i) {
    for (unsigned int j = 0; j < mmContainers[i].size(); ++j) {
      const auto& shardSet = (*mmContainerShards_)[i][j];
      const bool complete =
          shardSet.tailTimes != nullptr && mmContainers[i][j] != nullptr &&
          std::all_of(shardSet.extraShards.begin(),
                      shardSet.extraShards.end(),
                      [](const auto& shard) { return shard != nullptr; });
      if (!complete && (shardSet.tailTimes || mmContainers[i][j])) {
        throw std::invalid_argument(fmt::format(
            "Persisted MM container shards of pool {} class {} are incomplete",
            i, j));
      }
    }
  }
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::MMContainers
CacheAllocator<CacheTrait>::deserializeMMContainers(
    Deserializer& deserializer,
    const typename Item::PtrCompressor& compressor) {
  const auto container =
      deserializer.deserialize<MMSerializationTypeContainer>();

  MMContainers mmContainers;

  for (auto& kvPool : *container.pools_ref()) {
    auto i = static_cast<PoolId>(kvPool.first);
    auto& pool = getPool(i);
    for (auto& kv : kvPool.second) {
      // see saveStateToIOBuf() for how shards are keyed
      const auto shard =
          static_cast<uint32_t>(kv.first) / MemoryAllocator::kMaxClasses;
      auto j = static_cast<ClassId>(static_cast<uint32_t>(kv.first) %
                                    MemoryAllocator::kMaxClasses);
      if (shard >= config_.mmContainerShards) {
        throw std::invalid_argument(fmt::format(
            "Persisted MM container shard {} of pool {} class {} exceeds the "
            "configured {} shards",
            shard, i, j, config_.mmContainerShards));
      }
      MMContainerPtr ptr =
          std::make_unique<typename MMContainerPtr::element_type>(kv.second,
                                                                  compressor);
      auto config = ptr->getConfig();
      config.addExtraConfig(getMMContainerTailSize(pool, j, shard));
      ptr->setConfig(config);
      if (shard == 0) {
        mmContainers[i][j] = std::move(ptr);
        continue;
      }
      if (!(*mmContainerShards_)[i][j].tailTimes) {
        initMMContainerShardSet(i, j);
      }
      (*mmContainerShards_)[i][j].extraShards[shard - 1] = std::move(ptr);
    }
  }
  if (isMMContainerSharded()) {
    validateMMContainerShards(mmContainers);
  }
  // We need to drop the unevictableMMContainer in the desierializer.
  // TODO: remove this at version 17.
  if (metadata_.allocatorVersion() <= 15) {
    deserializer.deserialize<MMSerializationTypeContainer>();
  }
  return mmContainers;
}

template <typename CacheTrait>
serialization::CacheAllocatorMetadata
CacheAllocator<CacheTrait>::deserializeCacheAllocatorMetadata(
    Deserializer& deserializer) {
  auto meta = deserializer.deserialize<serialization::CacheAllocatorMetadata>();

  if (*meta.ramFormatVersion() != kCacheRamFormatVersion) {
    throw std::runtime_error(
        fmt::format("Expected cache ram format version {}. But found {}.",
                    kCacheRamFormatVersion, *meta.ramFormatVersion()));
  }

  if (*meta.accessType() != AccessType::kId) {
    throw std::invalid_argument(
        fmt::format("Expected {}, got {} for AccessType", *meta.accessType(),
                    AccessType::kId));
  }

  const auto persistedMMType =
      *meta.mmType() & ((int64_t{1} << kMMTypeIdShardShift) - 1);
  if (persistedMMType != MMType::kId) {
    throw std::invalid_argument(fmt::format("Expected {}, got {} for MMType",
                                            *meta.mmType(), MMType::kId));
  }

  const auto persistedShards =
      std::max<int64_t>(1, *meta.mmType() >> kMMTypeIdShardShift);
  if (persistedShards != config_.mmContainerShards) {
    throw std::invalid_argument(fmt::format(
        "MM container shards changed across restarts: persisted {}, "
        "configured {}",
        persistedShards, config_.mmContainerShards));
  }

  const size_t persistedSlabPageSize =
      PageSize(static_cast<size_t>(*meta.slabPageSize())).getPageSize();
  const size_t persistedAccessPageSize =
      PageSize(static_cast<size_t>(*meta.accessPageSize())).getPageSize();
  if (persistedSlabPageSize != config_.slabHugePageSize.getPageSize() ||
      persistedAccessPageSize != config_.accessHugePageSize.getPageSize()) {
    throw std::invalid_argument(fmt::format(
        "Huge page config changed across restarts: persisted slab/access page "
        "sizes {}/{}, configured {}/{}",
        persistedSlabPageSize, persistedAccessPageSize,
        config_.slabHugePageSize.getPageSize(),
        config_.accessHugePageSize.getPageSize()));
  }
  return meta;
}

template <typename CacheTrait>
int64_t CacheAllocator<CacheTrait>::getNumActiveHandles() const {
  return handleCount_.getSnapshot();
}

template <typename CacheTrait>
int64_t CacheAllocator<CacheTrait>::getHandleCountForThread() const {
  return handleCount_.tlStats();
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::resetHandleCountForThread_private() {
  handleCount_.tlStats() = 0;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::adjustHandleCountForThread_private(
    int64_t delta) {
  handleCount_.tlStats() += delta;
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::initStats() {
  stats_.init();

  // deserialize the fragmentation size of each thread.
  for (const auto& pid : *metadata_.fragmentationSize()) {
    for (const auto& cid : pid.second) {
      (*stats_.fragmentationSize)[pid.first][cid.first].set(
          static_cast<uint64_t>(cid.second));
    }
  }

  // deserialize item counter stats
  stats_.numChainedParentItems.set(*metadata_.numChainedParentItems());
  stats_.numChainedChildItems.set(*metadata_.numChainedChildItems());
  stats_.numAbortedSlabReleases.set(
      static_cast<uint64_t>(*metadata_.numAbortedSlabReleases()));
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::forEachChainedItem(
    const Item& parent, std::function<void(ChainedItem&)> func) {
  auto l = chainedItemLocks_.lockShared(parent.getKey());

  auto headHandle = findChainedItem(parent);
  if (!headHandle) {
    return;
  }

  ChainedItem* head = &headHandle.get()->asChainedItem();
  while (head) {
    func(*head);
    head = head->getNext(compressor_);
  }
}

template <typename CacheTrait>
typename CacheAllocator<CacheTrait>::WriteHandle
CacheAllocator<CacheTrait>::findChainedItem(const Item& parent) const {
  const auto cPtr = compressor_.compress(&parent);
  return chainedItemAccessContainer_->find(
      Key{reinterpret_cast<const char*>(&cPtr), ChainedItem::kKeySize});
}

template <typename CacheTrait>
template <typename Handle, typename Iter>
CacheChainedAllocs<CacheAllocator<CacheTrait>, Handle, Iter>
CacheAllocator<CacheTrait>::viewAsChainedAllocsT(const Handle& parent) {
  XDCHECK(parent);
  auto handle = parent.clone();
  if (!handle) {
    throw std::invalid_argument("Failed to clone item handle");
  }

  if (!handle->hasChainedItem()) {
    throw std::invalid_argument(
        fmt::format("Failed to materialize chain. Parent does not have "
                    "chained items. Parent: {}",
                    parent->toString()));
  }

  auto l = chainedItemLocks_.lockShared(handle->getKey());
  auto head = findChainedItem(*handle);
  return CacheChainedAllocs<CacheAllocator<CacheTrait>, Handle, Iter>{
      std::move(l), std::move(handle), *head, compressor_};
}

template <typename CacheTrait>
GlobalCacheStats CacheAllocator<CacheTrait>::getGlobalCacheStats() const {
  stats().numExpensiveStatsPolled.inc();

  GlobalCacheStats ret{};
  stats_.populateGlobalCacheStats(ret);

  // Sum only existing pools and classes: reading a TLCounter allocates a
  // thread-local id, so walking all kMaxPools x kMaxClasses would bloat every
  // thread's id table.
  for (const auto pid : allocator_->getPoolIds()) {
    const auto& pool = allocator_->getPool(pid);
    for (unsigned int cid = 0; cid < pool.getNumClassId(); ++cid) {
      ret.allocAttempts += (*stats_.allocAttempts)[pid][cid].get();
      ret.evictionAttempts += (*stats_.evictionAttempts)[pid][cid].get();
      ret.allocFailures += (*stats_.allocFailures)[pid][cid].get();
      ret.numEvictions += (*stats_.chainedItemEvictions)[pid][cid].get() +
                          (*stats_.regularItemEvictions)[pid][cid].get();
    }
  }

  ret.numItems = accessContainer_->getStats().numKeys;

  const uint64_t currTime = util::getCurrentTimeSec();
  ret.cacheInstanceUpTime = currTime - cacheInstanceCreationTime_;
  ret.ramUpTime = currTime - cacheCreationTime_;
  ret.nvmUpTime = currTime - nvmCacheState_.getCreationTime();
  ret.nvmCacheEnabled = nvmCache_ ? nvmCache_->isEnabled() : false;
  ret.reaperStats = getReaperStats();
  ret.rebalancerStats = getRebalancerStats();
  ret.evictionStats = getBackgroundMoverStats(MoverDir::Evict);
  ret.promotionStats = getBackgroundMoverStats(MoverDir::Promote);
  ret.numActiveHandles = getNumActiveHandles();

  ret.isNewRamCache = cacheCreationTime_ == cacheInstanceCreationTime_;
  // NVM cache is new either if newly created or started fresh with truncate
  ret.isNewNvmCache =
      (nvmCacheState_.getCreationTime() == cacheInstanceCreationTime_) ||
      nvmCacheState_.shouldStartFresh();

  return ret;
}

template <typename CacheTrait>
CacheMemoryStats CacheAllocator<CacheTrait>::getCacheMemoryStats() const {
  const auto totalCacheSize = allocator_->getMemorySize();
  const auto configuredTotalCacheSize = allocator_->getMemorySizeInclAdvised();

  auto addSize = [this](size_t a, PoolId pid) {
    return a + allocator_->getPool(pid).getPoolSize();
  };
  const auto regularPoolIds = getRegularPoolIds();
  const auto ccCachePoolIds = getCCachePoolIds();
  size_t configuredRegularCacheSize = std::accumulate(
      regularPoolIds.begin(), regularPoolIds.end(), 0ULL, addSize);
  size_t configuredCompactCacheSize = std::accumulate(
      ccCachePoolIds.begin(), ccCachePoolIds.end(), 0ULL, addSize);

  return CacheMemoryStats{
      totalCacheSize,
      configuredTotalCacheSize,
      configuredRegularCacheSize,
      configuredCompactCacheSize,
      allocator_->getAdvisedMemorySize(),
      memMonitorMaxAdvisePct_.load(std::memory_order_relaxed),
      allocator_->getUnreservedMemorySize(),
      nvmCache_ ? nvmCache_->getSize() : 0,
      util::getMemAvailable(),
      util::getRSSBytes(),
      accessContainer_->getMemorySize(),
      chainedItemAccessContainer_->getMemorySize()};
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::autoResizeEnabledForPool(PoolId pid) const {
  std::shared_lock lock(compactCachePoolsLock_);
  if (isCompactCachePool_[pid]) {
    // compact caches need to be registered to enable auto resizing
    return optimizerEnabled_[pid];
  } else {
    // by default all regular pools participate in auto resizing
    return true;
  }
}

template <typename CacheTrait>
void CacheAllocator<CacheTrait>::startCacheWorkers() {
  initWorkers();
}

template <typename CacheTrait>
template <typename T>
bool CacheAllocator<CacheTrait>::stopWorker(folly::StringPiece name,
                                            std::unique_ptr<T>& worker,
                                            std::chrono::seconds timeout) {
  std::lock_guard<std::mutex> l(workersMutex_);
  auto ret = util::stopPeriodicWorker(name, worker, timeout);
  worker.reset();
  return ret;
}

template <typename CacheTrait>
template <typename T, typename... Args>
bool CacheAllocator<CacheTrait>::startNewWorker(
    folly::StringPiece name,
    std::unique_ptr<T>& worker,
    std::chrono::milliseconds interval,
    Args&&... args) {
  if (worker && !stopWorker(name, worker)) {
    return false;
  }

  std::lock_guard<std::mutex> l(workersMutex_);
  return util::startPeriodicWorker(name, worker, interval,
                                   std::forward<Args>(args)...);
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::startNewPoolRebalancer(
    std::chrono::milliseconds interval,
    std::shared_ptr<RebalanceStrategy> strategy,
    unsigned int freeAllocThreshold) {
  if (!startNewWorker("PoolRebalancer", poolRebalancer_, interval, *this,
                      strategy, freeAllocThreshold)) {
    return false;
  }

  config_.poolRebalanceInterval = interval;
  config_.defaultPoolRebalanceStrategy = strategy;
  config_.poolRebalancerFreeAllocThreshold = freeAllocThreshold;

  return true;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::startNewPoolResizer(
    std::chrono::milliseconds interval,
    unsigned int poolResizeSlabsPerIter,
    std::shared_ptr<RebalanceStrategy> strategy) {
  if (!startNewWorker("PoolResizer", poolResizer_, interval, *this,
                      poolResizeSlabsPerIter, strategy)) {
    return false;
  }

  config_.poolResizeInterval = interval;
  config_.poolResizeSlabsPerIter = poolResizeSlabsPerIter;
  config_.poolResizeStrategy = strategy;
  return true;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::startNewPoolOptimizer(
    std::chrono::seconds regularInterval,
    std::chrono::seconds ccacheInterval,
    std::shared_ptr<PoolOptimizeStrategy> strategy,
    unsigned int ccacheStepSizePercent) {
  // For now we are asking the worker to wake up every second to see whether
  // it should do actual size optimization. Probably need to move to using
  // the same interval for both, with confirmation of further experiments.
  const auto workerInterval = std::chrono::seconds(1);
  if (!startNewWorker("PoolOptimizer", poolOptimizer_, workerInterval, *this,
                      strategy, regularInterval.count(), ccacheInterval.count(),
                      ccacheStepSizePercent)) {
    return false;
  }

  config_.regularPoolOptimizeInterval = regularInterval;
  config_.compactCacheOptimizeInterval = ccacheInterval;
  config_.poolOptimizeStrategy = strategy;
  config_.ccacheOptimizeStepSizePercent = ccacheStepSizePercent;

  return true;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::startNewMemMonitor(
    std::chrono::milliseconds interval,
    MemoryMonitor::Config config,
    std::shared_ptr<RebalanceStrategy> strategy) {
  if (!isOnShm_) {
    throw std::invalid_argument(
        "Memory monitoring is not supported for cache on heap. It is "
        "supported for cache on a shared memory segment only.");
  }
  config_.validateMemMonitorAndHugePages();
  MemoryMonitor::validatePoolIds(*this, config.poolIds);

  // Stop a running monitor before counting the advised slabs: an iteration
  // still in progress could otherwise advise more after they were counted.
  if (memMonitor_ && !stopMemMonitor()) {
    return false;
  }

  reclaimUnmonitoredSlabs(config, interval);

  size_t newMaxAdvisePct = 0;
  if (!config.disabled() && interval.count() > 0) {
    // On a warm restart, resume from the slabs already advised away from the
    // pools this monitor manages.
    const size_t numSlabsAdvised =
        config.poolIds.empty() ? allocator_->getNumSlabsAdvised()
                               : allocator_->getNumSlabsAdvised(config.poolIds);
    if (!startNewWorker("MemoryMonitor", memMonitor_, interval, *this, config,
                        strategy, numSlabsAdvised)) {
      return false;
    }
    newMaxAdvisePct = memMonitor_->getMaxAdvisePct();
  }
  memMonitorMaxAdvisePct_.store(newMaxAdvisePct, std::memory_order_relaxed);
  std::lock_guard<std::mutex> l(workersMutex_);
  config_.memMonitorInterval = interval;
  config_.memMonitorConfig = std::move(config);
  config_.poolAdviseStrategy = strategy;
  return true;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::startNewReaper(
    std::chrono::milliseconds interval,
    util::Throttler::Config reaperThrottleConfig) {
  if (!startNewWorker("Reaper", reaper_, interval, *this,
                      reaperThrottleConfig)) {
    return false;
  }

  config_.reaperInterval = interval;
  config_.reaperConfig = reaperThrottleConfig;
  return true;
}

template <typename CacheTrait>
auto CacheAllocator<CacheTrait>::createBgWorkerMemoryAssignments(
    size_t numWorkers) {
  std::vector<std::vector<MemoryDescriptorType>> asssignedMemory(numWorkers);
  auto pools = filterCompactCachePools(allocator_->getPoolIds());
  for (const auto pid : pools) {
    const auto& mpStats = getPool(pid).getStats();
    for (const auto cid : mpStats.classIds) {
      asssignedMemory[BackgroundMover<CacheT>::workerId(pid, cid, numWorkers)]
          .emplace_back(pid, cid);
    }
  }
  return asssignedMemory;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::startNewBackgroundEvictor(
    std::chrono::milliseconds interval,
    std::shared_ptr<BackgroundMoverStrategy> strategy,
    size_t threads) {
  XDCHECK(threads > 0);
  backgroundEvictor_.resize(threads);
  bool result = true;

  auto memoryAssignments = createBgWorkerMemoryAssignments(threads);
  for (size_t i = 0; i < threads; i++) {
    auto ret = startNewWorker("BackgroundEvictor" + std::to_string(i),
                              backgroundEvictor_[i], interval, *this, strategy,
                              MoverDir::Evict);
    result = result && ret;

    if (result) {
      backgroundEvictor_[i]->setAssignedMemory(std::move(memoryAssignments[i]));
    }
  }
  return result;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::startNewBackgroundPromoter(
    std::chrono::milliseconds interval,
    std::shared_ptr<BackgroundMoverStrategy> strategy,
    size_t threads) {
  XDCHECK(threads > 0);
  backgroundPromoter_.resize(threads);
  bool result = true;

  auto memoryAssignments = createBgWorkerMemoryAssignments(threads);
  for (size_t i = 0; i < threads; i++) {
    auto ret = startNewWorker("BackgroundPromoter" + std::to_string(i),
                              backgroundPromoter_[i], interval, *this, strategy,
                              MoverDir::Promote);
    result = result && ret;

    if (result) {
      backgroundPromoter_[i]->setAssignedMemory(
          std::move(memoryAssignments[i]));
    }
  }
  return result;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopPoolRebalancer(
    std::chrono::seconds timeout) {
  auto res = stopWorker("PoolRebalancer", poolRebalancer_, timeout);
  if (res) {
    config_.poolRebalanceInterval = std::chrono::seconds{0};
  }
  return res;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopPoolResizer(std::chrono::seconds timeout) {
  auto res = stopWorker("PoolResizer", poolResizer_, timeout);
  if (res) {
    config_.poolResizeInterval = std::chrono::seconds{0};
  }
  return res;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopPoolOptimizer(
    std::chrono::seconds timeout) {
  auto res = stopWorker("PoolOptimizer", poolOptimizer_, timeout);
  if (res) {
    config_.regularPoolOptimizeInterval = std::chrono::seconds{0};
    config_.compactCacheOptimizeInterval = std::chrono::seconds{0};
  }
  return res;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopMemMonitor(std::chrono::seconds timeout) {
  auto res = stopWorker("MemoryMonitor", memMonitor_, timeout);
  memMonitorMaxAdvisePct_.store(0, std::memory_order_relaxed);
  if (res) {
    std::lock_guard<std::mutex> l(workersMutex_);
    config_.memMonitorInterval = std::chrono::seconds{0};
  }
  return res;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopReaper(std::chrono::seconds timeout) {
  auto res = stopWorker("Reaper", reaper_, timeout);
  if (res) {
    config_.reaperInterval = std::chrono::seconds{0};
  }
  return res;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopBackgroundEvictor(
    std::chrono::seconds timeout) {
  bool result = true;
  for (size_t i = 0; i < backgroundEvictor_.size(); i++) {
    auto ret = stopWorker("BackgroundEvictor", backgroundEvictor_[i], timeout);
    result = result && ret;
  }
  return result;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::stopBackgroundPromoter(
    std::chrono::seconds timeout) {
  bool result = true;
  for (size_t i = 0; i < backgroundPromoter_.size(); i++) {
    auto ret =
        stopWorker("BackgroundPromoter", backgroundPromoter_[i], timeout);
    result = result && ret;
  }
  return result;
}

template <typename CacheTrait>
bool CacheAllocator<CacheTrait>::cleanupStrayShmSegments(
    const std::string& cacheDir,
    bool posix,
    const std::string& hugePageMountDir,
    const std::string& posixShmDir) {
  if (util::getStatIfExists(cacheDir, nullptr) && util::isDir(cacheDir)) {
    try {
      // cache dir exists. clean up only if there are no other processes
      // attached. if another process was attached, the following would fail.
      ShmManager::cleanup(cacheDir, posix, hugePageMountDir, posixShmDir);
    } catch (const std::exception& e) {
      XLOGF(ERR, "Error cleaning up {}. Exception: ", cacheDir, e.what());
      return false;
    }
  } else {
    // cache dir did not exist. Try to nuke the segments we know by name.
    // Any other concurrent process can not be attached to the segments or
    // even if it does, we want to mark it for destruction.
    ShmManager::removeByName(cacheDir, detail::kShmInfoName, posix,
                             hugePageMountDir, posixShmDir);
    ShmManager::removeByName(cacheDir, detail::kShmCacheName, posix,
                             hugePageMountDir, posixShmDir);
    ShmManager::removeByName(cacheDir, detail::kShmHashTableName, posix,
                             hugePageMountDir, posixShmDir);
    ShmManager::removeByName(cacheDir, detail::kShmChainedItemHashTableName,
                             posix, hugePageMountDir, posixShmDir);
  }
  return true;
}

template <typename CacheTrait>
uint64_t CacheAllocator<CacheTrait>::getItemPtrAsOffset(const void* ptr) {
  // Return unt64_t instead of uintptr_t to accommodate platforms where
  // the two differ (e.g. Mac OS 12) - causing templating instantiation
  // errors downstream.

  // if this succeeeds, the address is valid within the cache.
  allocator_->getAllocInfo(ptr);

  if (!isOnShm_ || !shmManager_) {
    throw std::invalid_argument("Shared memory not used");
  }

  const auto& shm = shmManager_->getShmByName(detail::kShmCacheName);

  return reinterpret_cast<uint64_t>(ptr) -
         reinterpret_cast<uint64_t>(shm.getCurrentMapping().addr);
}

template <typename CacheTrait>
util::StatsMap CacheAllocator<CacheTrait>::getNvmCacheStatsMap() const {
  stats().numExpensiveStatsPolled.inc();

  auto ret = nvmCache_ ? nvmCache_->getStatsMap() : util::StatsMap{};
  if (nvmAdmissionPolicy_) {
    nvmAdmissionPolicy_->getCounters(ret.createCountVisitor());
  }
  if (nvmCache_) {
    auto reasonCount = [this](StartTruncateReason reason) {
      return startTruncateReason_ == reason ? 1 : 0;
    };
    ret.insertCount("start_truncated",
                    reasonCount(StartTruncateReason::kNone) ? 0 : 1);
    ret.insertCount("start_truncated_no_cache_dir",
                    reasonCount(StartTruncateReason::kNoCacheDir));
    ret.insertCount("start_truncated_no_usable_state",
                    reasonCount(StartTruncateReason::kNoUsableState));
    ret.insertCount("start_truncated_dram_cache_new",
                    reasonCount(StartTruncateReason::kDramCacheNew));
    // The previous shutdown's value: the stats exporters are gone by the time
    // persist runs.
    ret.insertCount("navy_persist_time_ms",
                    nvmCacheState_.getLastPersistTimeMs());
  }
  return ret;
}
} // namespace facebook::cachelib

namespace facebook::cachelib {
// Declare templates ahead of use to reduce compilation time
extern template class CacheAllocator<LruCacheTrait>;
extern template class CacheAllocator<LruCacheWithSpinBucketsTrait>;
extern template class CacheAllocator<Lru2QCacheTrait>;
extern template class CacheAllocator<TinyLFUCacheTrait>;
extern template class CacheAllocator<WTinyLFUCacheTrait>;

// CacheAllocator with an LRU eviction policy
// LRU policy can be configured to act as a segmented LRU as well
using LruAllocator = CacheAllocator<LruCacheTrait>;
using Lru5BAllocator = CacheAllocator<Lru5BCacheTrait>;
using LruAllocatorSpinBuckets = CacheAllocator<LruCacheWithSpinBucketsTrait>;

// CacheAllocator with 2Q eviction policy
// Hot, Warm, Cold queues are maintained
// Item Life Time:
//  0. On access, each item is promoted to the head of its current
//  queue
//  1. first enter Hot queue
//  2. if accessed while in Hot, item will qualify entry to Warm queue
//     otherwise, item will enter cold queue
//  3. items in cold queue are evicted to make room for new items
using Lru2QAllocator = CacheAllocator<Lru2QCacheTrait>;
using Lru5B2QAllocator = CacheAllocator<Lru5B2QCacheTrait>;

// CacheAllocator with Tiny LFU eviction policy
// It has a window initially to gauge the frequency of accesses of newly
// inserted items. And eventually it will only admit items that are accessed
// beyond a threshold into the warm cache.
using TinyLFUAllocator = CacheAllocator<TinyLFUCacheTrait>;

// CacheAllocator with Tiny LFU eviction policy and protection segment. It has a
// window initially to gauge the frequency of accesses of newly inserted items.
// The Main Cache is broken down into probation segment taking ~20% queue size
// and protection segment taking ~ 80%. Popular items that exceed a defined
// protected frequency will be preserved in the protection segment. If
// protection segment is full, it will not be immediately evicted out of the
// main queue, but will be moved into the probation segment. This will prevent
// the popular items from being evicted out immediately.
using WTinyLFUAllocator = CacheAllocator<WTinyLFUCacheTrait>;
} // namespace facebook::cachelib
