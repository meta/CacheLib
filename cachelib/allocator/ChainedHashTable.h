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
#include <folly/Optional.h>
#include <folly/synchronization/SanitizeThread.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"
#include "cachelib/common/Throttler.h"
#include "cachelib/shm/Shm.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <folly/Range.h>
#pragma GCC diagnostic pop

namespace facebook::cachelib {

/**
 * Implementation of a hash table with chaining. The elements of the hash
 * table need to have a public member of type Hook . Expects T to provide a
 * getKey(), getHash<Hasher>() and appropriate key comparison operators for
 * doing the key comparisons. The hashtable container guarantees thread
 * safety. The container acts as an intrusive member-hook hashtable.
 */
class ChainedHashTable {
 public:
  // unique identifier per AccessType
  static const int kId;

  template <typename T>
  struct Hook;

 private:
  // Implements a hash table with chaining.
  template <typename T, Hook<T> T::* HookPtr>
  class Impl {
   public:
    using Key = typename T::Key;
    using BucketId = size_t;
    using CompressedPtrType = typename T::CompressedPtrType;
    using PtrCompressor = typename T::PtrCompressor;

    // allocate memory for hash table; the memory is managed by Impl.
    //
    // @param numBuckets    the number of buckets to be allocated, power of two
    // @param compressor    object used to compress/decompress node pointers
    // @param hasher        object used to hash the key for its bucket id
    // @param pageSize      when a supported huge-page size (bytes), back the
    //                      bucket array with anonymous HugeTLB pages; default
    //                      => new[]
    Impl(size_t numBuckets,
         const PtrCompressor& compressor,
         const Hasher& hasher,
         PageSize pageSize = PageSize());

    // allocate memory for hash table; the memory is managed by the user.
    //
    // @param numBuckets    the number of buckets to be allocated, power of two
    // @param memStart      user managed memory. The size must be enough to
    //                      accommodate the number of the buckets
    // @param compressor    object used to compress/decompress node pointers
    // @param hasher        object used to hash the key for its bucket id
    // @param resetMem      fill memory with CompressedPtrType{}
    Impl(size_t numBuckets,
         void* memStart,
         const PtrCompressor& compressor,
         const Hasher& hasher,
         bool resetMem = false);

    // hash table memory is not released if managed by user.
    // i.e. Impl::isRestorable() == true
    ~Impl();

    // prohibit copying
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    T* getHashNext(const T& node) const noexcept {
      return (node.*HookPtr).getHashNext(compressor_);
    }

    CompressedPtrType getHashNextCompressed(const T& node) const noexcept {
      return (node.*HookPtr).getHashNext();
    }

    void setHashNext(T& node, T* next) const noexcept {
      (node.*HookPtr).setHashNext(next, compressor_);
    }

    void setHashNext(T& node, CompressedPtrType next) {
      (node.*HookPtr).setHashNext(next);
    }

    // inserts the element into the bucket.
    //
    // @param node    node to be inserted into the hashtable
    // @param bucket  the hashtable bucket that the node belongs to
    // @return  True if the insertion was success. False if not. Insertion
    //          fails if there is already a node with similar key in the
    //          hashtable.
    bool insertInBucket(T& node, BucketId bucket) noexcept;

    // inserts or replaces the element into the bucket.
    //
    // @param node    node to be inserted into the hashtable
    // @param bucket  the hashtable bucket that the node belongs to
    // @return  old node if it exists, nullptr otherwise
    T* insertOrReplaceInBucket(T& node, BucketId bucket) noexcept;

    // removes the node from the bucket.
    //
    // precondition:  node must be in the bucket.
    // @param node    the node to be removed.
    // @param bucket  the hashtable bucket that the node belongs to
    void removeFromBucket(T& node, BucketId bucket) noexcept;

    // finds the node corresponding to the key from the bucket and returns it
    // if found.
    //
    // @param key     the key for the node we are looking for.
    // @param bucket  the hashtable bucket that the key belongs to
    // @return  a T* corresponding to the node or nullptr if there is no such
    //          node with the key in the bucket.
    T* findInBucket(Key key, BucketId bucket) const noexcept;

    // gets the bucket for the key by using the corresponding hash function.
    BucketId getBucket(Key k) const noexcept;

    // Prefetches the bucket slot. Batched lookups call this for every key
    // first so that the slot cache misses of the whole batch overlap.
    void prefetchBucket(BucketId bucket) const noexcept {
      __builtin_prefetch(hashTable_.get() + bucket);
    }

    // Prefetches the first node of the bucket's chain. The slot is read without
    // the bucket lock, so the pointer may be stale and is only prefetched.
    // Only for 4-byte pointers, as a wider read could tear; the table is
    // allocated or mapped with at least 4-byte alignment.
    void prefetchBucketHead([[maybe_unused]] BucketId bucket) const noexcept {
      if constexpr (sizeof(CompressedPtrType) == sizeof(uint32_t)) {
        uint32_t raw;
        {
          folly::annotate_ignore_thread_sanitizer_guard g(__FILE__, __LINE__);
          raw = __atomic_load_n(
              reinterpret_cast<const uint32_t*>(hashTable_.get() + bucket),
              __ATOMIC_RELAXED);
        }
        compressor_.prefetch(CompressedPtrType{
            static_cast<typename CompressedPtrType::SerializedPtrType>(raw)});
      }
    }

    // Call 'func' on each element in the given bucket.
    //
    // @param bucket  the bucket id to fetch.
    template <typename F>
    void forEachBucketElem(BucketId bucket, F&& func) const;

    // fetch the number of elements of a given bucket
    //
    // @param bucket  the bucket id to fetch.
    unsigned int getBucketNumElems(BucketId bucket) const;

    // true if the hash table can be restored
    bool isRestorable() const noexcept { return restorable_; }

    // return the hashtable size in bytes
    size_t getMemorySize() const noexcept {
      return mmapBytes_ == 0 ? numBuckets_ * sizeof(CompressedPtrType)
                             : mmapBytes_;
    }

    const void* getBucketMemory() const noexcept { return hashTable_.get(); }

    // return the number of buckets in hash table
    size_t getNumBuckets() const noexcept { return numBuckets_; }

   private:
    // finds the previous node in the hash chain for this node if one exists
    // such that prev->next is node.
    //
    // @param node    the node for which we are looking for the previous
    // @param bucket  the hashtable bucket that the node belongs to
    // @return  previous node for this node in the hash chain or nullptr if
    //          this node is in the head of the hash chain.
    T* findPrevInBucket(const T& node, BucketId bucket) const noexcept;

    // number of buckets we have in the hashtable, must be power of two
    const size_t numBuckets_{0};

    // materialized value of numBuckets_ - 1
    const size_t numBucketsMask_{0};

    // actual buckets.
    std::unique_ptr<CompressedPtrType[]> hashTable_;

    // when non-zero, hashTable_ points at an Impl-owned HugeTLB mmap of this
    // many bytes (freed via munmap); zero means new[] or user-managed memory.
    size_t mmapBytes_{0};

    // indicate whether or not the hash table uses user-managed memory and
    // is thus restorable from serialized state
    const bool restorable_{false};

    // object used to compress/decompress node pointers to reduce memory
    // footprint of Hook
    const PtrCompressor compressor_;

    // Hash the key
    const Hasher hasher_;
  };

 public:
  using SerializationType = serialization::ChainedHashTableObject;

  // node used for chaining the hash table for collision.
  template <typename T>
  struct CACHELIB_PACKED_ATTR Hook {
    using CompressedPtrType = typename T::CompressedPtrType;
    using PtrCompressor = typename T::PtrCompressor;
    // sets the next in the hash chain to the passed in value.
    void setHashNext(T* n, const PtrCompressor& compressor) noexcept {
      next_ = compressor.compress(n);
    }

    void setHashNext(CompressedPtrType n) noexcept { next_ = n; }

    // gets the next in hash chain for this node.
    T* getHashNext(const PtrCompressor& compressor) const noexcept {
      return compressor.unCompress(next_);
    }

    CompressedPtrType getHashNext() const noexcept { return next_; }

   private:
    CompressedPtrType next_{};
  };

  // Config class for the chained hash table.
  class Config {
   public:
    // Do not add 'noexcept' here - causes GCC to delete this method:
    //    "config() is implicitly deleted because its exception-specification
    //     does not match the implicit exception-specification
    //     <noexcept (false)>"
    // followed by:
    //    "CacheAllocatorConfig.h:522:29: error: use of deleted function
    //     constexpr facebook::cachelib::ChainedHashTable::Config::Config()
    Config() = default;

    // @param bucketsPower number of buckets in base 2 logarithm
    // @param locksPower number of locks in base 2 logarithm
    Config(unsigned int bucketsPower, unsigned int locksPower)
        : Config(bucketsPower, locksPower, std::make_shared<MurmurHash2>()) {}

    // @param bucketsPower number of buckets in base 2 logarithm
    // @param locksPower number of locks in base 2 logarithm
    // @param hasher the key hash function
    Config(unsigned int bucketsPower, unsigned int locksPower, Hasher hasher)
        : bucketsPower_(bucketsPower),
          locksPower_(locksPower),
          hasher_(std::move(hasher)) {
      if (bucketsPower_ > kMaxBucketPower || locksPower_ > kMaxLockPower ||
          locksPower_ > bucketsPower_) {
        throw std::invalid_argument(fmt::format(
            "Invalid arguments to the config constructor bucketPower =  {}, "
            "lockPower = {}",
            bucketsPower_, locksPower_));
      }
    }

    Config(const Config&) = default;
    Config& operator=(const Config&) = default;

    size_t getNumBuckets() const noexcept {
      return static_cast<size_t>(1) << bucketsPower_;
    }

    size_t getNumLocks() const noexcept {
      return static_cast<size_t>(1) << locksPower_;
    }

    // Estimate bucketsPower and LocksPower based on cache entries.
    void sizeBucketsPowerAndLocksPower(size_t cacheEntries) {
      // The percentage of used buckets vs unused buckets is measured by a load
      // factor. For optimal performance, the load factor should not be more
      // than 60%.
      bucketsPower_ =
          static_cast<size_t>(ceil(log2(cacheEntries * 1.6 /* load factor */)));

      if (bucketsPower_ > kMaxBucketPower) {
        throw std::invalid_argument(fmt::format(
            "Invalid arguments to the config constructor cacheEntries =  {}",
            cacheEntries));
      }

      // 1 lock per 1000 buckets.
      locksPower_ =
          (bucketsPower_ <= 20) ? (bucketsPower_ / 2) + 1 : bucketsPower_ - 10;
    }

    unsigned int getBucketsPower() const noexcept { return bucketsPower_; }

    unsigned int getLocksPower() const noexcept { return locksPower_; }

    const Hasher& getHasher() const noexcept { return hasher_; }

    std::map<std::string, std::string> serialize() const {
      std::map<std::string, std::string> configMap;
      configMap["BucketsPower"] = std::to_string(bucketsPower_);
      configMap["LocksPower"] = std::to_string(locksPower_);
      configMap["Hasher"] =
          hasher_->getMagicId() == 1 ? "FNVHash" : "MurmurHash2";
      return configMap;
    }

   private:
    // 4 billion buckets should be good enough for everyone.
    static constexpr unsigned int kMaxBucketPower = 32;
    static constexpr unsigned int kMaxLockPower = 32;

    // The following are expressed as powers of two to make the modulo
    // arithmetic simpler.

    // total number of buckets in the hashtable expressed as power of two.
    unsigned int bucketsPower_{10};

    // total number of locks for the hashtable expressed as a power of two.
    unsigned int locksPower_{5};

    Hasher hasher_ = std::make_shared<MurmurHash2>();
  };

  // Interface for the Container that implements a hash table. Maintains
  // the node's isInAccessContainer state. T must implement an interface to
  // markAccessible(), unmarkAccessible() and isAccessible().
  template <typename T, Hook<T> T::* HookPtr, typename LockT>
  struct Container {
   private:
    using BucketId = typename Impl<T, HookPtr>::BucketId;

   public:
    using Key = typename T::Key;
    using Handle = typename T::Handle;
    using HandleMaker = typename T::HandleMaker;
    using PreRemoveCb = std::function<void(RemoveContext, const T&)>;
    using CompressedPtrType = typename T::CompressedPtrType;
    using PtrCompressor = typename T::PtrCompressor;

    // default handle maker that calls incRef
    static const HandleMaker kDefaultHandleMaker;

    // container with default config.
    Container() noexcept
        : Container(Config{}, PtrCompressor(), kDefaultHandleMaker) {}

    // create hash table container with local-managed memory
    // @param config      the config for the hashtable
    // @param compressor  object used to compress/decompress node pointers
    // @param hm          the functor that creates a Handle from T*
    // @param pageSize    back the bucket array with HugeTLB pages (bytes);
    //                    default => normal pages
    Container(Config c,
              const PtrCompressor& compressor,
              HandleMaker hm = kDefaultHandleMaker,
              PageSize pageSize = PageSize())
        : config_(std::move(c)),
          handleMaker_(std::move(hm)),
          ht_{config_.getNumBuckets(), compressor, config_.getHasher(),
              pageSize},
          locks_{config_.getLocksPower(), config_.getHasher()} {}

    // create hash table container with user-managed memory
    //
    // @param c           config for hash table
    // @param memStart    hash table memory managed by the user
    // @param compressor  object used to compress/decompress node pointers
    // @param hm          the functor that creates a Handle from T*
    Container(Config c,
              void* memStart,
              const PtrCompressor& compressor,
              HandleMaker hm = kDefaultHandleMaker)
        : config_(std::move(c)),
          handleMaker_(std::move(hm)),
          ht_{config_.getNumBuckets(), memStart, compressor,
              config_.getHasher(), true /* resetMem */},
          locks_{config_.getLocksPower(), config_.getHasher()} {}

    // restore hash table from serialized data.
    //
    // @param object      serialized object
    // @param newConfig   the new set of configurations
    // @param memSegment  shared memory segment for the hash table
    // @param compressor  object used to compress/decompress node pointers
    // @param hm          the functor that creates a Handle from T*
    // @param pageSize    page size the segment was backed with (bytes), used
    //                    to validate the mapped segment size.
    //
    // @throw std::invalid argument if the bucket power in new config does not
    //        match the previous state or the size of the memSegment does not
    //        match the old state.
    Container(const serialization::ChainedHashTableObject& object,
              const Config& newConfig,
              ShmAddr memSegment,
              const PtrCompressor& compressor,
              HandleMaker hm = kDefaultHandleMaker,
              PageSize pageSize = PageSize());

    // restore hash table from previous state. This only works when the
    // hash table memory is managed by the user.
    //
    // @param object      serialized object
    // @param newConfig   the new set of configurations
    // @param memStart    hash table memory managed by the user
    // @param nBytes      size of memory allocation pointed to by memStart
    // @param compressor  object used to compress/decompress node pointers
    // @param hm          the functor that creates a Handle from T*
    // @param pageSize    page size the segment was backed with (bytes), used
    //                    to validate the mapped segment size.
    //
    // @throw std::invalid argument if the bucket power in new config does not
    //        match the previous state or the size of the memSegment does not
    //        match the old state.
    Container(const serialization::ChainedHashTableObject& object,
              const Config& newConfig,
              void* memStart,
              size_t nBytes,
              const PtrCompressor& compressor,
              HandleMaker hm = kDefaultHandleMaker,
              PageSize pageSize = PageSize());

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    // inserts the node into the hash table and marks it as being in the
    // hashtable upon success. If another node exists with the same key, the
    // insert fails. On failure the state of the node is unchanged.
    //
    // @param node  the node to be inserted into the hashtable
    // @return  True if the node was successfully inserted into the hashtable.
    //          False if not.
    bool insert(T& node) noexcept;

    // inserts or replaces the node into the hash table and marks it being in
    // the hashtable upon success. If another node exists with the same key, the
    // that node is removed. On failure the state of the node is unchanged.
    //
    // @param node  the node to be inserted into the hashtable
    // @return  if the node was successfully inserted into the hashtable,
    //          returns a null handle. If the node replaced an existing node,
    //          a handle to the old node is returned.
    //
    // @throw std::overflow_error is the maximum item refcount is execeeded by
    //        creating this item handle.
    Handle insertOrReplace(T& node);

    // replaces a node into the hash table, only if another node exists with
    // the same key and is marked accessible.
    //
    // @param oldNode   expected current node in the hash table
    // @param newNode   the new node for the key
    //
    // @return true  if oldNode exists, is accessible, and was replaced
    //               successfully.
    bool replaceIfAccessible(T& oldNode, T& newNode) noexcept;

    // replaces a node if predicate returns true on the existing node
    //
    // @param oldNode   expected current node in the hash table
    // @param newNode   the new node for the key
    // @param predicate   asseses if condition is met for the oldNode to merit
    //                    a replace
    //
    // @return true  if oldNode exists, is accessible, predicate is true, and
    //               was replaced successfully.
    template <typename F>
    bool replaceIf(T& oldNode, T& newNode, F&& predicate);

    // removes the node from the hashtable and unmarks it as accessible. If
    // the node does not exists, returns False.
    //
    // @param   node  node to be removed from the hashtable.
    // @return  True if the node was in the hashtable and if it was
    //          successfully removed. False if the node was not in the
    //          hashtable.
    bool remove(T& node,
                RemoveContext context = RemoveContext::kNormal,
                const PreRemoveCb& preRemoveCb = {}) noexcept;

    // remove a node from the container if it exists for the key and the
    // predicate returns true for the node. This is intended to simplify the
    // eviction purposes to guarantee a good selection of candidate.
    //
    // @param  node       the node to be removed
    // @param  predicate  the predicate check for the node
    //
    // @return handle to the node if we successfully removed it. returns a
    // null handle if the node was either not in the container or the
    // predicate failed.
    Handle removeIf(T& node,
                    const std::function<bool(const T& node)>& predicate,
                    RemoveContext context = RemoveContext::kNormal,
                    const PreRemoveCb& preRemoveCb = {});

    // finds the node corresponding to the key in the hashtable and returns a
    // handle to that node.
    //
    // @param key   the lookup key
    // @param args  arguments to construct a handle for T.
    //
    // @return  Handle with valid T* if there is a node corresponding to the
    //          key or a Handle with nullptr if not.
    //
    // @throw std::overflow_error is the maximum item refcount is execeeded by
    //        creating this item handle.
    Handle find(Key key) const;

    // Batched find(): handles[i] = find(keys[i]), with the bucket slots and
    // chain heads prefetched ahead of the lookups.
    //
    // @param keys     keys to look up
    // @param handles  output, same size as keys
    //
    // @throw std::overflow_error if the maximum item refcount is exceeded by
    //        creating a handle
    template <typename OutputHandle>
    void findMany(folly::Range<const Key*> keys,
                  folly::Range<OutputHandle*> handles) const;

    // for saving the state of the hash table
    //
    // precondition:  serialization must happen without any reader or writer
    // present. Any modification of this object afterwards will result in an
    // invalid, inconsistent state for the serialized data.
    //
    // @throw std::logic_error if the container has any pending iterators that
    // need to be destroyed or if the container can not be restored.
    serialization::ChainedHashTableObject saveState() const;

    // get the required size for the buckets.
    static size_t getRequiredSize(size_t numBuckets) noexcept {
      return sizeof(CompressedPtrType) * numBuckets;
    }

    // return the hashtable size in bytes
    size_t getMemorySize() const noexcept { return ht_.getMemorySize(); }

    const void* getBucketMemory() const noexcept {
      return ht_.getBucketMemory();
    }

    const Config& getConfig() const noexcept { return config_; }

    unsigned int getHashpower() const noexcept {
      return config_.getBucketsPower();
    }

    // Result of a non-blocking handle acquisition attempt.
    enum class TryAcquireResult : uint8_t {
      kSuccess, // handle acquired
      kSkip,    // handle not acquirable (evicted, removed, etc.); skip
      kMoving,  // item being moved, retry via find()
    };

    // Iterator interface for the hashtable. Iterates over the hashtable
    // bucket by bucket and takes a snapshot of the bucket to iterate over. It
    // guarantees that all keys that were present when the iteration started
    // will be accessible unless they are removed. Keys that are
    // removed/inserted during the lifetime of an iterator are not guaranteed
    // to be either visited or not-visited. Adding/Removing from the hash
    // table while the iterator is alive will not invalidate any iterator or
    // the element that the iterator points at currently. The iterator
    // internally holds a Handle to the item.
    class Iterator {
     public:
      ~Iterator() {
        XDCHECK_GT(container_->numIterators_.load(), 0u);
        --container_->numIterators_;
      }
      Iterator(const Iterator&) = delete;
      Iterator& operator=(const Iterator&) = delete;

      Iterator(Iterator&&) noexcept;
      Iterator& operator=(Iterator&&) noexcept;
      enum EndIterT { EndIter };

      // increment the iterator to the next element.
      // with/without throttler
      Iterator& operator++();

      // dereference the current element that the iterator is pointing to.
      T& operator*();
      T* operator->() { return &(*(*this)); }
      const T& operator*() const;
      const T* operator->() const { return &(*(*this)); }

      bool operator==(const Iterator& other) const noexcept {
        return container_ == other.container_ &&
               currBucket_ == other.currBucket_ && curSor_ == other.curSor_;
      }

      bool operator!=(const Iterator& other) const noexcept {
        return !(*this == other);
      }

      const Handle& asHandle() { return curr(); }

      // reset the Iterator to begin of container
      void reset();

     private:
      // container for the iterator
      using C = Container<T, HookPtr, LockT>;

      // construct an iterator with the given
      friend C;
      explicit Iterator(C& ht,
                        folly::Optional<util::Throttler::Config>
                            throttlerConfig = folly::none);

      Iterator(C& ht, EndIterT);

      // the container over which we are iterating
      mutable C* container_;

      // current bucket that the iterator is pointing to.
      mutable BucketId currBucket_{0};

      // cursor into the current bucket.
      mutable unsigned int curSor_{0};

      // current bucket.
      mutable std::vector<Handle> bucketElems_;

      // optional throttler
      folly::Optional<util::Throttler> throttler_ = folly::none;

      // returns the handle for current item in the iterator.
      Handle& curr() const {
        if (curSor_ < bucketElems_.size()) {
          return bucketElems_[curSor_];
        }
        throw std::logic_error(
            "Iterator in invalid state with curSor_: " +
            folly::to<std::string>(curSor_) + ", currBucket_: " +
            folly::to<std::string>(currBucket_) + ", total buckets: " +
            folly::to<std::string>(container_->config_.getNumBuckets()));
      }
    };

    // Iterator interface to the container.
    // whether it constructs iterator of begin with a throttler config
    Iterator begin(folly::Optional<util::Throttler::Config> throttlerConfig);

    Iterator begin() { return Iterator(*this); }
    Iterator end() { return Iterator(*this, Iterator::EndIter); }

    // Like Iterator, but batches by lock group instead of per-bucket.
    // Acquires each lock once and snapshots handles for all buckets under
    // that lock, reducing the total number of lock acquisitions from
    // O(numBuckets) to O(numLocks). Uses non-blocking handle creation
    // under the lock with retry outside, avoiding the deadlock where
    // blocking handleMaker_ waits for item moves while holding the lock.
    //
    // Trade-off: larger snapshot per lock group means more items are pinned
    // (not evictable) at once compared to the per-bucket Iterator.
    class LockGroupIterator {
     public:
      using TryHandleMakerFn =
          std::function<std::pair<Handle, TryAcquireResult>(T*)>;
      using FindByKeyFn = std::function<Handle(folly::StringPiece)>;
      using FilterFn = std::function<bool(folly::StringPiece)>;

      struct ScanStats {
        uint64_t visited{0}; // total items scanned
        uint64_t matched{0}; // items where key filter returned true
        uint64_t skipped{0}; // matched items skipped (handle not acquirable)
        uint64_t retried{0}; // matched items retried via findByKey
      };

      ~LockGroupIterator() {
        XDCHECK_GT(container_->numIterators_.load(), 0u);
        --container_->numIterators_;
      }
      LockGroupIterator(const LockGroupIterator&) = delete;
      LockGroupIterator& operator=(const LockGroupIterator&) = delete;

      LockGroupIterator(LockGroupIterator&&) noexcept;
      LockGroupIterator& operator=(LockGroupIterator&&) noexcept;
      enum EndIterT { EndIter };

      LockGroupIterator& operator++();

      T& operator*();
      T* operator->() { return &(*(*this)); }
      const T& operator*() const;
      const T* operator->() const { return &(*(*this)); }

      bool operator==(const LockGroupIterator& other) const noexcept {
        return container_ == other.container_ && currLock_ == other.currLock_ &&
               cursor_ == other.cursor_;
      }

      bool operator!=(const LockGroupIterator& other) const noexcept {
        return !(*this == other);
      }

      const Handle& asHandle() { return curr(); }

      void reset();

      // Accumulated scan statistics since construction or last reset.
      const ScanStats& getStats() const { return stats_; }

     private:
      using C = Container<T, HookPtr, LockT>;

      friend C;
      explicit LockGroupIterator(C& ht,
                                 TryHandleMakerFn tryHandleMaker,
                                 FindByKeyFn findByKey,
                                 FilterFn filter,
                                 folly::Optional<util::Throttler::Config>
                                     throttlerConfig = folly::none);

      LockGroupIterator(C& ht, EndIterT);

      mutable C* container_;
      TryHandleMakerFn tryHandleMaker_;
      FindByKeyFn findByKey_;
      FilterFn filter_;

      // current lock group that the iterator is pointing to.
      mutable size_t currLock_{0};
      // cursor into the current lock group's snapshot.
      mutable unsigned int cursor_{0};
      // snapshot of handles for all items in the current lock group.
      mutable std::vector<Handle> lockGroupElems_;

      ScanStats stats_;

      folly::Optional<util::Throttler> throttler_ = folly::none;

      Handle& curr() const {
        if (cursor_ < lockGroupElems_.size()) {
          return lockGroupElems_[cursor_];
        }
        throw std::logic_error(
            "LockGroupIterator in invalid state with cursor_: " +
            folly::to<std::string>(cursor_) + ", currLock_: " +
            folly::to<std::string>(currLock_) + ", total locks: " +
            folly::to<std::string>(container_->config_.getNumLocks()));
      }

      // Returns handles for all items in the given lock group. Uses
      // non-blocking tryHandleMaker_ under the lock, then retries moving
      // items via findByKey_ outside it.
      std::vector<Handle> getLockGroupElems(size_t lockIdx);
    };

    LockGroupIterator beginLockGroup(
        typename LockGroupIterator::TryHandleMakerFn tryHandleMaker,
        typename LockGroupIterator::FindByKeyFn findByKey,
        typename LockGroupIterator::FilterFn filter,
        folly::Optional<util::Throttler::Config> throttlerConfig);

    LockGroupIterator beginLockGroup(
        typename LockGroupIterator::TryHandleMakerFn tryHandleMaker,
        typename LockGroupIterator::FindByKeyFn findByKey,
        typename LockGroupIterator::FilterFn filter = {}) {
      return LockGroupIterator(*this, std::move(tryHandleMaker),
                               std::move(findByKey), std::move(filter));
    }

    LockGroupIterator endLockGroup() {
      return LockGroupIterator(*this, LockGroupIterator::EndIter);
    }

    // Stats describing the distribution of items (keys) in the hash table
    struct DistributionStats {
      uint64_t numKeys{0};
      uint64_t numBuckets{0};
      // map from bucket id to number of items in the bucket.
      std::map<unsigned int, uint64_t> itemDistribution{};
    };

    struct Stats {
      uint64_t numKeys;
      uint64_t numBuckets;
    };

    // Get the distribution stats. This function will use cached results
    // if the difference since last updated is not significant. This is
    // expensive. Call at your discretion.
    //
    // Critiera for refreshing the stats:
    //  - 10 minutes since last update, OR
    //  - 5% more or less number of keys in the hash table
    DistributionStats getDistributionStats() const;

    // lightweight stats that give the number of keys and buckets inside the
    // container. This is guaranteed to be fast.
    Stats getStats() const noexcept { return {numKeys_, ht_.getNumBuckets()}; }

    // Get the total number of keys inserted into the hash table
    uint64_t getNumKeys() const noexcept {
      return numKeys_.load(std::memory_order_relaxed);
    }

   private:
    using Hashtable = Impl<T, HookPtr>;

    static void invokePreRemoveCb(const PreRemoveCb& preRemoveCb,
                                  RemoveContext context,
                                  const T& node) noexcept {
      if (preRemoveCb) {
        preRemoveCb(context, node);
      }
    }

    // Fetch a vector of handle to the items belonging to a given bucket. This
    // is for use by the iterator. 'handles' will be cleared and then populated
    // with handles for the items in the given bucket. Items will be skipped if
    // the handle cannot be acquired for any reason.
    void getBucketElems(BucketId bucket, std::vector<Handle>& handles) const;

    // config for the hash table.
    const Config config_{};

    // handle maker to convert the T* to T::Handle
    HandleMaker handleMaker_;

    // the hashtable buckets
    Hashtable ht_;

    // locks protecting the hashtable buckets
    mutable LockT locks_;

    std::atomic<unsigned int> numIterators_{0};

    // Cached stats for distribution
    // This is updated if the number of keys changes by more than 5%, or
    // it has been 10 minutes since the stats has last been updated.
    mutable std::mutex cachedStatsLock_;
    mutable DistributionStats cachedStats_{};

    // if we can recompute the cachedStats if it is too old. Set to false when
    // another thread is computing it.
    mutable bool canRecomputeDistributionStats_{true};

    // when the distribution was last computed.
    mutable time_t cachedStatsUpdateTime_{0};

    // number of the keys stored in this hash table
    std::atomic<uint64_t> numKeys_{0};
  };
};

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
const typename T::HandleMaker
    ChainedHashTable::Container<T, HookPtr, LockT>::kDefaultHandleMaker =
        [](T* t) -> typename T::Handle {
  if (t) {
    t->incRef();
  }
  return typename T::Handle{t};
};

template <typename T, typename ChainedHashTable::Hook<T> T::* HookPtr>
ChainedHashTable::Impl<T, HookPtr>::Impl(size_t numBuckets,
                                         const PtrCompressor& compressor,
                                         const Hasher& hasher,
                                         PageSize pageSize)
    : numBuckets_(numBuckets),
      numBucketsMask_(numBuckets - 1),
      compressor_(compressor),
      hasher_(hasher) {
  if (numBuckets == 0) {
    throw std::invalid_argument("Can not have 0 buckets");
  }
  if (numBuckets & (numBuckets - 1)) {
    throw std::invalid_argument("Number of buckets must be a power of two");
  }
  if (pageSize.isHugePage()) {
    mmapBytes_ =
        pageSize.getPageAlignedSize(numBuckets_ * sizeof(CompressedPtrType));
    hashTable_.reset(static_cast<CompressedPtrType*>(
        util::mmapAlignedZeroedMemory(pageSize.getPageSize(), mmapBytes_,
                                      /*noAccess=*/false,
                                      pageSize.hugePageMmapFlags())));
  } else {
    hashTable_ = std::make_unique<CompressedPtrType[]>(numBuckets_);
  }
  CompressedPtrType* memStart = hashTable_.get();
  std::fill(memStart, memStart + numBuckets_, CompressedPtrType{});
}

template <typename T, typename ChainedHashTable::Hook<T> T::* HookPtr>
ChainedHashTable::Impl<T, HookPtr>::Impl(size_t numBuckets,
                                         void* memStart,
                                         const PtrCompressor& compressor,
                                         const Hasher& hasher,
                                         bool resetMem)
    : numBuckets_(numBuckets),
      numBucketsMask_(numBuckets - 1),
      hashTable_(static_cast<CompressedPtrType*>(memStart)),
      restorable_(true),
      compressor_(compressor),
      hasher_(hasher) {
  if (numBuckets == 0) {
    throw std::invalid_argument("Can not have 0 buckets");
  }
  if (numBuckets & (numBuckets - 1)) {
    throw std::invalid_argument("Number of buckets must be a power of two");
  }
  if (resetMem) {
    CompressedPtrType* memStartBucket =
        static_cast<CompressedPtrType*>(memStart);
    std::fill(memStartBucket, memStartBucket + numBuckets_,
              CompressedPtrType{});
  }
}

template <typename T, typename ChainedHashTable::Hook<T> T::* HookPtr>
ChainedHashTable::Impl<T, HookPtr>::Impl::~Impl() {
  if (mmapBytes_ != 0) {
    // Impl-owned HugeTLB mapping: detach from the new[] deleter, then munmap.
    munmap(hashTable_.release(), mmapBytes_);
  } else if (restorable_) {
    hashTable_.release();
  }
}

template <typename T, typename ChainedHashTable::Hook<T> T::* HookPtr>
typename ChainedHashTable::Impl<T, HookPtr>::BucketId
ChainedHashTable::Impl<T, HookPtr>::getBucket(
    typename T::Key k) const noexcept {
  return (*hasher_)(k.data(), k.size()) & numBucketsMask_;
}

template <typename T, typename ChainedHashTable::Hook<T> T::* HookPtr>
bool ChainedHashTable::Impl<T, HookPtr>::insertInBucket(
    T& node, BucketId bucket) noexcept {
  XDCHECK_LT(bucket, numBuckets_);
  const auto existing = findInBucket(node.getKey(), bucket);
  if (existing != nullptr) {
    // already there
    return false;
  }

  // insert at the head of the bucket
  const auto head = hashTable_[bucket];
  hashTable_[bucket] = compressor_.compress(&node);
  setHashNext(node, head);
  return true;
}

template <typename T, typename ChainedHashTable::Hook<T> T::* HookPtr>
T* ChainedHashTable::Impl<T, HookPtr>::insertOrReplaceInBucket(
    T& node, BucketId bucket) noexcept {
  XDCHECK_LT(bucket, numBuckets_);

  // See if we can find the key and the previous node
  T* curr = compressor_.unCompress(hashTable_[bucket]);
  T* prev = nullptr;

  const auto key = node.getKey();
  while (curr != nullptr && key != curr->getKey()) {
    prev = curr;
    curr = getHashNext(*curr);
  }

  // insert if the key doesn't exist
  if (!curr) {
    const auto head = hashTable_[bucket];
    hashTable_[bucket] = compressor_.compress(&node);
    setHashNext(node, head);
    return nullptr;
  }

  // replace
  if (prev) {
    setHashNext(*prev, &node);
  } else {
    hashTable_[bucket] = compressor_.compress(&node);
  }
  setHashNext(node, getHashNext(*curr));

  return curr;
}

template <typename T, typename ChainedHashTable::Hook<T> T::* HookPtr>
void ChainedHashTable::Impl<T, HookPtr>::removeFromBucket(
    T& node, BucketId bucket) noexcept {
  // node must be present in hashtable.
  XDCHECK_EQ(reinterpret_cast<uintptr_t>(findInBucket(node.getKey(), bucket)),
             reinterpret_cast<uintptr_t>(&node))
      << node.toString();

  T* const prev = findPrevInBucket(node, bucket);
  if (prev != nullptr) {
    setHashNext(*prev, getHashNext(node));
  } else {
    XDCHECK_EQ(reinterpret_cast<uintptr_t>(&node),
               reinterpret_cast<uintptr_t>(
                   compressor_.unCompress(hashTable_[bucket])));
    hashTable_[bucket] = getHashNextCompressed(node);
  }
}

template <typename T, typename ChainedHashTable::Hook<T> T::* HookPtr>
T* ChainedHashTable::Impl<T, HookPtr>::findInBucket(
    Key key, BucketId bucket) const noexcept {
  XDCHECK_LT(bucket, numBuckets_);
  T* curr = compressor_.unCompress(hashTable_[bucket]);
  while (curr != nullptr && curr->getKey() != key) {
    curr = getHashNext(*curr);
  }
  return curr;
}

template <typename T, typename ChainedHashTable::Hook<T> T::* HookPtr>
T* ChainedHashTable::Impl<T, HookPtr>::findPrevInBucket(
    const T& node, BucketId bucket) const noexcept {
  XDCHECK_LT(bucket, numBuckets_);
  T* curr = compressor_.unCompress(hashTable_[bucket]);
  T* prev = nullptr;

  const auto key = node.getKey();
  while (curr != nullptr && key != curr->getKey()) {
    prev = curr;
    curr = getHashNext(*curr);
  }
  // node must be in the hashtable
  XDCHECK(curr != nullptr);
  return prev;
}

template <typename T, typename ChainedHashTable::Hook<T> T::* HookPtr>
template <typename F>
void ChainedHashTable::Impl<T, HookPtr>::forEachBucketElem(BucketId bucket,
                                                           F&& func) const {
  XDCHECK_LT(bucket, numBuckets_);
  T* curr = compressor_.unCompress(hashTable_[bucket]);

  while (curr != nullptr) {
    func(curr);
    curr = getHashNext(*curr);
  }
}

template <typename T, typename ChainedHashTable::Hook<T> T::* HookPtr>
unsigned int ChainedHashTable::Impl<T, HookPtr>::getBucketNumElems(
    BucketId bucket) const {
  XDCHECK_LT(bucket, numBuckets_);

  T* curr = compressor_.unCompress(hashTable_[bucket]);

  unsigned int numElems = 0;
  while (curr != nullptr) {
    ++numElems;
    curr = getHashNext(*curr);
  }
  return numElems;
}

// AccessContainer interface
template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
ChainedHashTable::Container<T, HookPtr, LockT>::Container(
    const serialization::ChainedHashTableObject& object,
    const Config& config,
    ShmAddr memSegment,
    const PtrCompressor& compressor,
    HandleMaker hm,
    PageSize pageSize)
    : Container(object,
                config,
                memSegment.addr,
                memSegment.size,
                compressor,
                std::move(hm),
                pageSize) {}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
ChainedHashTable::Container<T, HookPtr, LockT>::Container(
    const serialization::ChainedHashTableObject& object,
    const Config& config,
    void* memStart,
    size_t nBytes,
    const PtrCompressor& compressor,
    HandleMaker hm,
    PageSize pageSize)
    : config_{config},
      handleMaker_(std::move(hm)),
      ht_{config_.getNumBuckets(), memStart, compressor, config_.getHasher(),
          false /* resetMem */},
      locks_{config_.getLocksPower(), config_.getHasher()},
      numKeys_(*object.numKeys()) {
  if (config_.getBucketsPower() !=
      static_cast<uint32_t>(*object.bucketsPower())) {
    throw std::invalid_argument(
        fmt::format("Hashtable bucket power not compatible. old = {}, new = {}",
                    *object.bucketsPower(),
                    config.getBucketsPower()));
  }

  // Take page alignment into consideration when comparing the size of the
  // shared memory and the size of the hashtable.
  if (nBytes !=
      util::getAlignedSize(ht_.getMemorySize(), pageSize.getPageSize())) {
    throw std::invalid_argument(
        fmt::format("Hashtable size not compatible. old = {}, new = {}",
                    ht_.getMemorySize(),
                    nBytes));
  }

  // checking hasher magic id not equal to 0 is to ensure it'll be
  // a warm roll going from a cachelib without hasher magic id to
  // one with a magic id
  if (*object.hasherMagicId() != 0 &&
      *object.hasherMagicId() != config_.getHasher()->getMagicId()) {
    throw std::invalid_argument(fmt::format(
        "Hash object's ID mismatch. expected = {}, actual = {}",
        *object.hasherMagicId(), config_.getHasher()->getMagicId()));
  }
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
typename ChainedHashTable::Container<T, HookPtr, LockT>::DistributionStats
ChainedHashTable::Container<T, HookPtr, LockT>::getDistributionStats() const {
  const auto now = util::getCurrentTimeSec();
  const uint64_t numKeys = numKeys_;

  std::unique_lock<std::mutex> statsLockGuard(cachedStatsLock_);
  const auto numKeysDifference = numKeys > cachedStats_.numKeys
                                     ? numKeys - cachedStats_.numKeys
                                     : cachedStats_.numKeys - numKeys;

  const bool needToRecompute =
      (now - cachedStatsUpdateTime_ > 10 * 60 /* seconds */) ||
      (cachedStats_.numKeys > 0 &&
       (static_cast<double>(numKeysDifference) /
            static_cast<double>(cachedStats_.numKeys) >
        0.05));

  // return the cached value or if someone else is already computing.
  if (!needToRecompute || !canRecomputeDistributionStats_) {
    return cachedStats_;
  }

  // record that we are iterating so that we dont cause everyone who
  // observes this to recompute
  canRecomputeDistributionStats_ = false;

  // release the lock.
  statsLockGuard.unlock();

  // compute the distribution
  std::map<unsigned int, uint64_t> distribution;
  const auto numBuckets = ht_.getNumBuckets();
  for (BucketId currBucket = 0; currBucket < numBuckets; ++currBucket) {
    auto l = locks_.lockShared(currBucket);
    ++distribution[ht_.getBucketNumElems(currBucket)];
  }

  // acquire lock
  statsLockGuard.lock();
  cachedStats_.numKeys = numKeys;
  cachedStats_.itemDistribution = std::move(distribution);
  cachedStats_.numBuckets = ht_.getNumBuckets();
  cachedStatsUpdateTime_ = now;
  canRecomputeDistributionStats_ = true;
  return cachedStats_;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
bool ChainedHashTable::Container<T, HookPtr, LockT>::insert(T& node) noexcept {
  if (node.isAccessible()) {
    // already in hash table.
    return false;
  }

  const auto bucket = ht_.getBucket(node.getKey());
  auto l = locks_.lockExclusive(bucket);
  const bool res = ht_.insertInBucket(node, bucket);

  if (res) {
    node.markAccessible();
    numKeys_.fetch_add(1, std::memory_order_relaxed);
  }

  return res;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
typename T::Handle
ChainedHashTable::Container<T, HookPtr, LockT>::insertOrReplace(T& node) {
  if (node.isAccessible()) {
    return handleMaker_(nullptr);
  }

  const auto bucket = ht_.getBucket(node.getKey());
  auto l = locks_.lockExclusive(bucket);
  T* oldNode = ht_.insertOrReplaceInBucket(node, bucket);
  XDCHECK_NE(reinterpret_cast<uintptr_t>(&node),
             reinterpret_cast<uintptr_t>(oldNode));

  // grab a handle to the old node before we mark it as not being in the hash
  // table.
  typename T::Handle handle;
  try {
    handle = handleMaker_(oldNode);
  } catch (const std::exception&) {
    // put the element back since we failed to grab handle.
    ht_.insertOrReplaceInBucket(*oldNode, bucket);
    XDCHECK_EQ(
        reinterpret_cast<uintptr_t>(ht_.findInBucket(node.getKey(), bucket)),
        reinterpret_cast<uintptr_t>(oldNode))
        << oldNode->toString();
    throw;
  }

  node.markAccessible();

  if (oldNode) {
    oldNode->unmarkAccessible();
  } else {
    numKeys_.fetch_add(1, std::memory_order_relaxed);
  }

  return handle;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
bool ChainedHashTable::Container<T, HookPtr, LockT>::replaceIfAccessible(
    T& oldNode, T& newNode) noexcept {
  return replaceIf(oldNode, newNode, [](T&) { return true; });
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
template <typename F>
bool ChainedHashTable::Container<T, HookPtr, LockT>::replaceIf(T& oldNode,
                                                               T& newNode,
                                                               F&& predicate) {
  const auto key = newNode.getKey();
  const auto bucket = ht_.getBucket(key);
  auto l = locks_.lockExclusive(bucket);

  if (oldNode.isAccessible() && predicate(oldNode)) {
    ht_.insertOrReplaceInBucket(newNode, bucket);
    oldNode.unmarkAccessible();
    newNode.markAccessible();
    return true;
  }
  return false;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
bool ChainedHashTable::Container<T, HookPtr, LockT>::remove(
    T& node, RemoveContext context, const PreRemoveCb& preRemoveCb) noexcept {
  const auto bucket = ht_.getBucket(node.getKey());
  auto l = locks_.lockExclusive(bucket);

  // check inside the lock to prevent from racing removes
  if (!node.isAccessible()) {
    return false;
  }

  invokePreRemoveCb(preRemoveCb, context, node);
  ht_.removeFromBucket(node, bucket);
  node.unmarkAccessible();

  numKeys_.fetch_sub(1, std::memory_order_relaxed);
  return true;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
typename T::Handle ChainedHashTable::Container<T, HookPtr, LockT>::removeIf(
    T& node,
    const std::function<bool(const T& node)>& predicate,
    RemoveContext context,
    const PreRemoveCb& preRemoveCb) {
  const auto bucket = ht_.getBucket(node.getKey());
  auto l = locks_.lockExclusive(bucket);

  // check inside the lock to prevent from racing removes
  if (node.isAccessible() && predicate(node)) {
    // grab the handle before we do any other state change. this ensures that
    // if handle maker throws an exception, we leave the item in a consistent
    // state.
    auto handle = handleMaker_(&node);
    invokePreRemoveCb(preRemoveCb, context, node);
    ht_.removeFromBucket(node, bucket);
    node.unmarkAccessible();
    numKeys_.fetch_sub(1, std::memory_order_relaxed);
    return handle;
  } else {
    return handleMaker_(nullptr);
  }
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
typename T::Handle ChainedHashTable::Container<T, HookPtr, LockT>::find(
    Key key) const {
  const auto bucket = ht_.getBucket(key);
  auto l = locks_.lockShared(bucket);
  return handleMaker_(ht_.findInBucket(key, bucket));
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
template <typename OutputHandle>
void ChainedHashTable::Container<T, HookPtr, LockT>::findMany(
    folly::Range<const Key*> keys, folly::Range<OutputHandle*> handles) const {
  XDCHECK_EQ(keys.size(), handles.size());
  // Bucket slots are prefetched one group of keys ahead of the chain heads,
  // which can only be decoded once their slot has arrived. Groups of 8
  // measured fastest.
  constexpr size_t kGroup = 8;
  std::array<BucketId, 2 * kGroup> buckets; // the current and the next group
  const size_t n = keys.size();
  auto prefetchSlots = [&](size_t first) {
    for (size_t i = first; i < std::min(n, first + kGroup); ++i) {
      buckets[i % buckets.size()] = ht_.getBucket(keys[i]);
      ht_.prefetchBucket(buckets[i % buckets.size()]);
    }
  };
  prefetchSlots(0);
  for (size_t group = 0; group < n; group += kGroup) {
    prefetchSlots(group + kGroup);
    const size_t end = std::min(n, group + kGroup);
    for (size_t i = group; i < end; ++i) {
      ht_.prefetchBucketHead(buckets[i % buckets.size()]);
    }
    for (size_t i = group; i < end; ++i) {
      const BucketId bucket = buckets[i % buckets.size()];
      Handle handle;
      {
        auto l = locks_.lockShared(bucket);
        handle = handleMaker_(ht_.findInBucket(keys[i], bucket));
      }
      // The old handle may hold the last reference to a removed item, whose
      // release runs callbacks that must not run under the bucket lock.
      handles[i] = std::move(handle);
    }
  }
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
serialization::ChainedHashTableObject
ChainedHashTable::Container<T, HookPtr, LockT>::saveState() const {
  if (!ht_.isRestorable()) {
    throw std::logic_error(
        "hashtable is not restorable since the memory is not managed by user");
  }

  if (numIterators_ != 0) {
    throw std::logic_error(
        fmt::format("There are {} pending iterators", numIterators_.load()));
  }

  serialization::ChainedHashTableObject object;
  *object.bucketsPower() = config_.getBucketsPower();
  *object.locksPower() = config_.getLocksPower();
  *object.numKeys() = numKeys_;
  *object.hasherMagicId() = config_.getHasher()->getMagicId();
  return object;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
void ChainedHashTable::Container<T, HookPtr, LockT>::getBucketElems(
    BucketId bucket, std::vector<Handle>& handles) const {
  handles.clear();

  // Handle validity can wait on a moving item, which may need the bucket lock
  // to finish moving. Check validity after releasing the lock to avoid
  // deadlock.
  {
    auto l = locks_.lockShared(bucket);
    ht_.forEachBucketElem(bucket, [this, &handles](T* e) {
      try {
        XDCHECK(e);
        handles.emplace_back(handleMaker_(e));
      } catch (const std::exception&) {
        // if we are not able to acquire a handle, skip over them.
      }
    });
  }

  if (handles.empty()) {
    return;
  }

  std::erase_if(handles, [](const auto& h) { return !h; });
}

// Container's Iterator
// with/without throtter to iterate
template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
typename ChainedHashTable::Container<T, HookPtr, LockT>::Iterator&
ChainedHashTable::Container<T, HookPtr, LockT>::Iterator::operator++() {
  if (throttler_) {
    throttler_->throttle();
  }

  ++curSor_;
  if (curSor_ < bucketElems_.size()) {
    return *this;
  }

  ++currBucket_;
  for (; currBucket_ < container_->config_.getNumBuckets(); ++currBucket_) {
    container_->getBucketElems(currBucket_, bucketElems_);
    if (!bucketElems_.empty()) {
      curSor_ = 0;
      return *this;
    } else if (throttler_) {
      throttler_->throttle();
    }
  }

  // reach the end
  bucketElems_.clear();
  curSor_ = 0;
  return *this;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
T& ChainedHashTable::Container<T, HookPtr, LockT>::Iterator::operator*() {
  return *curr();
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
const T& ChainedHashTable::Container<T, HookPtr, LockT>::Iterator::operator*()
    const {
  return *curr();
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
ChainedHashTable::Container<T, HookPtr, LockT>::Iterator::Iterator(
    Container<T, HookPtr, LockT>& container,
    folly::Optional<util::Throttler::Config> throttlerConfig)
    : container_(&container) {
  if (throttlerConfig) {
    throttler_.assign(util::Throttler(*throttlerConfig));
  }

  ++container_->numIterators_;

  reset();
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
ChainedHashTable::Container<T, HookPtr, LockT>::Iterator::Iterator(
    Iterator&& other) noexcept
    : container_{other.container_},
      currBucket_{other.currBucket_},
      curSor_{other.curSor_},
      bucketElems_(std::move(other.bucketElems_)) {
  // increment the iterator count when we move.
  ++container_->numIterators_;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
typename ChainedHashTable::Container<T, HookPtr, LockT>::Iterator&
ChainedHashTable::Container<T, HookPtr, LockT>::Iterator::operator=(
    Iterator&& other) noexcept {
  if (this != &other) {
    this->~Iterator();
    new (this) Iterator(std::move(other));
  }
  return *this;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
ChainedHashTable::Container<T, HookPtr, LockT>::Iterator::Iterator(
    Container<T, HookPtr, LockT>& container, EndIterT)
    : container_(&container), currBucket_{container_->config_.getNumBuckets()} {
  // increment the iterator for both the end and begin() types so that the
  // destructor can just blindly decrement.
  ++container_->numIterators_;
  XDCHECK_EQ(0u, curSor_);
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
typename ChainedHashTable::Container<T, HookPtr, LockT>::Iterator
ChainedHashTable::Container<T, HookPtr, LockT>::begin(
    folly::Optional<util::Throttler::Config> throttlerConfig) {
  return Iterator(*this, throttlerConfig);
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
void ChainedHashTable::Container<T, HookPtr, LockT>::Iterator::reset() {
  curSor_ = 0;
  currBucket_ = 0;
  container_->getBucketElems(currBucket_, bucketElems_);
  while (bucketElems_.empty() &&
         ++currBucket_ < container_->config_.getNumBuckets()) {
    if (throttler_) {
      throttler_->throttle();
    }
    container_->getBucketElems(currBucket_, bucketElems_);
  }
  XDCHECK_EQ(0u, curSor_);
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
std::vector<typename ChainedHashTable::Container<T, HookPtr, LockT>::Handle>
ChainedHashTable::Container<T, HookPtr, LockT>::LockGroupIterator::
    getLockGroupElems(size_t lockIdx) {
  std::vector<Handle> elems;
  std::vector<std::string> retryKeys;

  {
    auto guard = container_->locks_.lockShared(lockIdx);
    const auto numBuckets = container_->config_.getNumBuckets();
    container_->locks_.forEachBucketForLock(
        lockIdx, numBuckets, [this, &elems, &retryKeys](size_t bucket) {
          container_->ht_.forEachBucketElem(
              bucket, [this, &elems, &retryKeys](T* elem) {
                ++stats_.visited;
                try {
                  if (filter_ && !filter_(elem->getKey())) {
                    return;
                  }
                  ++stats_.matched;
                  auto [h, tryRes] = tryHandleMaker_(elem);
                  if (tryRes == TryAcquireResult::kSuccess) {
                    elems.emplace_back(std::move(h));
                  } else if (tryRes == TryAcquireResult::kMoving) {
                    // Can't retry under the lock — findByKey_ may block on
                    // the move which needs exclusive access to this same
                    // lock group. Save the key and retry after releasing
                    // the lock.
                    auto key = elem->getKey();
                    retryKeys.emplace_back(key.data(), key.size());
                  } else {
                    ++stats_.skipped;
                  }
                } catch (const std::exception&) {
                  // if we are not able to acquire a handle, skip over them.
                  ++stats_.skipped;
                }
              });
        });
  }

  // Retry items that were being moved. Now that we don't hold any lock,
  // findByKey_ can safely block waiting for the move to complete.
  for (auto& key : retryKeys) {
    ++stats_.retried;
    try {
      auto h = findByKey_(folly::StringPiece(key));
      if (h) {
        elems.emplace_back(std::move(h));
      }
    } catch (const std::exception&) {
      // if we are not able to acquire a handle, skip over them.
    }
  }

  return elems;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
typename ChainedHashTable::Container<T, HookPtr, LockT>::LockGroupIterator&
ChainedHashTable::Container<T, HookPtr, LockT>::LockGroupIterator::
operator++() {
  if (throttler_) {
    throttler_->throttle();
  }

  // Release the handle we're advancing past so it no longer pins the item.
  // This unblocks evictions and slab rebalances on iterated-past items
  // before we move to the next lock group.
  if (cursor_ < lockGroupElems_.size()) {
    lockGroupElems_[cursor_].reset();
  }

  ++cursor_;
  if (cursor_ < lockGroupElems_.size()) {
    return *this;
  }

  ++currLock_;
  for (; currLock_ < container_->config_.getNumLocks(); ++currLock_) {
    lockGroupElems_ = getLockGroupElems(currLock_);
    if (!lockGroupElems_.empty()) {
      cursor_ = 0;
      return *this;
    } else if (throttler_) {
      throttler_->throttle();
    }
  }

  // reached the end
  lockGroupElems_.clear();
  cursor_ = 0;
  return *this;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
T& ChainedHashTable::Container<T, HookPtr, LockT>::LockGroupIterator::
operator*() {
  return *curr();
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
const T&
ChainedHashTable::Container<T, HookPtr, LockT>::LockGroupIterator::operator*()
    const {
  return *curr();
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
ChainedHashTable::Container<T, HookPtr, LockT>::LockGroupIterator::
    LockGroupIterator(Container<T, HookPtr, LockT>& container,
                      TryHandleMakerFn tryHandleMaker,
                      FindByKeyFn findByKey,
                      FilterFn filter,
                      folly::Optional<util::Throttler::Config> throttlerConfig)
    : container_(&container),
      tryHandleMaker_(std::move(tryHandleMaker)),
      findByKey_(std::move(findByKey)),
      filter_(std::move(filter)) {
  if (throttlerConfig) {
    throttler_.assign(util::Throttler(*throttlerConfig));
  }

  ++container_->numIterators_;

  reset();
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
ChainedHashTable::Container<T, HookPtr, LockT>::LockGroupIterator::
    LockGroupIterator(Container<T, HookPtr, LockT>& container, EndIterT)
    : container_(&container), currLock_{container_->config_.getNumLocks()} {
  ++container_->numIterators_;
  XDCHECK_EQ(0u, cursor_);
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
ChainedHashTable::Container<T, HookPtr, LockT>::LockGroupIterator::
    LockGroupIterator(LockGroupIterator&& other) noexcept
    : container_{other.container_},
      tryHandleMaker_(std::move(other.tryHandleMaker_)),
      findByKey_(std::move(other.findByKey_)),
      filter_(std::move(other.filter_)),
      currLock_{other.currLock_},
      cursor_{other.cursor_},
      lockGroupElems_(std::move(other.lockGroupElems_)),
      stats_{other.stats_},
      throttler_(std::move(other.throttler_)) {
  ++container_->numIterators_;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
typename ChainedHashTable::Container<T, HookPtr, LockT>::LockGroupIterator&
ChainedHashTable::Container<T, HookPtr, LockT>::LockGroupIterator::operator=(
    LockGroupIterator&& other) noexcept {
  if (this != &other) {
    this->~LockGroupIterator();
    new (this) LockGroupIterator(std::move(other));
  }
  return *this;
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
typename ChainedHashTable::Container<T, HookPtr, LockT>::LockGroupIterator
ChainedHashTable::Container<T, HookPtr, LockT>::beginLockGroup(
    typename LockGroupIterator::TryHandleMakerFn tryHandleMaker,
    typename LockGroupIterator::FindByKeyFn findByKey,
    typename LockGroupIterator::FilterFn filter,
    folly::Optional<util::Throttler::Config> throttlerConfig) {
  return LockGroupIterator(*this, std::move(tryHandleMaker),
                           std::move(findByKey), std::move(filter),
                           throttlerConfig);
}

template <typename T,
          typename ChainedHashTable::Hook<T> T::* HookPtr,
          typename LockT>
void ChainedHashTable::Container<T, HookPtr, LockT>::LockGroupIterator::
    reset() {
  cursor_ = 0;
  currLock_ = 0;
  stats_ = ScanStats{};
  lockGroupElems_ = getLockGroupElems(currLock_);
  while (lockGroupElems_.empty() &&
         ++currLock_ < container_->config_.getNumLocks()) {
    if (throttler_) {
      throttler_->throttle();
    }
    lockGroupElems_ = getLockGroupElems(currLock_);
  }
  XDCHECK_EQ(0u, cursor_);
}

} // namespace facebook::cachelib
