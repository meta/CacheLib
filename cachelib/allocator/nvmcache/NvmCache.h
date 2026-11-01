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
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/container/small_vector.h>
#include <folly/fibers/TimedMutex.h>
#include <folly/hash/Hash.h>
#include <folly/json/dynamic.h>
#include <folly/json/json.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

#include "cachelib/allocator/nvmcache/AccessTimeMap.h"
#include "cachelib/allocator/nvmcache/CacheApiWrapper.h"
#include "cachelib/allocator/nvmcache/InFlightPuts.h"
#include "cachelib/allocator/nvmcache/NavyConfig.h"
#include "cachelib/navy/common/ChecksumOffload.h"
#include "cachelib/allocator/nvmcache/NavySetup.h"
#include "cachelib/allocator/nvmcache/NvmItem.h"
#include "cachelib/allocator/nvmcache/ReqContexts.h"
#include "cachelib/allocator/nvmcache/TombStones.h"
#include "cachelib/allocator/nvmcache/WaitContext.h"
#include "cachelib/common/AtomicCounter.h"
#include "cachelib/common/EventInterface.h"
#include "cachelib/common/Exceptions.h"
#include "cachelib/common/Hash.h"
#include "cachelib/common/PercentileStats.h"
#include "cachelib/common/Profiled.h"
#include "cachelib/common/Time.h"
#include "cachelib/common/Utils.h"
#include "cachelib/navy/common/Device.h"
#include "cachelib/navy/common/Types.h"
#include "folly/Range.h"

namespace facebook::cachelib {
namespace tests {
class NvmCacheTest;
}

using folly::fibers::TimedMutex;

// NvmCache is a key-value cache on flash. It is intended to be used
// along with a CacheAllocator to provide a uniform API to the application
// by abstracting away the details of ram and flash management.
template <typename C>
class NvmCache {
 public:
  using Item = typename C::Item;
  using ChainedItem = typename Item::ChainedItem;
  using ChainedItemIter = typename C::ChainedItemIter;
  using WritableChainedItemIter = typename C::WritableChainedItemIter;
  using ItemDestructor = typename C::ItemDestructor;
  using DestructorData = typename C::DestructorData;
  using SampleItem = typename C::SampleItem;
  using ItemDestructorMutex =
      trace::Profiled<TimedMutex, "cachelib:nvmcache:item_destructor">;

  // Context passed in encodeCb or decodeCb. If the item has children,
  // they are passed in the form of a folly::Range.
  // Chained items must be iterated though @chainedItemRange.
  // Other APIs used to access chained items are not compatible and should not
  // be used.
  struct EncodeDecodeContext {
    Item& item;
    folly::Range<ChainedItemIter> chainedItemRange;
  };

  // call back to encode an item to prepare for nvmcache if needed
  // @param it      item we are writing to nvmcache
  // @param allocs  pointer to chained allocs if the item has chained allocs
  // @return true if the item can be written. false if we need to fail
  using EncodeCB = std::function<bool(EncodeDecodeContext)>;

  // same as the above, but reverses the item back to its expected state if
  // needed.
  using DecodeCB = std::function<void(EncodeDecodeContext)>;

  // encrypt everything written to the device and decrypt on every read
  using DeviceEncryptor = navy::DeviceEncryptor;

  using WriteHandle = typename C::WriteHandle;
  using ReadHandle = typename C::ReadHandle;
  using DeleteTombStoneGuard = typename TombStones::Guard;
  using PutToken = typename InFlightPuts::PutToken;

  // Callback function to make a vector of BufferedBlob from the item being
  // evicted from DRAM. If the item does not have chained items, the vector
  // should have one single element containing the item converted to a
  // BufferedBlob that's ready to be copied to NvmItem buffer. If the item
  // contains chained item, the vector should have N+1 elements, where the
  // element 0 is the parent item, element [1, N] should be the N chained item
  // as ordered in the range. When an empty vector is returned, the insertion
  // will abort.
  using MakeBlobCB = typename std::function<std::vector<BufferedBlob>(
      const Item&, folly::Range<ChainedItemIter>)>;

  // Callback function to propagate the item when fetched from NVM.
  // The item and chained items are already allocated with the size from
  // NvmItem::Blobs::origSize.
  // Return false if there was an error and the insertion back to DRAM cache
  // should not proceed.
  using MakeObjCB = typename std::function<bool(
      const NvmItem&, Item&, folly::Range<WritableChainedItemIter>)>;

  struct Config {
    navy::NavyConfig navyConfig{};

    // (Optional) enables the user to change some bits to prepare the item for
    // nvmcache serialization like fixing up pointers etc. Both must be encode
    // and decode must be specified or NOT specified. Encode and decode happen
    // before encryption and after decryption respectively if enabled.
    EncodeCB encodeCb{};
    DecodeCB decodeCb{};

    // (Optional) Callback overriding logic between DRAM item and NVMItem. This
    // allows the user of cachelib to provide logic to:
    // 1. Create blobs to be copied into NvmItem from the Item being evicted
    // from DRAM; and
    // 2. Populate the content of Item with the Blobs retrieved from NvmItem.
    MakeBlobCB makeBlobCb{};
    MakeObjCB makeObjCb{};

    // (Optional) This enables encryption on a device level. Everything we write
    // into the nvm device will be encrypted.
    std::shared_ptr<navy::DeviceEncryptor> deviceEncryptor{};

    // Whether or not to store full alloc-class sizes into NVM device.
    // If true, only store the orignal size the user requested.
    bool truncateItemToOriginalAllocSizeInNvm{false};

    // Whether or not to disable the entire NvmCache after getting
    // a bad state from the underlying flash cache engine. A bad state
    // means we attempted to remove something but encountered error.
    // This used to mean the Flash engine may return the data we tried
    // to delete at a later time. Now it's no longer possible. So
    // this option is merely here to allow us to disable this behavior
    // gradually. See S421120 for more details.
    // Update: We're now disabling the NvmCache only when the device is throwing
    // errors with IO requests from the remove path. (DeviceError - which is a
    // real error). Leaving this option here for now, since it could be useful
    // in the future.
    bool disableNvmCacheOnBadState{true};

    // Offload the flash-hit copy-out (Navy read buffer -> DRAM item) to Intel
    // DSA through the DTO library for blobs of at least copyOutOffloadMinSize
    // bytes. Requires CacheLib built with BUILD_WITH_DTO; verified at
    // construction and silently falls back to memcpy when DSA is unusable.
    // The DRAM cache memory should be resident (pre-touched or huge-page slabs) or
    // the DSA work queue configured with block-on-fault, otherwise device
    // page faults turn every offloaded copy into a failed descriptor plus a
    // CPU copy.
    bool copyOutOffload{false};
    uint32_t copyOutOffloadMinSize{65536};

    // serialize the config for debugging purposes
    std::map<std::string, std::string> serialize() const;

    // validate the config, set any defaults that were not set and  return a
    // copy.
    //
    // @throw std::invalid_argument if encoding/decoding/encryption is
    // incompatible.
    Config validateAndSetDefaults();
  };

  // @param c         the cache instance using nvmcache
  // @param config    the config for nvmcache
  // @param truncate  if we should truncate the nvmcache store
  NvmCache(C& c,
           Config config,
           bool truncate,
           const ItemDestructor& itemDestructor,
           const navy::NavyPersistParams& navyPersistParams = {});

  // Look up item by key
  // @param key         key to lookup
  // @return            WriteHandle
  WriteHandle find(HashedKey key);

  // Returns true if a key is potentially in cache. There is a non-zero chance
  // the key does not exist in cache (e.g. hash collision in NvmCache). This
  // check is meant to be synchronous and fast as we only check DRAM cache and
  // in-memory index for NvmCache.
  //
  // @param key   the key for lookup
  // @return      true if the key could exist, false otherwise
  bool couldExistFast(HashedKey key);

  // Try to mark the key as in process of being evicted from RAM to NVM.
  // This is used to maintain the consistency between the RAM cache and
  // NvmCache when an item is transitioning from RAM to NvmCache in the
  // presence of concurrent gets and deletes from other threads.
  //
  // @param key         the key which is being evicted
  // @param fn          if the token is acquired successfully,
  //                    fn() will be executed while holding the token lock.
  //                    If fn() returns false, the token is discarded.
  // @return            a valid token if successfully marked and fn() returned
  // true.
  template <typename F>
  folly::Expected<PutToken, InFlightPuts::PutTokenError> createPutToken(
      folly::StringPiece key, F&& fn);

  // store the given item in navy
  // @param item        reference to cache item
  // @param token       the put token for the item. this must have been
  //                    obtained before enqueueing the put to maintain
  //                    consistency
  void put(Item& item, PutToken token);

  // returns the current state of whether nvmcache is enabled or not. nvmcache
  // can be disabled if the backend implementation ends up in a corrupt state
  bool isEnabled() const noexcept { return navyEnabled_; }

  // creates a delete tombstone for the key. This will ensure that all
  // concurrent gets and puts to nvmcache can synchronize with an upcoming
  // delete to make the cache consistent.
  DeleteTombStoneGuard createDeleteTombStone(HashedKey hk);

  // remove an item by key
  // @param hk          key to remove with hash
  // @param tombstone   the tombstone guard associated for this key. See
  //                    CacheAllocator::remove on how tombstone maintain
  //                    consistency with presence of concurrent get/puts to the
  //                    same key from nvmcache.Tombstone is kept alive and
  //                    destroyed until the remove operation is finished
  //                    processing asynchronously.
  //                    The tombstone should be created by
  //                    'createDeleteTombStone', it can be created right before
  //                    the remove called if caller is only removing item in
  //                    nvm; if the caller is also removing the item from ram,
  //                    tombstone should be created before removing item in ram.
  void remove(HashedKey hk, DeleteTombStoneGuard tombstone);

  // peek the nvmcache without bringing the item into the cache. creates a
  // temporary item handle with the content of the nvmcache. this is intended
  // for debugging purposes
  //
  // @param key   the key for the cache item
  // @return    handle to the item in nvmcache if present. if not, nullptr is
  //            returned. if a handle is returned, it is not inserted into
  //            cache and is temporary.
  WriteHandle peek(folly::StringPiece key);

  SampleItem getSampleItem();

  // safely shut down the cache. must be called after stopping all concurrent
  // access to cache. using nvmcache after this will result in no-op.
  // Returns true if shutdown was performed properly, false otherwise.
  bool shutDown();

  // How long the persist inside shutDown() took. Only meaningful after a
  // successful shutDown().
  uint64_t getLastPersistTimeMs() const { return lastPersistTimeMs_; }

  // blocks until all in-flight ops are flushed to the device. To be used when
  // there are no more operations being enqueued.
  void flushPendingOps();

  // Obtain stats in a <string -> double> representation.
  util::StatsMap getStatsMap() const;

  // returns the size of the NVM device
  size_t getSize() const noexcept { return navyCache_->getSize(); }

  // returns the size of the NVM space used for caching
  size_t getUsableSize() const noexcept { return navyCache_->getUsableSize(); }

  bool updateMaxRateForDynamicRandomAP(uint64_t maxRate) {
    return navyCache_->updateMaxRateForDynamicRandomAP(maxRate);
  }

  // Set the EventTracker for all underlying NVM engines
  void setEventTracker(EventTracker* tracker) {
    if (navyCache_) {
      navyCache_->setEventTracker(tracker);
    }
  }

  // This lock is to protect concurrent NvmCache evictCB and CacheAllocator
  // remove/insertOrReplace/invalidateNvm.
  // This lock scope within the above functions is
  // 1. check DRAM visibility in NvmCache evictCB
  // 2. modify DRAM visibility in CacheAllocator remove/insertOrReplace
  // 3. check/modify NvmClean, NvmEvicted flag
  // 4. check/modify itemRemoved_ set
  // The lock ensures that the items in itemRemoved_ must exist in nvm, and nvm
  // eviction must erase item from itemRemoved_, so there won't memory leak or
  // influence to future item with same key.
  auto getItemDestructorLock(HashedKey hk) const {
    using LockType = std::unique_lock<ItemDestructorMutex>;
    return itemDestructor_ ? LockType{itemDestructorMutex_[getShardForKey(hk)]}
                           : LockType{};
  }

  // For items with this key that are present in NVM, mark the DRAM to be the
  // authoritative copy for destructor events. This is usually done when items
  // are in-place mutated/removed/replaced and the nvm copy is being
  // invalidated by calling NvmCache::remove subsequently.
  //
  // caller must make sure itemDestructorLock is locked,
  // and the item is present in NVM (NvmClean set and NvmEvicted flag unset).
  void markNvmItemRemovedLocked(HashedKey hk);

  void updateAccessTime(HashedKey hk, uint32_t accessTimeSecs) {
    if (accessTimeMap_) {
      accessTimeMap_->set(hk.keyHash(), accessTimeSecs);
    }
  }

 private:
  // Helper function to record event with NvmItem metadata
  // @param event    The allocator API event type
  // @param key      The key for the event
  // @param result   The result of the operation
  // @param nvmItem  The NvmItem to extract metadata from
  void recordEvent(AllocatorApiEvent event,
                   folly::StringPiece key,
                   AllocatorApiResult result,
                   const NvmItem* nvmItem = nullptr) {
    // Check the event mask and sample the key once here. Use
    // recordEventWithoutSampling() downstream to avoid double/triple-sampling
    // through CacheAllocator::recordEvent().
    if (!CacheAPIWrapperForNvm<C>::shouldRecordEvent(cache_, event, key)) {
      return;
    }

    if (!nvmItem) {
      CacheAPIWrapperForNvm<C>::recordEventWithoutSampling(cache_, event, key,
                                                           result);
      return;
    }

    const auto blob = nvmItem->getBlob(0);
    const auto itemSize = blob.data.size();
    const auto allocSize = blob.origAllocSize;
    const auto expiryTime = nvmItem->getExpiryTime();
    uint32_t ttlSecs = 0;
    if (expiryTime != 0 && expiryTime > nvmItem->getCreationTime()) {
      ttlSecs = expiryTime - nvmItem->getCreationTime();
    }

    CacheAPIWrapperForNvm<C>::recordEventWithoutSampling(
        cache_, event, key, result,
        typename C::EventRecordParams{.size = itemSize,
                                      .ttlSecs = ttlSecs,
                                      .expiryTime = expiryTime,
                                      .allocSize = allocSize,
                                      .poolId = nvmItem->poolId()});
  }

  // returns the itemRemoved_ set size
  // it is the number of items were both in dram and nvm
  // and were removed from dram but not yet removed from nvm
  uint64_t getNvmItemRemovedSize() const;

  bool checkAndUnmarkItemRemovedLocked(HashedKey hk);

  detail::Stats& stats() { return CacheAPIWrapperForNvm<C>::getStats(cache_); }

  // Returns true if the NVM item would be routed to BlockCache (large item
  // engine) rather than BigHash (small item engine).
  // Delegates to navy::isItemLarge() (navy/common/Types.h) which is the
  // shared routing predicate also used by EnginePair::isItemLarge().
  bool isNvmItemLarge(folly::StringPiece key, const NvmItem& nvmItem) const {
    return navy::isItemLarge(key.size(), nvmItem.totalSize(),
                             smallItemMaxSize_);
  }

  // creates the RAM item from NvmItem.
  //
  // @param key   key for the nvm item
  // @param nvmItem contents for the key
  //
  // @param   return an item handle allocated and initialized to the right state
  //          based on the NvmItem
  WriteHandle createItem(folly::StringPiece key, const NvmItem& nvmItem);

  // Copies a blob's payload into freshly allocated item memory, on DSA when
  // copy-out offload is active and the blob is large enough (see
  // Config::copyOutOffload), otherwise with memcpy.
  void copyBlobData(void* dest, const void* src, size_t n);

  // creates the item into IOBuf from NvmItem, if the item has chained items,
  // chained IOBufs will be created.
  // @param key   key for the dipper item
  // @param nvmItem contents for the key
  // @param parentOnly create item and IOBuf only for the parent item
  //
  // @return an IOBuf allocated for the item and initialized the memory to Item
  //          based on the NvmItem
  std::unique_ptr<folly::IOBuf> createItemAsIOBuf(folly::StringPiece key,
                                                  const NvmItem& nvmItem,
                                                  bool parentOnly = false);
  // Returns an iterator to the item's chained IOBufs. The order of
  // iteration on the item will be LIFO of the addChainedItem calls.
  // This is only used when we have to create cache items on heap (IOBuf) for
  // the purpose of ItemDestructor.
  template <typename Iter>
  folly::Range<Iter> viewAsChainedAllocsRangeT(folly::IOBuf* buf) const;

  folly::Range<ChainedItemIter> viewAsChainedAllocsRange(
      folly::IOBuf* parent) const {
    return viewAsChainedAllocsRangeT<ChainedItemIter>(parent);
  }

  folly::Range<WritableChainedItemIter> viewAsWritableChainedAllocsRange(
      folly::IOBuf* parent) const {
    return viewAsChainedAllocsRangeT<WritableChainedItemIter>(parent);
  }

  // returns true if there is tombstone entry for the key.
  bool hasTombStone(HashedKey hk);

  std::unique_ptr<NvmItem> makeNvmItem(const Item& item, uint8_t poolId);

  // wrap an item into a blob for writing into navy.
  Blob makeBlob(const Item& it);
  uint32_t getStorageSizeInNvm(const Item& it);

  // Holds all the necessary data to do an async navy get
  // All of the supported operations aren't thread safe. The caller
  // needs to ensure thread safety
  struct GetCtx {
    NvmCache& cache;       //< the NvmCache instance
    const std::string key; //< key being fetched
    folly::small_vector<std::shared_ptr<WaitContext<ReadHandle>>, 4>
        waiters;    // list of
                    // waiters
    WriteHandle it; // will be set when Context is being filled
    util::LatencyTracker tracker_;
    bool valid_;

    GetCtx(NvmCache& c,
           folly::StringPiece k,
           std::shared_ptr<WaitContext<ReadHandle>> ctx,
           util::LatencyTracker tracker)
        : cache(c),
          key(k.toString()),
          tracker_(std::move(tracker)),
          valid_(true) {
      it.markWentToNvm();
      addWaiter(std::move(ctx));
    }

    ~GetCtx() {
      // prevent any further enqueue to waiters
      // Note: we don't need to hold locks since no one can enqueue
      // after this point.
      wakeUpWaiters();
    }

    // @return  key as StringPiece
    folly::StringPiece getKey() const { return {key.data(), key.length()}; }

    // record the item handle. Upon destruction we will wake up the waiters
    // and pass a clone of the handle to the callBack. By default we pass
    // a null handle
    void setWriteHandle(WriteHandle _it) { it = std::move(_it); }

    // enqueue a waiter into the waiter list
    // @param  waiter       WaitContext
    void addWaiter(std::shared_ptr<WaitContext<ReadHandle>> waiter) {
      XDCHECK(waiter);
      waiters.push_back(std::move(waiter));
    }

    // notify all pending waiters that are waiting for the fetch.
    void wakeUpWaiters() {
      bool refcountOverflowed = false;
      for (auto& w : waiters) {
        // If refcount overflowed earlier, then we will return miss to
        // all subsequent waitors.
        if (refcountOverflowed) {
          w->set(WriteHandle{});
          continue;
        }

        try {
          w->set(it.clone());
        } catch (const exception::RefcountOverflow&) {
          // We'll return a miss to the user's pending read,
          // so we should enqueue a delete via NvmCache.
          cache.cache_.remove(it);
          refcountOverflowed = true;
        }
      }
    }

    void invalidate() { valid_ = false; }

    bool isValid() const { return valid_; }
  };

  // Erase entry for the ctx from the fill map
  // @param     ctx   ctx to erase
  void removeFromFillMap(HashedKey hk) {
    std::unique_ptr<GetCtx> to_delete;
    {
      const auto shard = getShardForKey(hk);
      auto lock = getFillLockForShard(shard);
      auto& map = getFillMapForShard(shard);
      auto it = map.find(hk.key());
      if (it == map.end()) {
        return;
      }
      to_delete = std::move(it->second);
      map.erase(it);
    }
  }

  // Erase entry for the ctx from the fill map
  // @param     key   item key
  void invalidateFill(HashedKey hk) {
    const auto shard = getShardForKey(hk);
    auto lock = getFillLockForShard(shard);
    auto& map = getFillMapForShard(shard);
    auto it = map.find(hk.key());
    if (it != map.end() && it->second) {
      it->second->invalidate();
    }
  }

  // Logs and disables navy usage
  void disableNavy(const std::string& msg);

  // map of concurrent fills by key. The key is a string piece wrapper around
  // GetCtx's std::string. This makes the lookups possible without
  // constructing a string key.
  using FillMap =
      folly::F14ValueMap<folly::StringPiece, std::unique_ptr<GetCtx>>;

  size_t getShardForKey(HashedKey hk) const {
    return hk.keyHash() % numShards_;
  }

  FillMap& getFillMapForShard(size_t shard) { return fills_[shard].fills_; }

  /**
   * Obtains an exclusive fill lock for the shard.
   */
  auto getFillLockForShard(size_t shard) {
    return std::unique_lock{fillLock_[shard].fillLock_};
  }

  /**
   * Obtains a shared fill lock for the shard.
   */
  auto getSharedFillLockForShard(size_t shard) {
    return std::shared_lock{fillLock_[shard].fillLock_};
  }

  void onGetComplete(GetCtx& ctx,
                     navy::Status s,
                     HashedKey key,
                     navy::BufferView value,
                     uint32_t lastAccessTimeSecs);

  void evictCB(HashedKey hk, navy::BufferView val, navy::DestructorEvent e);

  static navy::BufferView makeBufferView(folly::ByteRange b) {
    return navy::BufferView{b.size(), b.data()};
  }

  const Config config_;

  // Whether the DSA copy-out offload passed its runtime self-check.
  bool copyOutOffload_{false};
  C& cache_;                            //< cache allocator
  std::atomic<bool> navyEnabled_{true}; //< switch to turn off/on navy

  uint64_t lastPersistTimeMs_{0};

  const size_t numShards_;

  // a function to check if an item is expired
  const navy::ExpiredCheck checkExpired_;

  // a map of all pending fills to prevent thundering herds
  struct FillStruct {
    alignas(folly::hardware_destructive_interference_size) FillMap fills_;
  };
  std::vector<FillStruct> fills_;

  // a map of fill locks for each shard
  struct FillLockStruct {
    alignas(folly::hardware_destructive_interference_size) trace::Profiled<
        folly::fibers::TimedRWMutexWritePriority<folly::fibers::GenericBaton>,
        "cachelib:nvmcache:fill"> fillLock_;
  };
  std::vector<FillLockStruct> fillLock_;

  // currently queued put operations to navy.
  std::vector<PutContexts> putContexts_;

  // currently queued delete operations to navy.
  std::vector<DelContexts> delContexts_;

  // co-ordination between in-flight evictions from cache that are not queued
  // to navy and in-flight gets into nvmcache that are not yet queued.
  std::vector<InFlightPuts> inflightPuts_;
  std::vector<TombStones> tombstones_;

  const ItemDestructor itemDestructor_;

  mutable std::vector<ItemDestructorMutex> itemDestructorMutex_{numShards_};

  // Used to track the keys of items present in NVM that should be excluded for
  // executing Destructor upon eviction from NVM, if the item is not present in
  // DRAM. The ownership of item destructor is already managed elsewhere for
  // these keys. This data struct is updated prior to issueing NvmCache::remove
  // to handle any racy eviction from NVM before the NvmCache::remove is
  // finished.
  std::vector<folly::F14FastSet<std::string>> itemRemoved_;

  // Tracks the most recent DRAM last-access timestamp for items in NVM.
  // It is best offered and not updated on every DRAM access so could sometime
  // have stale value which represents the promotion timestamp.
  // Populated on DRAM eviction of NvmClean BlockCache items;
  // consumed during BlockCache region reclaim to write fresh timestamps.
  std::unique_ptr<AccessTimeMap> accessTimeMap_;

  // BigHash small-item threshold from NavyConfig. Items with
  // key.size() + nvmBufferSize <= this threshold go to BigHash.
  // 0 means BigHash is not configured and all items go to BlockCache.
  const uint64_t smallItemMaxSize_;

  // Non-owning pointer to the ShmManager used for ATM persistence.
  // nullptr if shm persistence is not available.
  ShmManager* shmManager_{nullptr};

  std::unique_ptr<cachelib::navy::AbstractCache> navyCache_;

  friend class tests::NvmCacheTest;
  FRIEND_TEST(CachelibAdminCoreTest, WorkingSetAnalysisLoggingTest);
};

template <typename C>
std::map<std::string, std::string> NvmCache<C>::Config::serialize() const {
  std::map<std::string, std::string> configMap;
  configMap = navyConfig.serialize();
  configMap["encodeCB"] = encodeCb ? "set" : "empty";
  configMap["decodeCb"] = decodeCb ? "set" : "empty";
  configMap["encryption"] = deviceEncryptor ? "set" : "empty";
  configMap["truncateItemToOriginalAllocSizeInNvm"] =
      truncateItemToOriginalAllocSizeInNvm ? "true" : "false";
  configMap["disableNvmCacheOnBadState"] =
      disableNvmCacheOnBadState ? "true" : "false";
  configMap["copyOutOffload"] = copyOutOffload ? "true" : "false";
  configMap["copyOutOffloadMinSize"] = std::to_string(copyOutOffloadMinSize);
  return configMap;
}

template <typename C>
typename NvmCache<C>::Config NvmCache<C>::Config::validateAndSetDefaults() {
  const bool hasEncodeCb = !!encodeCb;
  const bool hasDecodeCb = !!decodeCb;
  if (hasEncodeCb != hasDecodeCb) {
    throw std::invalid_argument(
        "Encode and Decode CBs must be both specified or both empty.");
  }

  if (deviceEncryptor) {
    auto encryptionBlockSize = deviceEncryptor->encryptionBlockSize();
    auto blockSize = navyConfig.getBlockSize();
    if (blockSize % encryptionBlockSize != 0) {
      throw std::invalid_argument(fmt::format(
          "Encryption enabled but the encryption block granularity is not "
          "aligned to the navy block size. encryption block size: {}, "
          "block size: {}",
          encryptionBlockSize,
          blockSize));
    }

    if (navyConfig.isBigHashEnabled()) {
      auto bucketSize = navyConfig.bigHash().getBucketSize();
      if (bucketSize % encryptionBlockSize != 0) {
        throw std::invalid_argument(
            fmt::format("Encryption enabled but the encryption block "
                        "granularity is not aligned to the navy "
                        "big hash bucket size. ecryption block "
                        "size: {}, bucket size: {}",
                        encryptionBlockSize,
                        bucketSize));
      }
    }
  }

  return *this;
}

template <typename C>
typename NvmCache<C>::DeleteTombStoneGuard NvmCache<C>::createDeleteTombStone(
    HashedKey hk) {
  // lower bits for shard and higher bits for key.
  const auto shard = getShardForKey(hk);
  auto guard = tombstones_[shard].add(hk.key());

  // need to synchronize tombstone creations with fill lock to serialize
  // async fills with deletes
  // o/w concurrent onGetComplete might re-insert item to RAM after tombstone
  // is created.
  // The lock here is to that  adding the tombstone and checking the dram cache
  // in remove path happens entirely before or  happens entirely after checking
  // for tombstone and inserting into dram from the get path.
  // If onGetComplete is holding the FillLock, wait the insertion to be done.
  // If onGetComplete has not yet acquired FillLock, we are fine to exit since
  // hasTombStone in onGetComplete is checked after acquiring FillLock.
  auto lock = getFillLockForShard(shard);

  return guard;
}

template <typename C>
bool NvmCache<C>::hasTombStone(HashedKey hk) {
  return tombstones_[getShardForKey(hk)].isPresent(hk.key());
}

template <typename C>
typename NvmCache<C>::WriteHandle NvmCache<C>::find(HashedKey hk) {
  if (!isEnabled()) {
    return WriteHandle{};
  }

  stats().numNvmGets.inc();

  auto shard = getShardForKey(hk);
  // invalidateToken any inflight puts from DRAM -> NVM for the key
  inflightPuts_[shard].invalidateToken(hk.key());
  // map of fills from NVM -> DRAM for the shard to which the key belongs
  auto& fillMap = getFillMapForShard(shard);

  /*
   * Check for 3 things after we get the lock,
   *  1. Is the item now in DRAM? If so, return.
   *  2. FillMap has an entry so we can join as waiter in a GetCtx.
   *  3. Assuming 1 and 2 are false, we can do a fast negative lookup.
   */
  auto checkShared =
      [this, &hk, &shard,
       &fillMap](typename FillMap::iterator& itOut,
                 AllocatorApiEvent event) -> std::optional<WriteHandle> {
    // do not use the Cache::find() since that will call back into us.
    auto hdlShared = CacheAPIWrapperForNvm<C>::findInternal(cache_, hk.key());
    if (UNLIKELY(hdlShared != nullptr)) {
      AllocatorApiResult findResult = AllocatorApiResult::FOUND;
      if (hdlShared->isExpired()) {
        hdlShared.reset();
        hdlShared.markExpired();
        stats().numNvmGetMissExpired.inc();
        stats().numNvmGetMissFast.inc();
        stats().numNvmGetMiss.inc();
        findResult = AllocatorApiResult::EXPIRED;
      }
      // Special case where we have to record metadata with handle in NvmCache.h
      CacheAPIWrapperForNvm<C>::recordEvent(cache_, event, hk.key(),
                                            findResult);
      return hdlShared;
    }

    itOut = fillMap.find(hk.key());
    // we use async apis for nvmcache operations into navy. async apis for
    // lookups incur additional overheads and thread hops. However, navy can
    // quickly answer negative lookups through a synchronous api. So we try to
    // quickly validate this, if possible, before doing the heavier async
    // lookup.
    //
    // since this is a synchronous api, navy would not guarantee any
    // particular ordering semantic with other concurrent requests to same
    // key. we need to ensure there are no asynchronous api requests for the
    // same key. First, if there are already concurrent get requests, we
    // simply add ourselves to the list of waiters for that get. If there are
    // concurrent put requests already enqueued, executing this synchronous
    // api can read partial state. Hence the result can not be trusted. If
    // there are concurrent delete requests enqueued, we might get false
    // positives that key is present. That is okay since it is a loss of
    // performance and not correctness.
    //
    // For concurrent put, if it is already enqueued, its put context already
    // exists. If it is not enqueued yet (in-flight) the above invalidateToken
    // will prevent the put from being enqueued.
    if (itOut == fillMap.end() && !putContexts_[shard].hasContexts() &&
        !navyCache_->couldExist(hk)) {
      stats().numNvmGetMiss.inc();
      stats().numNvmGetMissFast.inc();
      recordEvent(event, hk.key(), AllocatorApiResult::NOT_FOUND);
      return WriteHandle{};
    }
    return std::nullopt;
  };

  WriteHandle hdl{nullptr};
  typename FillMap::iterator it{};
  GetCtx* ctx{nullptr};

  {
    // Hot miss path: shared lock, immutable access only
    auto lock = getSharedFillLockForShard(shard);
    if (auto hdlShared = checkShared(it, AllocatorApiEvent::NVM_FIND_FAST)) {
      return *std::move(hdlShared);
    }
    // fallthrough
  }
  {
    // Slow path: exclusive lock
    auto lock = getFillLockForShard(shard);

    // Recheck miss path under exclusive lock
    if (auto hdlShared = checkShared(it, AllocatorApiEvent::NVM_FIND)) {
      return *std::move(hdlShared);
    }

    hdl = CacheAPIWrapperForNvm<C>::createNvmCacheFillHandle(cache_);
    hdl.markWentToNvm();

    auto waitContext = CacheAPIWrapperForNvm<C>::getWaitContext(cache_, hdl);
    XDCHECK(waitContext);

    if (it != fillMap.end()) {
      ctx = it->second.get();
      ctx->addWaiter(std::move(waitContext));
      stats().numNvmGetCoalesced.inc();
      return hdl;
    }

    // create a context — start latency tracking here so bloom filter
    // fast-path misses and coalesced lookups are excluded
    util::LatencyTracker tracker(stats().nvmLookupLatency_);
    auto newCtx = std::make_unique<GetCtx>(
        *this, hk.key(), std::move(waitContext), std::move(tracker));
    auto res =
        fillMap.emplace(std::make_pair(newCtx->getKey(), std::move(newCtx)));
    XDCHECK(res.second);
    ctx = res.first->second.get();
  } // scope for fill lock

  XDCHECK(ctx);
  auto guard = folly::makeGuard([hk, this]() { removeFromFillMap(hk); });

  navyCache_->lookupAsync(
      HashedKey::precomputed(ctx->getKey(), hk.keyHash()),
      [this, ctx](navy::Status s, HashedKey k, navy::Buffer v,
                  uint32_t lastAccessTimeSecs) {
        this->onGetComplete(*ctx, s, k, v.view(), lastAccessTimeSecs);
      });
  guard.dismiss();
  return hdl;
}

template <typename C>
bool NvmCache<C>::couldExistFast(HashedKey hk) {
  if (!isEnabled()) {
    return false;
  }

  const auto shard = getShardForKey(hk);
  // invalidateToken any inflight puts for the same key since we are filling
  // from nvmcache.
  inflightPuts_[shard].invalidateToken(hk.key());

  auto lock = getFillLockForShard(shard);
  // do not use the Cache::find() since that will call back into us.
  auto hdl = CacheAPIWrapperForNvm<C>::findInternal(cache_, hk.key());
  if (hdl != nullptr) {
    if (hdl->isExpired()) {
      return false;
    }
    return true;
  }

  auto& fillMap = getFillMapForShard(shard);
  auto it = fillMap.find(hk.key());
  // we use async apis for nvmcache operations into navy. async apis for
  // lookups incur additional overheads and thread hops. However, navy can
  // quickly answer negative lookups through a synchronous api. So we try to
  // quickly validate this, if possible, before doing the heavier async
  // lookup.
  //
  // since this is a synchronous api, navy would not guarantee any
  // particular ordering semantic with other concurrent requests to same
  // key. we need to ensure there are no asynchronous api requests for the
  // same key. First, if there are already concurrent get requests, we
  // simply add ourselves to the list of waiters for that get. If there are
  // concurrent put requests already enqueued, executing this synchronous
  // api can read partial state. Hence the result can not be trusted. If
  // there are concurrent delete requests enqueued, we might get false
  // positives that key is present. That is okay since it is a loss of
  // performance and not correctness.
  //
  // For concurrent put, if it is already enqueued, its put context already
  // exists. If it is not enqueued yet (in-flight) the above invalidateToken
  // will prevent the put from being enqueued.
  if (it == fillMap.end() && !putContexts_[shard].hasContexts() &&
      !navyCache_->couldExist(hk)) {
    return false;
  }

  return true;
}

template <typename C>
typename NvmCache<C>::WriteHandle NvmCache<C>::peek(folly::StringPiece key) {
  if (!isEnabled()) {
    return nullptr;
  }

  trace::Profiled<folly::fibers::Baton, "cachelib:nvmcache:peek"> b;
  WriteHandle hdl{};
  hdl.markWentToNvm();

  // no need for fill lock or inspecting the state of other concurrent
  // operations since we only want to check the state for debugging purposes.
  navyCache_->lookupAsync(
      HashedKey{key},
      [&, this](navy::Status st, HashedKey, navy::Buffer v, uint32_t) {
        if (st != navy::Status::NotFound) {
          auto nvmItem = reinterpret_cast<const NvmItem*>(v.data());
          hdl = createItem(key, *nvmItem);
        }
        b.post();
      });
  b.wait();
  return hdl;
}

template <typename C>
void NvmCache<C>::evictCB(HashedKey hk,
                          navy::BufferView value,
                          navy::DestructorEvent event) {
  // invalidate any inflight lookup that is on flight since we are evicting it.
  invalidateFill(hk);

  const auto& nvmItem = *reinterpret_cast<const NvmItem*>(value.data());

  if (event == cachelib::navy::DestructorEvent::Recycled) {
    // Recycled means item is evicted
    // update stats for eviction
    stats().numNvmEvictions.inc();

    const auto timeNow = util::getCurrentTimeSec();
    const auto lifetime = timeNow - nvmItem.getCreationTime();
    const auto expiryTime = nvmItem.getExpiryTime();
    if (expiryTime != 0) {
      if (expiryTime < timeNow) {
        stats().numNvmExpiredEvict.inc();
        stats().nvmEvictionSecondsPastExpiry_.trackValue(timeNow - expiryTime);
      } else {
        stats().nvmEvictionSecondsToExpiry_.trackValue(expiryTime - timeNow);
      }
    }
    navyCache_->isItemLarge(hk, value)
        ? stats().nvmLargeLifetimeSecs_.trackValue(lifetime)
        : stats().nvmSmallLifetimeSecs_.trackValue(lifetime);
    navyCache_->updateEvictionStats(hk, value, lifetime);

    recordEvent(AllocatorApiEvent::NVM_EVICT, hk.key(),
                AllocatorApiResult::EVICTED, &nvmItem);
  }

  // Clean up ATM entry since the item is leaving NVM.
  // PutFailed is excluded because an item going through the put() path
  // never goes through updateAccessTime(), so no ATM entry exists.
  if ((event == cachelib::navy::DestructorEvent::Removed ||
       event == cachelib::navy::DestructorEvent::Recycled) &&
      accessTimeMap_) {
    accessTimeMap_->remove(hk.keyHash());
  }

  bool needDestructor = true;
  {
    // The ItemDestructorLock is to protect:
    // 1. peek item in DRAM cache,
    // 2. check it's NvmClean flag
    // 3. mark NvmEvicted flag
    // 4. lookup itemRemoved_ set.
    // Concurrent DRAM cache remove/replace/update for same item could
    // modify DRAM index, check NvmClean/NvmEvicted flag, update itemRemoved_
    // set, and unmark NvmClean flag.
    auto lock = getItemDestructorLock(hk);
    WriteHandle hdl;
    try {
      // FindInternal returns us the item in DRAM cache as long as this
      // item can be found via DRAM cache's Access Container.
      hdl =
          WriteHandle{CacheAPIWrapperForNvm<C>::findInternal(cache_, hk.key())};
    } catch (const exception::RefcountOverflow& ex) {
      // TODO(zixuan) item exists in DRAM, but we can't obtain the handle
      // and mark it as NvmEvicted. In this scenario, there are two
      // possibilities when the item is removed from nvm.
      // 1. destructor is not executed: The item in DRAM is still marked
      // NvmClean, so when it is evicted from DRAM, destructor is also skipped
      // since we infer nvm copy exists  (NvmClean && !NvmEvicted). In this
      // case, we incorrectly skip executing an item destructor and it is also
      // possible to leak the itemRemoved_ state if this item is
      // removed/replaced from DRAM before this happens.
      // 2. destructor is executed here: In addition to destructor being
      // executed here, it could also be executed if the item was removed from
      // DRAM and the handle goes out of scope. Among the two, (1) is preferred,
      // until we can solve this, since executing destructor here while item
      // handle being outstanding and being possibly used is dangerous.
      XLOGF(ERR,
            "Refcount overflowed when trying peek at an item in "
            "NvmCache::evictCB. key: {}, ex: {}",
            hk.key(),
            ex.what());
      stats().numNvmDestructorRefcountOverflow.inc();
      return;
    }

    if (hdl && hdl->isNvmClean()) {
      // item found in RAM and it is NvmClean
      // this means it is the same copy as what we are evicting/removing
      needDestructor = false;
      if (hdl->isNvmEvicted()) {
        // this means we evicted something twice. This should not happen even we
        // could have two copies in the nvm cache, since we only have one copy
        // in index, the one not in index should not reach here.
        stats().numNvmCleanDoubleEvict.inc();
      } else {
        hdl->markNvmEvicted();
        stats().numNvmCleanEvict.inc();
      }
    } else {
      if (hdl) {
        // item found in RAM but is NOT NvmClean
        // this happens when RAM copy is in-place updated, or replaced with a
        // new item.
        stats().numNvmUncleanEvict.inc();
      }

      // If we can't find item from DRAM or isNvmClean flag not set, it might be
      // removed/replaced. Check if it is in itemRemoved_, item existing in
      // itemRemoved_ means it was in DRAM, was removed/replaced and
      // destructor should have been executed by the DRAM copy.
      //
      // PutFailed event can skip the check because when item was in flight put
      // and failed it was the latest copy and item was not removed/replaced
      // but it could exist in itemRemoved_ due to in-place mutation and the
      // legacy copy in NVM is still pending to be removed.
      if (event != cachelib::navy::DestructorEvent::PutFailed &&
          checkAndUnmarkItemRemovedLocked(hk)) {
        needDestructor = false;
      }
    }
  }

  if (!needDestructor) {
    return;
  }

  if (event != cachelib::navy::DestructorEvent::Removed) {
    stats().numCacheEvictions.inc();
  }
  // ItemDestructor
  if (itemDestructor_) {
    // create the item on heap instead of memory pool to avoid allocation
    // failure and evictions from cache for a temporary item.
    auto iobuf = createItemAsIOBuf(hk.key(), nvmItem);
    if (iobuf) {
      auto& item = *reinterpret_cast<Item*>(iobuf->writableData());
      // make chained items
      auto chained = viewAsChainedAllocsRange(iobuf.get());
      auto context = event == cachelib::navy::DestructorEvent::Removed
                         ? DestructorContext::kRemovedFromNVM
                         : DestructorContext::kEvictedFromNVM;

      try {
        itemDestructor_(DestructorData{context, item, std::move(chained),
                                       nvmItem.poolId()});
        stats().numNvmDestructorCalls.inc();
      } catch (const std::exception& e) {
        stats().numDestructorExceptions.inc();
        XLOG_EVERY_N(INFO, 100)
            << "Catch exception from user's item destructor: " << e.what();
      }
    }
  }
}

template <typename C>
template <typename Iter>
folly::Range<Iter> NvmCache<C>::viewAsChainedAllocsRangeT(
    folly::IOBuf* parent) const {
  XDCHECK(parent);
  auto& item = *reinterpret_cast<Item*>(parent->writableData());
  return item.hasChainedItem()
             ? folly::Range<Iter>{Iter{parent->next()}, Iter{parent}}
             : folly::Range<Iter>{};
}

template <typename C>
NvmCache<C>::NvmCache(C& c,
                      Config config,
                      bool truncate,
                      const ItemDestructor& itemDestructor,
                      const navy::NavyPersistParams& navyPersistParams)
    : config_(config.validateAndSetDefaults()),
      cache_(c),
      numShards_{config_.navyConfig.getNumShards()},
      checkExpired_([](navy::BufferView v) -> bool {
        const auto& nvmItem = *reinterpret_cast<const NvmItem*>(v.data());
        return nvmItem.isExpired();
      }),
      fills_(numShards_),
      fillLock_(numShards_),
      putContexts_(numShards_),
      delContexts_(numShards_),
      inflightPuts_(numShards_),
      tombstones_(numShards_),
      itemDestructor_(itemDestructor),
      itemDestructorMutex_(numShards_),
      itemRemoved_(numShards_),
      accessTimeMap_(
          config_.navyConfig.getEnableAccessTimeMap()
              ? std::make_unique<AccessTimeMap>(
                    numShards_, config_.navyConfig.getAccessTimeMapMaxSize())
              : nullptr),
      smallItemMaxSize_(config_.navyConfig.isBigHashEnabled()
                            ? config_.navyConfig.bigHash().getSmallItemMaxSize()
                            : 0),
      shmManager_(navyPersistParams.shmManager.has_value()
                      ? &navyPersistParams.shmManager.value().get()
                      : nullptr) {
  navyCache_ = createNavyCache(
      config_.navyConfig,
      checkExpired_,
      [this](HashedKey hk, navy::BufferView v, navy::DestructorEvent e) {
        this->evictCB(hk, v, e);
      },
      truncate,
      std::move(config.deviceEncryptor),
      itemDestructor_ ? true : false,
      navyPersistParams);

  if (config_.copyOutOffload) {
    copyOutOffload_ = navy::copyOffloadSelfCheck();
    if (copyOutOffload_) {
      XLOGF(INFO,
            "NvmCache: DSA copy-out offload active for blobs >= {} bytes",
            config_.copyOutOffloadMinSize);
    } else {
      XLOG(WARN) << "NvmCache: DSA copy-out offload requested but the DTO/DSA "
                    "self-check failed (not built with DTO, or DSA "
                    "unavailable); falling back to memcpy";
    }
  }

  if (accessTimeMap_ && shmManager_) {
    try {
      accessTimeMap_->recover(*shmManager_);
    } catch (const std::exception& e) {
      XLOGF(ERR, "Failed to recover AccessTimeMap: {}", e.what());
    }
  }
}

template <typename C>
Blob NvmCache<C>::makeBlob(const Item& it) {
  return Blob{
      // User requested size
      it.getSize(),
      // Storage size in NvmCache may be greater than user-requested-size
      // if nvmcache is configured with useTruncatedAllocSize == false
      {reinterpret_cast<const char*>(it.getMemory()), getStorageSizeInNvm(it)}};
}

template <typename C>
uint32_t NvmCache<C>::getStorageSizeInNvm(const Item& it) {
  return config_.truncateItemToOriginalAllocSizeInNvm
             ? it.getSize()
             : cache_.getUsableSize(it);
}

template <typename C>
std::unique_ptr<NvmItem> NvmCache<C>::makeNvmItem(const Item& item,
                                                  uint8_t poolId) {
  if (item.isChainedItem()) {
    throw std::invalid_argument(fmt::format(
        "Chained item can not be flushed separately {}", item.toString()));
  }

  auto chainedItemRange =
      CacheAPIWrapperForNvm<C>::viewAsChainedAllocsRange(cache_, item);
  if (config_.encodeCb && !config_.encodeCb(EncodeDecodeContext{
                              const_cast<Item&>(item), chainedItemRange})) {
    return nullptr;
  }

  if (config_.makeBlobCb) {
    std::vector<BufferedBlob> bufferedBlobs;
    {
      util::LatencyTracker tracker(stats().nvmMakeBlobCbLatency_);
      bufferedBlobs = config_.makeBlobCb(item, chainedItemRange);
    }
    if (bufferedBlobs.empty()) {
      return nullptr;
    }
    std::vector<Blob> blobs;
    blobs.reserve(bufferedBlobs.size());
    for (const auto& bufferedBlob : bufferedBlobs) {
      blobs.push_back(bufferedBlob.toBlob());
    }
    const size_t bufSize = NvmItem::estimateVariableSize(blobs);
    return blobs.size() == 1 ? std::unique_ptr<NvmItem>(new (bufSize) NvmItem(
                                   poolId, item.getCreationTime(),
                                   item.getExpiryTime(), blobs[0]))
                             : std::unique_ptr<NvmItem>(new (bufSize) NvmItem(
                                   poolId, item.getCreationTime(),
                                   item.getExpiryTime(), blobs));

  } else {
    if (item.hasChainedItem()) {
      std::vector<Blob> blobs;
      blobs.push_back(makeBlob(item));

      for (auto& chainedItem : chainedItemRange) {
        blobs.push_back(makeBlob(chainedItem));
      }

      const size_t bufSize = NvmItem::estimateVariableSize(blobs);
      return std::unique_ptr<NvmItem>(new (bufSize) NvmItem(
          poolId, item.getCreationTime(), item.getExpiryTime(), blobs));
    } else {
      Blob blob;
      // Support object cache without chained items only.
      blob = makeBlob(item);
      const size_t bufSize = NvmItem::estimateVariableSize(blob);
      return std::unique_ptr<NvmItem>(new (bufSize) NvmItem(
          poolId, item.getCreationTime(), item.getExpiryTime(), blob));
    }
  }
}

template <typename C>
void NvmCache<C>::put(Item& item, PutToken token) {
  HashedKey hk{item.getKey()};

  // for regular items that can only write to nvmcache upon eviction, we
  // should not be recording a write for an nvmclean item unless it is marked
  // as evicted from nvmcache.
  if (item.isNvmClean() && !item.isNvmEvicted()) {
    throw std::runtime_error(fmt::format(
        "Item is not nvm evicted and nvm clean {}", item.toString()));
  }

  if (item.isChainedItem()) {
    throw std::invalid_argument(
        fmt::format("Invalid item {}", item.toString()));
  }

  // we skip writing if we know that the item is expired or has chained items
  if (!isEnabled()) {
    return;
  } else if (item.isExpired()) {
    recordEvent(AllocatorApiEvent::NVM_INSERT, item.getKey(),
                AllocatorApiResult::EXPIRED);
    return;
  }

  stats().numNvmPuts.inc();
  if (hasTombStone(hk)) {
    stats().numNvmAbortedPutOnTombstone.inc();
    recordEvent(AllocatorApiEvent::NVM_INSERT, item.getKey(),
                AllocatorApiResult::ABORTED);
    return;
  }

  auto poolId =
      static_cast<uint8_t>(cache_.getAllocInfo((void*)(&item)).poolId);
  auto expiryTime = item.getExpiryTime();
  auto nvmItem = makeNvmItem(item, poolId);
  if (!nvmItem) {
    stats().numNvmPutEncodeFailure.inc();
    recordEvent(AllocatorApiEvent::NVM_INSERT, item.getKey(),
                AllocatorApiResult::ABORTED);
    return;
  }

  if (item.isNvmClean() && item.isNvmEvicted()) {
    stats().numNvmPutFromClean.inc();
  }

  auto iobuf = toIOBuf(std::move(nvmItem));
  const auto valSize = iobuf.length();
  auto val = folly::ByteRange{iobuf.data(), iobuf.length()};

  auto shard = getShardForKey(hk);
  // start latency tracking here so early-exit paths (disabled, expired,
  // tombstone, encode failure) are excluded from the latency stats
  util::LatencyTracker tracker(stats().nvmInsertLatency_);
  auto& putContexts = putContexts_[shard];
  auto& ctx = putContexts.createContext(item.getKey(), std::move(iobuf),
                                        std::move(tracker));
  // capture array reference for putContext. it is stable
  auto putCleanup = [&putContexts, &ctx]() { putContexts.destroyContext(ctx); };
  auto guard = folly::makeGuard([putCleanup]() { putCleanup(); });

  // On a concurrent get, we remove the key from inflight evictions and hence
  // key not being present means a concurrent get happened with an inflight
  // eviction, and we should abandon this write to navy since we already
  // reported the key doesn't exist in the cache.
  const bool executed = token.executeIfValid([&]() {
    auto status = navyCache_->insertAsync(
        HashedKey::precomputed(ctx.key(), hk.keyHash()), makeBufferView(val),
        [this, putCleanup, valSize, val](navy::Status st, HashedKey key) {
          auto eventRes = AllocatorApiResult::INSERTED;
          if (st == navy::Status::Ok) {
            stats().nvmPutSize_.trackValue(valSize);
          } else if (st == navy::Status::BadState) {
            eventRes = AllocatorApiResult::FAILED;
            // we set disable navy since we got a BadState from navy
            disableNavy("Insert Failure. BadState");
          } else {
            eventRes = AllocatorApiResult::FAILED;
            // put failed, DRAM eviction happened and destructor was not
            // executed. we unconditionally trigger destructor here for cleanup.
            evictCB(key, makeBufferView(val), navy::DestructorEvent::PutFailed);
          }
          recordEvent(AllocatorApiEvent::NVM_INSERT, key.key(), eventRes);
          putCleanup();
        },
        poolId, expiryTime, item.getLastAccessTime());

    if (status == navy::Status::Ok) {
      guard.dismiss();
      // mark it as NvmClean and unNvmEvicted if we put it into the queue
      // so handle destruction awares that there's a NVM copy (at least in the
      // queue)
      item.markNvmClean();
      item.unmarkNvmEvicted();
    } else {
      stats().numNvmPutErrs.inc();
    }
  });

  // if insertAsync is not executed or put into scheduler queue successfully,
  // NvmClean is not marked for the item, destructor of the item will be invoked
  // upon handle release.
  if (!executed) {
    stats().numNvmAbortedPutOnInflightGet.inc();
    recordEvent(AllocatorApiEvent::NVM_INSERT, item.getKey(),
                AllocatorApiResult::ABORTED);
  }
}

template <typename C>
template <typename F>
typename folly::Expected<typename NvmCache<C>::PutToken,
                         InFlightPuts::PutTokenError>
NvmCache<C>::createPutToken(folly::StringPiece key, F&& fn) {
  return inflightPuts_[getShardForKey(HashedKey{key})].tryAcquireToken(
      key, std::forward<F>(fn));
}

template <typename C>
void NvmCache<C>::onGetComplete(GetCtx& ctx,
                                navy::Status status,
                                HashedKey hk,
                                navy::BufferView val,
                                uint32_t lastAccessTimeSecs) {
  auto guard =
      folly::makeGuard([&ctx, hk]() { ctx.cache.removeFromFillMap(hk); });
  // navy got disabled while we were fetching. If so, safely return a miss.
  // If navy gets disabled beyond this point, it is okay since we fetched it
  // before we got disabled.
  if (!isEnabled()) {
    return;
  }

  if (status != navy::Status::Ok) {
    // instead of disabling navy, we enqueue a delete and return a miss.
    if (status != navy::Status::NotFound) {
      recordEvent(AllocatorApiEvent::NVM_FIND, hk.key(),
                  AllocatorApiResult::FAILED);
      remove(hk, createDeleteTombStone(hk));
    } else {
      recordEvent(AllocatorApiEvent::NVM_FIND, hk.key(),
                  AllocatorApiResult::NOT_FOUND);
    }
    stats().numNvmGetMiss.inc();
    return;
  }

  const NvmItem* nvmItem = reinterpret_cast<const NvmItem*>(val.data());

  // this item expired. return a miss.
  if (nvmItem->isExpired()) {
    stats().numNvmGetMiss.inc();
    stats().numNvmGetMissExpired.inc();
    WriteHandle hdl{};
    hdl.markExpired();
    hdl.markWentToNvm();
    ctx.setWriteHandle(std::move(hdl));
    recordEvent(AllocatorApiEvent::NVM_FIND, hk.key(),
                AllocatorApiResult::EXPIRED, nvmItem);
    return;
  }

  auto it = createItem(hk.key(), *nvmItem);
  if (!it) {
    stats().numNvmGetMiss.inc();
    stats().numNvmGetMissErrs.inc();
    // we failed to fill due to an internal failure. Return a miss and
    // invalidate what we have in nvmcache
    remove(hk, createDeleteTombStone(hk));
    recordEvent(AllocatorApiEvent::NVM_FIND, hk.key(),
                AllocatorApiResult::FAILED);
    return;
  }

  XDCHECK(it->isNvmClean());

  auto lock = getFillLockForShard(getShardForKey(hk));
  if (hasTombStone(hk) || !ctx.isValid()) {
    // a racing remove or evict while we were filling
    stats().numNvmGetMiss.inc();
    stats().numNvmGetMissDueToInflightRemove.inc();
    recordEvent(AllocatorApiEvent::NVM_FIND, hk.key(),
                AllocatorApiResult::NOT_FOUND);
    return;
  }

  recordEvent(AllocatorApiEvent::NVM_FIND, hk.key(), AllocatorApiResult::FOUND,
              nvmItem);

  // Get the latest last accessed time if it ATM is enabled and an entry exists.
  uint32_t latestLastAccessTimeSecs = lastAccessTimeSecs;
  if (accessTimeMap_ && it->isNvmLargeItem()) {
    auto atmEntry = accessTimeMap_->get(hk.keyHash());
    if (atmEntry.has_value()) {
      latestLastAccessTimeSecs =
          std::max(latestLastAccessTimeSecs, atmEntry.value());
    }
  }
  if (latestLastAccessTimeSecs > 0) {
    auto ttaSecs = util::getCurrentTimeSec() - latestLastAccessTimeSecs;
    stats().nvmHitTTASecs_.trackValue(ttaSecs);
    it.setTTASecs(static_cast<uint32_t>(ttaSecs));
  }

  // by the time we filled from navy, another thread inserted in RAM. We
  // disregard.
  if (CacheAPIWrapperForNvm<C>::insertFromNvm(cache_, it)) {
    it.markWentToNvm();
    ctx.setWriteHandle(std::move(it));
  }
} // namespace cachelib

template <typename C>
typename NvmCache<C>::WriteHandle NvmCache<C>::createItem(
    folly::StringPiece key, const NvmItem& nvmItem) {
  const size_t numBufs = nvmItem.getNumBlobs();
  // parent item
  XDCHECK_GE(numBufs, 1u);
  const auto pBlob = nvmItem.getBlob(0);

  stats().numNvmAllocAttempts.inc();
  // use the original alloc size to allocate, but make sure that the usable
  // size matches the pBlob's size
  auto it = CacheAPIWrapperForNvm<C>::allocateInternal(
      cache_, nvmItem.poolId(), key, pBlob.origAllocSize,
      nvmItem.getCreationTime(), nvmItem.getExpiryTime());

  if (!it) {
    return nullptr;
  }
  if (config_.makeObjCb) {
    for (int i = numBufs - 1; i >= 1; i--) {
      auto cBlob = nvmItem.getBlob(i);
      auto chainedIt = cache_.allocateChainedItem(it, cBlob.origAllocSize);
      if (!chainedIt) {
        return nullptr;
      }
      XDCHECK(chainedIt->isChainedItem());
      XDCHECK_LE(cBlob.data.size(), getStorageSizeInNvm(*chainedIt));
      cache_.addChainedItem(it, std::move(chainedIt));
      XDCHECK(it->hasChainedItem());
    }
    {
      util::LatencyTracker tracker(stats().nvmMakeObjCbLatency_);
      if (!config_.makeObjCb(
              nvmItem, *it,
              CacheAPIWrapperForNvm<C>::viewAsWritableChainedAllocsRange(
                  cache_, *it))) {
        return nullptr;
      }
    }
    // Often times an object needs its associated destructor to be triggered
    // to release resources (such as memory) properly. Today we use removeCB
    // or ItemDestructor to represent this logic for cachelib's object-cache.
    // Thus, we need to ensure these callbacks are always invoked when this
    // item goes away even if the item had never been inserted into cache (aka
    // not visible to other threads).
    it.unmarkNascent();
    it->markNvmClean();
    if (isNvmItemLarge(key, nvmItem)) {
      it->markNvmLargeItem();
    }
  } else {
    XDCHECK_LE(pBlob.data.size(), getStorageSizeInNvm(*it));
    XDCHECK_LE(pBlob.origAllocSize, pBlob.data.size());
    copyBlobData(it->getMemory(), pBlob.data.data(), pBlob.data.size());
    it->markNvmClean();
    if (isNvmItemLarge(key, nvmItem)) {
      it->markNvmLargeItem();
    }

    // if we have more, then we need to allocate them as chained items and add
    // them in the same order. To do that, we need to add them from the inverse
    // order
    if (numBufs > 1) {
      XDCHECK(!config_.makeObjCb);
      // chained items need to be added in reverse order to maintain the same
      // order as what we serialized.
      for (int i = numBufs - 1; i >= 1; i--) {
        auto cBlob = nvmItem.getBlob(i);
        XDCHECK_GT(cBlob.origAllocSize, 0u);
        XDCHECK_GT(cBlob.data.size(), 0u);
        stats().numNvmAllocAttempts.inc();
        auto chainedIt = cache_.allocateChainedItem(it, cBlob.origAllocSize);
        if (!chainedIt) {
          return nullptr;
        }
        XDCHECK(chainedIt->isChainedItem());
        XDCHECK_LE(cBlob.data.size(), getStorageSizeInNvm(*chainedIt));
        copyBlobData(chainedIt->getMemory(), cBlob.data.data(),
                     cBlob.data.size());
        cache_.addChainedItem(it, std::move(chainedIt));
        XDCHECK(it->hasChainedItem());
      }
    }
  }

  // issue the call back to decode and fix up the item if needed.
  if (config_.decodeCb) {
    config_.decodeCb(EncodeDecodeContext{
        *it, CacheAPIWrapperForNvm<C>::viewAsChainedAllocsRange(cache_, *it)});
  }
  return it;
}

template <typename C>
void NvmCache<C>::copyBlobData(void* dest, const void* src, size_t n) {
  if (copyOutOffload_ && n >= config_.copyOutOffloadMinSize) {
    if (navy::copyWithOffload(reinterpret_cast<uint8_t*>(dest),
                              reinterpret_cast<const uint8_t*>(src), n)) {
      stats().numNvmCopyOutOffloaded.inc();
      stats().numNvmCopyOutOffloadedBytes.add(n);
      return;
    }
    // copyWithOffload already completed the copy on the CPU.
    stats().numNvmCopyOutFallbacks.inc();
    return;
  }
  ::memcpy(dest, src, n);
}

template <typename C>
std::unique_ptr<folly::IOBuf> NvmCache<C>::createItemAsIOBuf(
    folly::StringPiece key, const NvmItem& nvmItem, bool parentOnly) {
  const size_t numBufs = parentOnly ? 1 : nvmItem.getNumBlobs();
  // Only use custom cb if we are not doing parent only.
  bool useCustomCb = !parentOnly && config_.makeObjCb;
  // parent item
  XDCHECK_GE(numBufs, 1u);
  const auto pBlob = nvmItem.getBlob(0);

  stats().numNvmAllocForItemDestructor.inc();
  std::unique_ptr<folly::IOBuf> head;

  try {
    // Use the pBlob's actual size instead of origAllocSize
    // because the slack space might be used if nvmcache is configured
    // with useTruncatedAllocSize == false
    XDCHECK_LE(pBlob.origAllocSize, pBlob.data.size());
    auto size = useCustomCb ? Item::getRequiredSize(key, pBlob.origAllocSize)
                            : Item::getRequiredSize(key, pBlob.data.size());

    head = folly::IOBuf::create(size);
    head->append(size);
  } catch (const std::bad_alloc&) {
    stats().numNvmItemDestructorAllocErrors.inc();
    return nullptr;
  }
  auto item = new (head->writableData())
      Item(key, pBlob.origAllocSize, nvmItem.getCreationTime(),
           nvmItem.getExpiryTime());

  XDCHECK_LE(pBlob.origAllocSize, item->getSize());
  XDCHECK_LE(pBlob.origAllocSize, pBlob.data.size());

  if (!useCustomCb) {
    copyBlobData(item->getMemory(), pBlob.data.data(), pBlob.data.size());
  }

  item->markNvmClean();
  item->markNvmEvicted();
  // if we have more, then we need to allocate them as chained items and add
  // them in the same order. To do that, we need to add them from the inverse
  // order.
  // We'll allocate the chained items and add to the chain first, then use the
  // customized callback to propagate their payload.
  if (numBufs > 1) {
    // chained items need to be added in reverse order to maintain the same
    // order as what we serialized.
    for (int i = numBufs - 1; i >= 1; i--) {
      auto cBlob = nvmItem.getBlob(i);
      XDCHECK_GT(cBlob.origAllocSize, 0u);
      XDCHECK_GT(cBlob.data.size(), 0u);
      stats().numNvmAllocForItemDestructor.inc();
      std::unique_ptr<folly::IOBuf> chained;
      try {
        auto size = ChainedItem::getRequiredSize(cBlob.origAllocSize);
        chained = folly::IOBuf::create(size);
        chained->append(size);
      } catch (const std::bad_alloc&) {
        stats().numNvmItemDestructorAllocErrors.inc();
        return nullptr;
      }
      auto chainedItem = new (chained->writableData())
          ChainedItem(typename C::CompressedPtrType(), cBlob.origAllocSize,
                      util::getCurrentTimeSec());
      XDCHECK(chainedItem->isChainedItem());
      // Propagate the payload directly from Blob only if no customized callback
      // is set.
      if (!useCustomCb) {
        copyBlobData(chainedItem->getMemory(), cBlob.data.data(),
                     cBlob.origAllocSize);
      }
      head->appendChain(std::move(chained));
      item->markHasChainedItem();
      XDCHECK(item->hasChainedItem());
    }
  }
  // If the customized callback is set, we'll call it to propagate the payload.
  if (useCustomCb) {
    util::LatencyTracker tracker(stats().nvmMakeObjCbLatency_);
    if (!config_.makeObjCb(nvmItem, *item,
                           viewAsWritableChainedAllocsRange(head.get()))) {
      return nullptr;
    }
  }

  return head;
}

template <typename C>
void NvmCache<C>::disableNavy(const std::string& msg) {
  if (isEnabled() && config_.disableNvmCacheOnBadState) {
    navyEnabled_ = false;
    XLOGF(CRITICAL, "Disabling navy. {}", msg);
  }
}

template <typename C>
void NvmCache<C>::remove(HashedKey hk, DeleteTombStoneGuard tombstone) {
  if (!isEnabled()) {
    return;
  }
  XDCHECK(tombstone);

  stats().numNvmDeletes.inc();

  util::LatencyTracker tracker(stats().nvmRemoveLatency_);
  const auto shard = getShardForKey(hk);
  //
  // invalidate any inflight put that is on flight since we are queueing up a
  // deletion.
  inflightPuts_[shard].invalidateToken(hk.key());

  // Skip scheduling async job to remove the key if the key couldn't exist,
  // if there are no put requests for the key shard.
  //
  // The existence check for skipping a remove to be enqueued is not going to
  // be changed by a get. It can be changed only by a concurrent put. And to
  // co-ordinate with that, we need to ensure that there are no put contexts
  // (in-flight puts) before we check for couldExist.  Any put contexts
  // created after couldExist api returns does not matter, since the put
  // token is invalidated before all of this begins.
  if (!putContexts_[shard].hasContexts() && !navyCache_->couldExist(hk)) {
    stats().numNvmSkippedDeletes.inc();
    return;
  }
  auto& delContexts = delContexts_[shard];
  auto& ctx = delContexts.createContext(hk.key(), std::move(tracker),
                                        std::move(tombstone));

  // capture array reference for delContext. it is stable
  auto delCleanup = [&delContexts, &ctx, this](navy::Status status,
                                               HashedKey) mutable {
    const auto result =
        status == navy::Status::Ok
            ? AllocatorApiResult::REMOVED
            : (status == navy::Status::NotFound ? AllocatorApiResult::NOT_FOUND
                                                : AllocatorApiResult::FAILED);
    recordEvent(AllocatorApiEvent::NVM_REMOVE, ctx.key(), result);
    delContexts.destroyContext(ctx);
    if (status == navy::Status::Ok || status == navy::Status::NotFound ||
        status == navy::Status::ChecksumError) {
      return;
    }
    // we set disable navy since we failed to delete something
    disableNavy(
        fmt::format("Delete Failure. status = {}", static_cast<int>(status)));
  };

  navyCache_->removeAsync(HashedKey::precomputed(ctx.key(), hk.keyHash()),
                          delCleanup);
}

template <typename C>
typename NvmCache<C>::SampleItem NvmCache<C>::getSampleItem() {
  navy::Buffer value;
  auto [status, keyStr] = navyCache_->getRandomAlloc(value);
  if (status != navy::Status::Ok || checkExpired_(value.view())) {
    return SampleItem{true /* fromNvm */};
  }

  folly::StringPiece key(keyStr);

  const auto& nvmItem = *reinterpret_cast<const NvmItem*>(value.data());
  const auto requiredSize =
      Item::getRequiredSize(key, nvmItem.getBlob(0).origAllocSize);

  const auto poolId = nvmItem.poolId();
  auto& pool = cache_.getPool(poolId);
  auto clsId = pool.getAllocationClassId(requiredSize);
  auto allocSize = pool.getAllocationClass(clsId).getAllocSize();

  std::shared_ptr<folly::IOBuf> iobufs =
      createItemAsIOBuf(key, nvmItem, true /* parentOnly */);
  if (!iobufs) {
    return SampleItem{true /* fromNvm */};
  }

  return SampleItem(std::move(*iobufs), poolId, clsId, allocSize,
                    true /* fromNvm */);
}

template <typename C>
bool NvmCache<C>::shutDown() {
  navyEnabled_ = false;
  try {
    this->flushPendingOps();

    // Persist ATM to shared memory before persisting navy cache
    if (accessTimeMap_ && shmManager_) {
      try {
        accessTimeMap_->persist(*shmManager_);
      } catch (const std::exception& e) {
        XLOGF(ERR, "Failed to persist AccessTimeMap: {}", e.what());
      }
    }

    const auto persistStart = util::getCurrentTimeMs();
    navyCache_->persist();
    lastPersistTimeMs_ = util::getCurrentTimeMs() - persistStart;
  } catch (const std::exception& e) {
    XLOG(ERR) << "Got error persisting cache: " << e.what();
    return false;
  }
  XLOG(INFO) << "Cache recovery saved to the Flash Device";
  return true;
}

template <typename C>
void NvmCache<C>::flushPendingOps() {
  navyCache_->flush();
}

template <typename C>
util::StatsMap NvmCache<C>::getStatsMap() const {
  util::StatsMap statsMap;
  navyCache_->getCounters(statsMap.createCountVisitor());
  if (accessTimeMap_) {
    accessTimeMap_->getCounters(statsMap.createCountVisitor());
  }
  statsMap.insertCount("items_tracked_for_destructor", getNvmItemRemovedSize());
  return statsMap;
}

template <typename C>
void NvmCache<C>::markNvmItemRemovedLocked(HashedKey hk) {
  if (itemDestructor_) {
    itemRemoved_[getShardForKey(hk)].insert(hk.key());
  }
}

template <typename C>
bool NvmCache<C>::checkAndUnmarkItemRemovedLocked(HashedKey hk) {
  auto& removedSet = itemRemoved_[getShardForKey(hk)];
  auto it = removedSet.find(hk.key());
  if (it != removedSet.end()) {
    removedSet.erase(it);
    return true;
  }
  return false;
}

template <typename C>
uint64_t NvmCache<C>::getNvmItemRemovedSize() const {
  uint64_t size = 0;
  for (size_t i = 0; i < numShards_; ++i) {
    auto lock = std::unique_lock{itemDestructorMutex_[i]};
    size += itemRemoved_[i].size();
  }
  return size;
}
} // namespace facebook::cachelib
