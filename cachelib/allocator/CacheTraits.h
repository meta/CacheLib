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
#include "cachelib/allocator/ChainedHashTable.h"
#include "cachelib/allocator/MM2Q.h"
#include "cachelib/allocator/MMLru.h"
#include "cachelib/allocator/MMS3FIFO.h"
#include "cachelib/allocator/MMTinyLFU.h"
#include "cachelib/allocator/MMWTinyLFU.h"
#include "cachelib/allocator/memory/CompressedPtr.h"
#include "cachelib/common/Mutex.h"
#include "cachelib/common/Profiled.h"

namespace facebook {
namespace cachelib {

using ChainedHashTableBucketMutexes = RWBucketLocks<
    trace::Profiled<folly::SharedMutex, "cachelib:chained_hash_table">>;

// The cache traits supported by CacheLib.
// Cache trait is a combination of MMType, AccessType, AccesTypeLock and
// CompressedPtr. MMType is the type of MM (memory management) container
// used by the cache, which controls a cache item's life time. AccessType is
// the type of access container, which controls how an item is accessed.
// AccessTypeLock is the lock type for the access container that supports
// multiple locking primitives CompressedPtr maps slabs and allocations
// within slabs in cache memory.
struct LruCacheTrait {
  using MMType = MMLru;
  using AccessType = ChainedHashTable;
  using AccessTypeLocks = ChainedHashTableBucketMutexes;
  using CompressedPtrType = CompressedPtr4B;
};

struct LruCacheWithSpinBucketsTrait {
  using MMType = MMLru;
  using AccessType = ChainedHashTable;
  using AccessTypeLocks = SpinBuckets;
  using CompressedPtrType = CompressedPtr4B;
};

struct Lru2QCacheTrait {
  using MMType = MM2Q;
  using AccessType = ChainedHashTable;
  using AccessTypeLocks = ChainedHashTableBucketMutexes;
  using CompressedPtrType = CompressedPtr4B;
};

struct S3FIFOCacheTrait {
  using MMType = MMS3FIFO;
  using AccessType = ChainedHashTable;
  using AccessTypeLocks = SharedMutexBuckets;
  using CompressedPtrType = CompressedPtr4B;
};

struct S3FIFO5BCacheTrait {
  using MMType = MMS3FIFO;
  using AccessType = ChainedHashTable;
  using AccessTypeLocks = SharedMutexBuckets;
  using CompressedPtrType = CompressedPtr5B;
};

struct TinyLFUCacheTrait {
  using MMType = MMTinyLFU;
  using AccessType = ChainedHashTable;
  using AccessTypeLocks = ChainedHashTableBucketMutexes;
  using CompressedPtrType = CompressedPtr4B;
};

struct WTinyLFUCacheTrait {
  using MMType = MMWTinyLFU;
  using AccessType = ChainedHashTable;
  using AccessTypeLocks = ChainedHashTableBucketMutexes;
  using CompressedPtrType = CompressedPtr4B;
};

struct Lru5BCacheTrait {
  using MMType = MMLru;
  using AccessType = ChainedHashTable;
  using AccessTypeLocks = ChainedHashTableBucketMutexes;
  using CompressedPtrType = CompressedPtr5B;
};

struct Lru5BCacheWithSpinBucketsTrait {
  using MMType = MMLru;
  using AccessType = ChainedHashTable;
  using AccessTypeLocks = SpinBuckets;
  using CompressedPtrType = CompressedPtr5B;
};

struct Lru5B2QCacheTrait {
  using MMType = MM2Q;
  using AccessType = ChainedHashTable;
  using AccessTypeLocks = ChainedHashTableBucketMutexes;
  using CompressedPtrType = CompressedPtr5B;
};

struct TinyLFU5BCacheTrait {
  using MMType = MMTinyLFU;
  using AccessType = ChainedHashTable;
  using AccessTypeLocks = ChainedHashTableBucketMutexes;
  using CompressedPtrType = CompressedPtr5B;
};

struct WTinyLFU5BCacheTrait {
  using MMType = MMWTinyLFU;
  using AccessType = ChainedHashTable;
  using AccessTypeLocks = ChainedHashTableBucketMutexes;
  using CompressedPtrType = CompressedPtr5B;
};

} // namespace cachelib
} // namespace facebook
