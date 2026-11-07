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
#include <folly/Range.h>
#include <folly/String.h>

#include <atomic>
#include <utility>

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/CacheChainedItemIterator.h"
#include "cachelib/allocator/Handle.h"
#include "cachelib/allocator/KAllocation.h"
#include "cachelib/allocator/Refcount.h"
#include "cachelib/allocator/TypedHandle.h"
#include "cachelib/allocator/datastruct/SList.h"
#include "cachelib/allocator/memory/CompressedPtr.h"
#include "cachelib/allocator/memory/MemoryAllocator.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Exceptions.h"
#include "cachelib/common/Mutex.h"

namespace facebook::cachelib {
namespace tests {
template <typename AllocatorT>
class BaseAllocatorTest;

template <typename AllocatorT>
class AllocatorHitStatsTest;

template <typename AllocatorT>
class MapTest;

class CacheAllocatorTestWrapper;
} // namespace tests

namespace interface {
class RAMCacheItem;
} // namespace interface

// forward declaration
template <typename CacheTrait>
class CacheAllocator;

template <typename CacheTrait>
class CacheChainedItem;

template <typename CacheTrait>
class ChainedItemPayload;

template <typename C>
class NvmCache;

template <typename Cache, typename Handle, typename Iter>
class CacheChainedAllocs;

template <typename K, typename V, typename C>
class Map;

// This is the actual representation of the cache item. It has two member
// hooks of type MMType::Hook and AccessType::Hook to ensure that the CacheItem
// can be put in the MMType::Container and AccessType::Container.
template <typename CacheTrait>
class CACHELIB_PACKED_ATTR CacheItem {
 public:
  /**
   * CacheAllocator is what the user will be interacting to cache
   * anything. NvmCache is an abstraction that abstracts away NVM
   * devices. It is abstracted inside CacheAllocator and user does
   * not deal with it directly. An item in NVM takes longer to fetch
   * than one that resides in RAM.
   */
  using CacheT = CacheAllocator<CacheTrait>;
  using NvmCacheT = NvmCache<CacheT>;
  using Flags = RefcountWithFlags::Flags;

  /**
   * Currently there are two types of items that can be cached.
   * A ChainedItem is a dependent item on a regular item. A chained
   * item does not have a real key but is instead associated with
   * a regular item (its parent item). Multiple chained items can be
   * linked to the same regular item and together they can cache data
   * much bigger that of a single item.
   */
  using Item = CacheItem<CacheTrait>;
  using ChainedItem = CacheChainedItem<CacheTrait>;

  /**
   * A cache item is roughly consisted of the following parts:
   *
   *  ---------------------
   *  | Intrusive Hooks   |
   *  | Reference & Flags |
   *  | Creation Time     |
   *  | Expiry Time       |
   *  | Payload           |
   *  ---------------------
   *
   * Intrusive hooks are used for access/mm containers. They contain
   * compressed pointers that link an item to said container in addition
   * to other metadata that the container itself deems useful to keep.
   *
   * Payload in this case is KAllocation which contains its own metadata
   * that describes the length of the payload, the size of the key in
   * addition to the actual key and the data.
   */
  using AccessHook = typename CacheTrait::AccessType::template Hook<Item>;
  using MMHook = typename CacheTrait::MMType::template Hook<Item>;
  using Key = KAllocation::Key;

  /**
   * User primarily interacts with an item through its handle.
   * An item handle is essentially a std::shared_ptr like structure
   * that ensures the item's refcount is properly maintained and ensures
   * the item is freed when it is not linked to access/mm containers
   * and its refcount drops to 0.
   */
  using ReadHandle = detail::ReadHandleImpl<CacheItem>;
  using WriteHandle = detail::WriteHandleImpl<CacheItem>;
  using Handle = WriteHandle;
  using HandleMaker = std::function<Handle(CacheItem*)>;

  /**
   * Item* and ChainedItem* are represented in this compressed form
   * inside the access and mm containers. They are more efficient to
   * store than raw pointers and can be leveraged to allow the cache
   * to be mapped to different addresses on shared memory.
   */
  using CompressedPtrType = typename CacheTrait::CompressedPtrType;
  using PtrCompressor =
      MemoryAllocator::PtrCompressorType<Item, CompressedPtrType>;

  // Get the required size for a cache item given the size of memory
  // user wants to allocate and the key size for the item
  //
  // @return required size if it's within the max size of uint32_t,
  //         0 otherwise
  static uint32_t getRequiredSize(Key key, uint32_t size) noexcept;

  // Same as above but explicitly passes in the key size.  Can be used in
  // checks and compile-time constants.
  static constexpr uint32_t getRequiredSize(uint32_t keySize,
                                            uint32_t size) noexcept;

  // Get the number of maximum outstanding handles there can be at any given
  // time for an item
  static uint64_t getRefcountMax() noexcept;

  // Copying or moving this can launch a nuclear missile and blow up everything
  CacheItem(const CacheItem&) = delete;
  CacheItem(CacheItem&&) = delete;
  void operator=(const CacheItem&) = delete;
  void operator=(CacheItem&&) = delete;

  // Fetch the key corresponding to the allocation
  const Key getKey() const noexcept;

  // Same as above but safe to call for unallocated data.  User must specify an
  // allocation size.
  const Key getKeyCheckedNoAsan(uint32_t allocSize) const noexcept;

  // Readonly memory for this allocation.
  const void* getMemory() const noexcept;

  // Writable memory for this allocation. The caller is free to do whatever he
  // wants with it and needs to ensure thread safety for access into this
  // piece of memory.
  void* getMemory() noexcept;

  // Cast item's readonly memory to a readonly user type
  template <typename T>
  const T* getMemoryAs() const noexcept {
    return reinterpret_cast<const T*>(getMemory());
  }

  // Cast item's writable memory to a writable user type
  template <typename T>
  T* getMemoryAs() noexcept {
    return reinterpret_cast<T*>(getMemory());
  }

  // This is the size of the memory allocation requested by the user.
  // The memory range [getMemory(), getMemory() + getSize()) is usable.
  uint32_t getSize() const noexcept;

  // This is the total memory used including header and user data
  uint32_t getTotalSize() const noexcept;

  // Return timestamp of when this item was created
  uint32_t getCreationTime() const noexcept;

  // return the original configured time to live in seconds.
  std::chrono::seconds getConfiguredTTL() const noexcept;

  // Return the last time someone accessed this item
  uint32_t getLastAccessTime() const noexcept;

  // Convenience method for debug purposes.
  std::string toString() const;

  // return the expiry time of the item
  uint32_t getExpiryTime() const noexcept;

  // check if the item reaches the expiry timestamp
  // expiryTime_ == 0 means no time limitation for this Item
  bool isExpired() const noexcept;

  // Check if the item is expired relative to the provided timestamp.
  bool isExpired(uint32_t currentTimeSecs) const noexcept;

  // Same as above but safe to call for unallocated data
  bool isExpiredNoAsan() const noexcept;
  bool isExpiredNoAsan(uint32_t currentTimeSec) const noexcept;

  /**
   * Access specific flags for an item
   *
   * These flags are set atomically and any of the APIs here will give a
   * consistent view on all the flags that are set or unset at that moment.
   *
   * However the content of the flag can be changed after any of these calls
   * are returned, so to reliably rely on them, the user needs to make sure
   * they're either the sole owner of this item or every one accessing this
   * item is only reading its content.
   */
  bool isChainedItem() const noexcept;
  bool hasChainedItem() const noexcept;

  /**
   * Keep track of whether the item was modified while in ram cache
   */
  bool isNvmClean() const noexcept;
  void markNvmClean() noexcept;
  void unmarkNvmClean() noexcept;

  /**
   * Marks that the item was potentially evicted from the nvmcache and might
   * need to be rewritten even if it was nvm-clean
   */
  void markNvmEvicted() noexcept;
  void unmarkNvmEvicted() noexcept;
  bool isNvmEvicted() const noexcept;

  /**
   * Track whether the item was routed to BlockCache (large item) in NVM.
   * Set during NVM promotion when the exact NVM buffer size is known.
   */
  void markNvmLargeItem() noexcept;
  bool isNvmLargeItem() const noexcept;

  /**
   * Function to set the timestamp for when to expire an item
   *
   * This API will only succeed when an item is a regular item, and user
   * has already inserted it into the cache (via @insert or @insertOrReplace).
   * In addition, the item cannot be in a "exclusive" state.
   *
   * @param expiryTime the expiryTime value to update to
   *
   * @return boolean indicating whether expiry time was successfully updated
   *         false when item is not linked in cache, or in exclusive state, or a
   *         chained item
   */
  bool updateExpiryTime(uint32_t expiryTimeSecs) noexcept;

  // Same as @updateExpiryTime, but sets expiry time to @ttl seconds from now.
  // It has the same restrictions as @updateExpiryTime. An item must be a
  // regular item and is part of the cache and NOT in the exclusive state.
  //
  // @param ttl   TTL (from now)
  // @return boolean indicating whether expiry time was successfully updated
  //         false when item is not linked in cache, or in exclusive state, or a
  //         chained item
  bool extendTTL(std::chrono::seconds ttl) noexcept;

  // Return the refcount of an item
  RefcountWithFlags::Value getRefCount() const noexcept;

  // Returns true if the item is in access container, false otherwise
  bool isAccessible() const noexcept;

  // Same as above but safe to call for unallocated data
  bool isAccessibleNoAsan() const noexcept;

 protected:
  // construct an item without expiry timestamp.
  CacheItem(Key key, uint32_t size, uint32_t creationTime);

  // @param key           Key for this item
  // @param size          Size allocated by the user. This may be smaller than
  //                      the full usable size
  // @param creationTime  Timestamp when this item was created
  // @param expiryTime    Timestamp when this item will be expired.
  CacheItem(Key key, uint32_t size, uint32_t creationTime, uint32_t expiryTime);

  // changes the item's key. This is only supported for ChainedItems. For
  // regular items, the key does not change with the lifetime of the item. For
  // ChainedItems since the key is the parent item, the key can change when
  // the parent item is being moved or tranferred.
  //
  // @throw std::invalid_argument if item is not a chained item or the key
  //        size does not match with the current key
  void changeKey(Key key);

  void* getMemoryInternal() const noexcept;

  /**
   * CacheItem's refcount contain admin references, access referneces, and
   * flags, refer to Refcount.h for details.
   *
   * Currently we support up to 2^18 references on any given item.
   * Increment and decrement may throw the following due to over/under-flow.
   *  cachelib::exception::RefcountOverflow
   *  cachelib::exception::RefcountUnderflow
   */
  RefcountWithFlags::Value getRefCountAndFlagsRaw() const noexcept;

  // Increments item's ref count
  //
  // @return true on success, failure if item is marked as exclusive
  // @throw exception::RefcountOverflow on ref count overflow
  FOLLY_ALWAYS_INLINE RefcountWithFlags::IncResult incRef() {
    try {
      return ref_.incRef();
    } catch (exception::RefcountOverflow& e) {
      throw exception::RefcountOverflow(
          fmt::format("{} item: {}", e.what(), toString()));
    }
  }

  FOLLY_ALWAYS_INLINE RefcountWithFlags::Value decRef() {
    return ref_.decRef();
  }

  // Whether or not an item is completely drained of all references including
  // the internal ones. This means there is no access refcount bits and zero
  // admin bits set. I.e. refcount is 0 and the item is not linked, accessible,
  // nor exclusive
  bool isDrained() const noexcept;

  /**
   * The following three functions correspond to the state of the allocation
   * in the memory management container. This is protected by the
   * MMContainer's internal locking. Inspecting this outside the mm
   * container will be racy.
   */
  void markInMMContainer() noexcept;
  void unmarkInMMContainer() noexcept;
  bool isInMMContainer() const noexcept;

  /**
   * The following three functions correspond to the state of the allocation
   * in the access container. This will be protected by the access container
   * lock. Depending on their state outside of the access container might be
   * racy
   */
  void markAccessible() noexcept;
  void unmarkAccessible() noexcept;

  /**
   * The following two functions corresond to whether or not an item is
   * currently in the process of being evicted.
   *
   * An item can only be marked exclusive when `isInMMContainer` returns true
   * and item is not already exclusive nor moving and the ref count is 0.
   * This operation is atomic.
   *
   * Unmarking exclusive does not depend on `isInMMContainer`
   * Unmarking exclusive will also return the refcount at the moment of
   * unmarking.
   */
  MarkForEvictionResult markForEviction() noexcept;
  RefcountWithFlags::Value unmarkForEviction() noexcept;
  bool isMarkedForEviction() const noexcept;

  /**
   * The following functions correspond to whether or not an item is
   * currently in the processed of being moved. When moving, ref count
   * is always == 1.
   *
   * An item can only be marked moving when `isInMMContainer` returns true
   * and item is not already exclusive nor moving.
   *
   * Unmarking moving does not depend on `isInMMContainer`
   * Unmarking moving will also return the refcount at the moment of
   * unmarking.
   */
  bool markMoving();
  RefcountWithFlags::Value unmarkMoving() noexcept;
  bool isMoving() const noexcept;

  /** This function attempts to mark item as exclusive.
   * Can only be called on the item that is moving.*/
  bool markForEvictionWhenMoving();

  /**
   * Item cannot be marked both chained allocation and
   * marked as having chained allocations at the same time
   */
  void markIsChainedItem() noexcept;
  void unmarkIsChainedItem() noexcept;
  void markHasChainedItem() noexcept;
  void unmarkHasChainedItem() noexcept;
  ChainedItem& asChainedItem() noexcept;
  const ChainedItem& asChainedItem() const noexcept;

  // Returns the offset of the beginning of usable memory for an item
  uint32_t getOffsetForMemory() const noexcept;

  /**
   * Functions to set, unset and get bits
   */
  template <RefcountWithFlags::Flags flagBit>
  void setFlag() noexcept;
  template <RefcountWithFlags::Flags flagBit>
  void unSetFlag() noexcept;
  template <RefcountWithFlags::Flags flagBit>
  bool isFlagSet() const noexcept;

  /**
   * The following are the data members of CacheItem
   *
   * Hooks to access and mm containers are public since other parts of the
   * code need access to them. Everything else should be private.
   */
 public:
  // Hook for the access type
  AccessHook accessHook_;

  using AccessContainer = typename CacheTrait::AccessType::template Container<
      Item,
      &Item::accessHook_,
      typename CacheTrait::AccessTypeLocks>;

  // Hook for the mm type
  MMHook mmHook_;

  using MMContainer =
      typename CacheTrait::MMType::template Container<Item, &Item::mmHook_>;

 protected:
  // Refcount for the item and also flags on the items state
  RefcountWithFlags ref_;

  // Time when this cache item is created
  const uint32_t creationTime_{0};

  // Expiry timestamp for the item
  // 0 means no time limitation
  uint32_t expiryTime_{0};

  // The actual allocation.
  KAllocation alloc_;

  friend ChainedItem;
  friend CacheT;
  friend AccessContainer;
  friend MMContainer;
  friend NvmCacheT;
  template <typename Cache, typename Handle, typename Iter>
  friend class CacheChainedAllocs;
  template <typename Cache, typename Item>
  friend class CacheChainedItemIterator;
  friend class facebook::cachelib::tests::CacheAllocatorTestWrapper;
  template <typename K, typename V, typename C>
  friend class Map;

  // interface
  friend class interface::RAMCacheItem;

  // tests
  template <typename AllocatorT>
  friend class facebook::cachelib::tests::BaseAllocatorTest;
  friend class facebook::cachelib::tests::MapTest<CacheAllocator<CacheTrait>>;
  FRIEND_TEST(LruAllocatorTest, ItemSampling);
  FRIEND_TEST(LruAllocatorTest, AddChainedAllocationSimple);
  FRIEND_TEST(ItemTest, ChangeKey);
  FRIEND_TEST(ItemTest, ToString);
  FRIEND_TEST(ItemTest, CreationTime);
  FRIEND_TEST(ItemTest, ExpiryTime);
  FRIEND_TEST(ItemTest, ChainedItemConstruction);
  FRIEND_TEST(ItemTest, NonStringKey);
  template <typename AllocatorT>
  friend class facebook::cachelib::tests::AllocatorHitStatsTest;
};

// A chained item has a hook pointing to the next chained item. The hook is
// a compressed pointer stored at the beginning of KAllocation's data.
// A chained item's key is a compressed pointer to its parent item.
//
// Memory layout:
// | --------------------- |
// | AccessHook            |
// | MMHook                |
// | RefCountWithFlags     |
// | creationTime_         |
// | --------------------- |
// |  K | size_            |
// |  A | ---------------- |
// |  l |       | keyData  | <-- sizeof(CompressedPtrType)
// |  l |       | -------- |
// |  o |       | P | hook | <-- sizeof(SlistHook<ChainedItem>)
// |  c | data_ | a | data |
// |  a |       | y |      |
// |  t |       | l |      |
// |  i |       | o |      |
// |  o |       | a |      |
// |  n |       | d |      |
// | --------------------- |
template <typename CacheTrait>
class CACHELIB_PACKED_ATTR CacheChainedItem : public CacheItem<CacheTrait> {
 public:
  using Item = CacheItem<CacheTrait>;
  using ChainedItem = CacheChainedItem<CacheTrait>;
  using Payload = ChainedItemPayload<CacheTrait>;
  using CompressedPtrType = typename Item::CompressedPtrType;
  using PtrCompressor = typename Item::PtrCompressor;

  /**
   * Key for CacheChainedItem is the raw pointer to the parent item,
   * so it is 8 bytes big.
   */
  using Key = typename Item::Key;
  static constexpr uint32_t kKeySize = sizeof(CompressedPtrType);

  // Get the required size for a cache item given the size of memory
  // user wants to allocate
  //
  // @return required size if it's within the max size of uint32_t,
  //         0 otherwise
  static uint32_t getRequiredSize(uint32_t size) noexcept;

  // Get the parent item this chained allocation is associated with
  Item& getParentItem(const PtrCompressor& compressor) const noexcept;

  // Usable memory for this allocation. The caller is free to do whatever he
  // wants with it and needs to ensure concurrency for access into this
  // piece of memory.
  void* getMemory() const noexcept;

  // This is the size of the memory allocation requested by the user.
  // The memory range [getMemory(), getMemory() + getSize()) is usable.
  uint32_t getSize() const noexcept;

  // Convenience method for debug purposes.
  std::string toString() const;

 protected:
  // The key of a chained allocation is the address of its parent item
  //
  // @param ptr         Compressed ptr to parent item
  // @param allocSize   This is the size of the entire allocation for
  //                    constructing this item
  // @param creationTime  Timestamp when this item was created
  CacheChainedItem(CompressedPtrType key, uint32_t size, uint32_t creationTime);

  // reset the key of the ChainedItem. For regular Items, we dont allow doing
  // this. However for chained items since the parent is the key, we need to
  // allow this for transferring the ownership from one parent to another.
  //
  // @throw std::invalid_argument if the chained item is still in accessible
  //        state.
  void changeKey(CompressedPtrType newKey);

  // Append chain to this item. The new chain can contain one or more items
  // but this item to which the new chain is being appended must be a single
  // item, or the last item of an existing chain
  //
  // @throw std::invalid_argument  if this item is already part of a chain
  //                               and is not the last item
  void appendChain(ChainedItem& newChain, const PtrCompressor& compressor);

  // get the next in the chain for this chained item.
  ChainedItem* getNext(const PtrCompressor& compressor) const noexcept;

  // set the next in chain for this chained Item
  void setNext(const ChainedItem* next,
               const PtrCompressor& compressor) noexcept;

  Payload& getPayload();
  const Payload& getPayload() const;

  friend Payload;
  friend CacheAllocator<CacheTrait>;
  template <typename Cache, typename Handle, typename Iter>
  friend class CacheChainedAllocs;
  template <typename Cache, typename Item>
  friend class CacheChainedItemIterator;
  friend NvmCache<CacheAllocator<CacheTrait>>;
  template <typename AllocatorT>
  friend class facebook::cachelib::tests::BaseAllocatorTest;
  FRIEND_TEST(ItemTest, ChainedItemConstruction);
  FRIEND_TEST(ItemTest, ToString);
  FRIEND_TEST(ItemTest, ChangeKey);
};

template <typename CacheTrait>
uint32_t CacheItem<CacheTrait>::getRequiredSize(Key key,
                                                uint32_t size) noexcept {
  const uint64_t requiredSize = static_cast<uint64_t>(size) + key.size() +
                                sizeof(Item) +
                                KAllocation::extraBytesForLargeKeys(key.size());

  XDCHECK_LE(requiredSize,
             static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
  if (requiredSize >
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return 0;
  }
  return static_cast<uint32_t>(requiredSize);
}

template <typename CacheTrait>
constexpr uint32_t CacheItem<CacheTrait>::getRequiredSize(
    uint32_t keySize, uint32_t size) noexcept {
  const uint64_t requiredSize = static_cast<uint64_t>(size) + keySize +
                                sizeof(Item) +
                                KAllocation::extraBytesForLargeKeys(keySize);
  if (requiredSize >
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return 0;
  }
  return static_cast<uint32_t>(requiredSize);
}

template <typename CacheTrait>
uint64_t CacheItem<CacheTrait>::getRefcountMax() noexcept {
  return RefcountWithFlags::kAccessRefMask;
}

template <typename CacheTrait>
CacheItem<CacheTrait>::CacheItem(Key key,
                                 uint32_t size,
                                 uint32_t creationTime,
                                 uint32_t expiryTime)
    : creationTime_(creationTime), expiryTime_(expiryTime), alloc_(key, size) {}

template <typename CacheTrait>
CacheItem<CacheTrait>::CacheItem(Key key, uint32_t size, uint32_t creationTime)
    : CacheItem(key, size, creationTime, 0 /* expiryTime_ */) {}

template <typename CacheTrait>
const typename CacheItem<CacheTrait>::Key CacheItem<CacheTrait>::getKey()
    const noexcept {
  return alloc_.getKey();
}

template <typename CacheTrait>
const typename CacheItem<CacheTrait>::Key
CacheItem<CacheTrait>::getKeyCheckedNoAsan(uint32_t allocSize) const noexcept {
  return alloc_.getKeyCheckedNoAsan(allocSize);
}

template <typename CacheTrait>
const void* CacheItem<CacheTrait>::getMemory() const noexcept {
  return getMemoryInternal();
}

template <typename CacheTrait>
void* CacheItem<CacheTrait>::getMemory() noexcept {
  return getMemoryInternal();
}

template <typename CacheTrait>
void* CacheItem<CacheTrait>::getMemoryInternal() const noexcept {
  if (isChainedItem()) {
    return asChainedItem().getMemory();
  } else {
    return alloc_.getMemory();
  }
}

template <typename CacheTrait>
uint32_t CacheItem<CacheTrait>::getOffsetForMemory() const noexcept {
  return reinterpret_cast<uintptr_t>(getMemory()) -
         reinterpret_cast<uintptr_t>(this);
}

template <typename CacheTrait>
uint32_t CacheItem<CacheTrait>::getSize() const noexcept {
  if (isChainedItem()) {
    return asChainedItem().getSize();
  } else {
    return alloc_.getSize();
  }
}

template <typename CacheTrait>
uint32_t CacheItem<CacheTrait>::getTotalSize() const noexcept {
  auto headerSize = reinterpret_cast<uintptr_t>(getMemory()) -
                    reinterpret_cast<uintptr_t>(this);
  return headerSize + getSize();
}

template <typename CacheTrait>
uint32_t CacheItem<CacheTrait>::getExpiryTime() const noexcept {
  return expiryTime_;
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::isExpired() const noexcept {
  return util::isExpired(expiryTime_);
}

template <typename CacheTrait>
FOLLY_DISABLE_ADDRESS_SANITIZER bool CacheItem<CacheTrait>::isExpiredNoAsan()
    const noexcept {
  return util::isExpired(expiryTime_);
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::isExpired(uint32_t currentTimeSec) const noexcept {
  return (expiryTime_ > 0 && expiryTime_ < currentTimeSec);
}

template <typename CacheTrait>
FOLLY_DISABLE_ADDRESS_SANITIZER bool CacheItem<CacheTrait>::isExpiredNoAsan(
    uint32_t currentTimeSec) const noexcept {
  return (expiryTime_ > 0 && expiryTime_ < currentTimeSec);
}

template <typename CacheTrait>
uint32_t CacheItem<CacheTrait>::getCreationTime() const noexcept {
  return creationTime_;
}

template <typename CacheTrait>
std::chrono::seconds CacheItem<CacheTrait>::getConfiguredTTL() const noexcept {
  return std::chrono::seconds(expiryTime_ > 0 ? expiryTime_ - creationTime_
                                              : 0);
}

template <typename CacheTrait>
uint32_t CacheItem<CacheTrait>::getLastAccessTime() const noexcept {
  return mmHook_.getUpdateTime();
}

template <typename CacheTrait>
std::string CacheItem<CacheTrait>::toString() const {
  if (isChainedItem()) {
    return asChainedItem().toString();
  } else {
    return fmt::format(
        "item: "
        "memory={}:raw-ref={}:size={}:key={}:hex-key={}:"
        "isInMMContainer={}:isAccessible={}:isMarkedForEviction={}:"
        "isMoving={}:references={}:ctime="
        "{}:"
        "expTime={}:updateTime={}:isNvmClean={}:isNvmEvicted={}:"
        "isNvmLargeItem={}:hasChainedItem="
        "{}",
        fmt::ptr(this), getRefCountAndFlagsRaw(), getSize(),
        folly::humanify(getKey().str()),
        folly::hexlify(folly::ByteRange{getKey()}), isInMMContainer(),
        isAccessible(), isMarkedForEviction(), isMoving(), getRefCount(),
        getCreationTime(), getExpiryTime(), getLastAccessTime(), isNvmClean(),
        isNvmEvicted(), isNvmLargeItem(), hasChainedItem());
  }
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::changeKey(Key key) {
  if (!isChainedItem()) {
    throw std::invalid_argument("Item is not chained type");
  }

  alloc_.changeKey(key);
  XDCHECK_EQ(key, getKey());
}

template <typename CacheTrait>
RefcountWithFlags::Value CacheItem<CacheTrait>::getRefCount() const noexcept {
  return ref_.getAccessRef();
}

template <typename CacheTrait>
RefcountWithFlags::Value CacheItem<CacheTrait>::getRefCountAndFlagsRaw()
    const noexcept {
  return ref_.getRaw();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::isDrained() const noexcept {
  return ref_.isDrained();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::markAccessible() noexcept {
  ref_.markAccessible();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::unmarkAccessible() noexcept {
  ref_.unmarkAccessible();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::markInMMContainer() noexcept {
  ref_.markInMMContainer();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::unmarkInMMContainer() noexcept {
  ref_.unmarkInMMContainer();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::isAccessible() const noexcept {
  return ref_.isAccessible();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::isAccessibleNoAsan() const noexcept {
  return ref_.isAccessibleNoAsan();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::isInMMContainer() const noexcept {
  return ref_.isInMMContainer();
}

template <typename CacheTrait>
MarkForEvictionResult CacheItem<CacheTrait>::markForEviction() noexcept {
  return ref_.markForEviction();
}

template <typename CacheTrait>
RefcountWithFlags::Value CacheItem<CacheTrait>::unmarkForEviction() noexcept {
  return ref_.unmarkForEviction();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::isMarkedForEviction() const noexcept {
  return ref_.isMarkedForEviction();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::markForEvictionWhenMoving() {
  return ref_.markForEvictionWhenMoving();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::markMoving() {
  return ref_.markMoving();
}

template <typename CacheTrait>
RefcountWithFlags::Value CacheItem<CacheTrait>::unmarkMoving() noexcept {
  return ref_.unmarkMoving();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::isMoving() const noexcept {
  return ref_.isMoving();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::markNvmClean() noexcept {
  ref_.markNvmClean();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::unmarkNvmClean() noexcept {
  ref_.unmarkNvmClean();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::isNvmClean() const noexcept {
  return ref_.isNvmClean();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::markNvmEvicted() noexcept {
  ref_.markNvmEvicted();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::unmarkNvmEvicted() noexcept {
  ref_.unmarkNvmEvicted();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::isNvmEvicted() const noexcept {
  return ref_.isNvmEvicted();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::markNvmLargeItem() noexcept {
  ref_.markNvmLargeItem();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::isNvmLargeItem() const noexcept {
  return ref_.isNvmLargeItem();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::markIsChainedItem() noexcept {
  XDCHECK(!hasChainedItem());
  ref_.markIsChainedItem();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::unmarkIsChainedItem() noexcept {
  XDCHECK(!hasChainedItem());
  ref_.unmarkIsChainedItem();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::markHasChainedItem() noexcept {
  XDCHECK(!isChainedItem());
  ref_.markHasChainedItem();
}

template <typename CacheTrait>
void CacheItem<CacheTrait>::unmarkHasChainedItem() noexcept {
  XDCHECK(!isChainedItem());
  ref_.unmarkHasChainedItem();
}

template <typename CacheTrait>
typename CacheItem<CacheTrait>::ChainedItem&
CacheItem<CacheTrait>::asChainedItem() noexcept {
  return *static_cast<ChainedItem*>(this);
}

template <typename CacheTrait>
const typename CacheItem<CacheTrait>::ChainedItem&
CacheItem<CacheTrait>::asChainedItem() const noexcept {
  return *static_cast<const ChainedItem*>(this);
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::isChainedItem() const noexcept {
  return ref_.isChainedItem();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::hasChainedItem() const noexcept {
  return ref_.hasChainedItem();
}

template <typename CacheTrait>
template <typename RefcountWithFlags::Flags flagBit>
void CacheItem<CacheTrait>::setFlag() noexcept {
  ref_.template setFlag<flagBit>();
}

template <typename CacheTrait>
template <typename RefcountWithFlags::Flags flagBit>
void CacheItem<CacheTrait>::unSetFlag() noexcept {
  ref_.template unSetFlag<flagBit>();
}

template <typename CacheTrait>
template <typename RefcountWithFlags::Flags flagBit>
bool CacheItem<CacheTrait>::isFlagSet() const noexcept {
  return ref_.template isFlagSet<flagBit>();
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::updateExpiryTime(uint32_t expiryTimeSecs) noexcept {
  // check for moving to make sure we are not updating the expiry time while at
  // the same time re-allocating the item with the old state of the expiry time
  // in moveRegularItem(). See D6852328
  if (isMoving() || isMarkedForEviction() || !isInMMContainer() ||
      isChainedItem()) {
    return false;
  }
  // attempt to atomically update the value of expiryTime
  while (true) {
    uint32_t currExpTime = expiryTime_;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Watomic-alignment"
    if (__atomic_compare_exchange_n(&expiryTime_, &currExpTime, expiryTimeSecs,
                                    false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST)) {
#pragma clang diagnostic pop
      return true;
    }
  }
}

template <typename CacheTrait>
bool CacheItem<CacheTrait>::extendTTL(std::chrono::seconds ttl) noexcept {
  return updateExpiryTime(util::getCurrentTimeSec() + ttl.count());
}

// Chained items are chained in a single linked list. The payload of each
// chained item is chained to the next item.
template <typename CacheTrait>
class CACHELIB_PACKED_ATTR ChainedItemPayload {
 public:
  using ChainedItem = CacheChainedItem<CacheTrait>;
  using Item = CacheItem<CacheTrait>;
  using PtrCompressor = typename ChainedItem::PtrCompressor;

  // Pointer to the next chained allocation. initialize to nullptr.
  SListHook<Item> hook_{};

  // Payload
  mutable unsigned char data_[0];

  // Usable memory for this allocation. The caller is free to do whatever he
  // wants with it and needs to ensure concurrency for access into this
  // piece of memory.
  void* getMemory() const noexcept { return &data_; }

  ChainedItem* getNext(const PtrCompressor& compressor) const noexcept {
    return static_cast<ChainedItem*>(hook_.getNext(compressor));
  }

  void setNext(const ChainedItem* next,
               const PtrCompressor& compressor) noexcept {
    hook_.setNext(static_cast<const Item*>(next), compressor);
    XDCHECK_EQ(reinterpret_cast<uintptr_t>(getNext(compressor)),
               reinterpret_cast<uintptr_t>(next));
  }
};

template <typename CacheTrait>
uint32_t CacheChainedItem<CacheTrait>::getRequiredSize(uint32_t size) noexcept {
  const uint64_t requiredSize = static_cast<uint64_t>(size) +
                                static_cast<uint64_t>(kKeySize) + sizeof(Item) +
                                sizeof(Payload);
  XDCHECK_LE(requiredSize,
             static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
  if (requiredSize >
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return 0;
  }
  return static_cast<uint32_t>(requiredSize);
}

template <typename CacheTrait>
CacheChainedItem<CacheTrait>::CacheChainedItem(CompressedPtrType ptr,
                                               uint32_t size,
                                               uint32_t creationTime)
    : Item(Key{reinterpret_cast<const char*>(&ptr), kKeySize},
           size + sizeof(Payload),
           creationTime) {
  this->markIsChainedItem();

  // Explicitly call ChainedItemPayload's ctor to initialize it properly, since
  // ChainedItemPayload is not a member of CacheChainedItem.
  new (reinterpret_cast<void*>(&getPayload())) Payload();
}

template <typename CacheTrait>
void CacheChainedItem<CacheTrait>::changeKey(CompressedPtrType newKey) {
  if (this->isAccessible()) {
    throw std::invalid_argument(fmt::format(
        "chained item {} is still in access container while modifying the key ",
        toString()));
  }
  Item::changeKey(Key{reinterpret_cast<const char*>(&newKey), kKeySize});
}

template <typename CacheTrait>
typename CacheChainedItem<CacheTrait>::Item&
CacheChainedItem<CacheTrait>::getParentItem(
    const PtrCompressor& compressor) const noexcept {
  return *compressor.unCompress(
      *reinterpret_cast<const CompressedPtrType*>(this->getKey().begin()));
}

template <typename CacheTrait>
void* CacheChainedItem<CacheTrait>::getMemory() const noexcept {
  return getPayload().getMemory();
}

template <typename CacheTrait>
uint32_t CacheChainedItem<CacheTrait>::getSize() const noexcept {
  // Chained Item has its own embedded payload in its KAllocation, so we
  // need to deduct its size here to give the user the accurate usable size
  return this->alloc_.getSize() - sizeof(Payload);
}

template <typename CacheTrait>
std::string CacheChainedItem<CacheTrait>::toString() const {
  const auto cPtr =
      *reinterpret_cast<const CompressedPtrType*>(Item::getKey().data());
  return fmt::format(
      "chained item: "
      "memory={}:raw-ref={}:size={}:parent-compressed-ptr={}:"
      "isInMMContainer={}:isAccessible={}:isMarkedForEviction={}:"
      "isMoving={}:references={}:ctime={}"
      ":"
      "expTime={}:updateTime={}",
      fmt::ptr(this), Item::getRefCountAndFlagsRaw(), Item::getSize(),
      cPtr.getRaw(), Item::isInMMContainer(), Item::isAccessible(),
      Item::isMarkedForEviction(), Item::isMoving(), Item::getRefCount(),
      Item::getCreationTime(), Item::getExpiryTime(),
      Item::getLastAccessTime());
}

template <typename CacheTrait>
void CacheChainedItem<CacheTrait>::appendChain(
    ChainedItem& newChain, const PtrCompressor& compressor) {
  if (getNext(compressor)) {
    throw std::invalid_argument(
        fmt::format("Item: {} is not the last item in a chain. Next: {}",
                    toString(), getNext(compressor)->toString()));
  }
  setNext(&newChain, compressor);
}

template <typename CacheTrait>
typename CacheChainedItem<CacheTrait>::ChainedItem*
CacheChainedItem<CacheTrait>::getNext(
    const PtrCompressor& compressor) const noexcept {
  return getPayload().getNext(compressor);
}

template <typename CacheTrait>
void CacheChainedItem<CacheTrait>::setNext(
    const ChainedItem* next, const PtrCompressor& compressor) noexcept {
  getPayload().setNext(next, compressor);
  XDCHECK_EQ(reinterpret_cast<uintptr_t>(getNext(compressor)),
             reinterpret_cast<uintptr_t>(next));
}

template <typename CacheTrait>
typename CacheChainedItem<CacheTrait>::Payload&
CacheChainedItem<CacheTrait>::getPayload() {
  return *reinterpret_cast<Payload*>(this->alloc_.getMemory());
}

template <typename CacheTrait>
const typename CacheChainedItem<CacheTrait>::Payload&
CacheChainedItem<CacheTrait>::getPayload() const {
  return *reinterpret_cast<const Payload*>(this->alloc_.getMemory());
}
} // namespace facebook::cachelib
