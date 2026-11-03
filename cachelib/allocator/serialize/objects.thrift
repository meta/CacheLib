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

namespace cpp2 facebook.cachelib.serialization

include "cachelib/allocator/datastruct/serialize/objects.thrift"
include "thrift/annotation/thrift.thrift"

@thrift.AllowLegacyMissingUris
package;

// Adding a new "required" field will cause the cache to be dropped
// in the next release for our users. If the field needs to be required,
// make sure to communicate that with our users.

struct CacheAllocatorMetadata {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required i64 allocatorVersion; // version of cache alloctor
  2: i64 cacheCreationTime = 0; // time when the cache was created.
  @thrift.AllowUnsafeRequiredFieldQualifier
  3: required i64 accessType = 0; // default chained alloc
  @thrift.AllowUnsafeRequiredFieldQualifier
  4: required i64 mmType = 0; // default LRU
  5: map<byte, map<byte, i64>> fragmentationSize;
  6: list<byte> compactCachePools;
  7: i64 numPermanentItems;
  8: i64 numChainedParentItems;
  9: i64 numChainedChildItems;
  10: i64 ramFormatVersion = 0; // format version of ram cache
  11: i64 numAbortedSlabReleases = 0; // number of times slab release is aborted
  // Configured page size in bytes backing slab memory; 0 selects normal pages.
  // Compared on attach; a mismatch forces a cold roll.
  12: i64 slabPageSize = 0;
  // Configured page size in bytes backing the access-container hash tables;
  // 0 selects normal pages. Compared on attach; a mismatch forces a cold roll.
  13: i64 accessPageSize = 0;
}

struct NvmCacheMetadata {
  1: i64 nvmFormatVersion = 0;
  2: i64 creationTime = 0;
  3: bool safeShutDown = false;
  4: bool encryptionEnabled = false;
  5: bool truncateAllocSize = false;
  6: i64 lastPersistTimeMs = 0;
}

struct CompactCacheMetadataObject {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required i64 keySize;
  @thrift.AllowUnsafeRequiredFieldQualifier
  2: required i64 valueSize;
}

struct CompactCacheAllocatorObject {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required list<i64> chunks;
  @thrift.AllowUnsafeRequiredFieldQualifier
  2: required CompactCacheMetadataObject ccMetadata;
}

struct CompactCacheAllocatorManagerObject {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required map<string, CompactCacheAllocatorObject> allocators;
}

struct MMLruConfig {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required i32 lruRefreshTime;
  @thrift.AllowUnsafeRequiredFieldQualifier
  2: required bool updateOnWrite;
  @thrift.AllowUnsafeRequiredFieldQualifier
  3: required i32 lruInsertionPointSpec;
  4: bool updateOnRead = true;
  5: bool tryLockUpdate = false;
  6: double lruRefreshRatio = 0.0;
}

struct MMLruObject {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required MMLruConfig config;

  // number of evictions for this MM object.
  5: i64 evictions = 0;

  @thrift.AllowUnsafeRequiredFieldQualifier
  6: required i64 insertionPoint;
  @thrift.AllowUnsafeRequiredFieldQualifier
  7: required i64 tailSize;
  @thrift.AllowUnsafeRequiredFieldQualifier
  8: required DListObject lru;
  @thrift.AllowUnsafeRequiredFieldQualifier
  9: required i64 compressedInsertionPoint;
}

struct MMLruCollection {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required map<i32, map<i32, MMLruObject>> pools;
}

struct MM2QConfig {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required i32 lruRefreshTime;
  @thrift.AllowUnsafeRequiredFieldQualifier
  2: required bool updateOnWrite;
  @thrift.AllowUnsafeRequiredFieldQualifier
  3: required i32 hotSizePercent;
  @thrift.AllowUnsafeRequiredFieldQualifier
  4: required i32 coldSizePercent;
  5: bool updateOnRead = true;
  6: bool tryLockUpdate = false;
  7: bool rebalanceOnRecordAccess = true;
  8: double lruRefreshRatio = 0.0;
}

struct MM2QObject {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required MM2QConfig config;
  13: bool tailTrackingEnabled = false;

  // number of evictions for this MM object.
  11: i64 evictions = 0;

  // Warm, hot and cold lrus
  @thrift.AllowUnsafeRequiredFieldQualifier
  12: required MultiDListObject lrus;
}

struct MM2QCollection {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required map<i32, map<i32, MM2QObject>> pools;
}



struct MMS3FIFOConfig {
  1: required bool updateOnWrite;
  2: bool updateOnRead = true;
  3: double smallSizePercent = 0.0;
  4: double ghostSizePercent = 100.0;
  6: double lruRefreshRatio = 0.0;
  7: double lruRefreshTime = 0.0;
}

struct MMS3FIFOObject {
  1: required MMS3FIFOConfig config;

  // number of evictions for this MM object.
  2: i64 evictions = 0;

  // Warm, hot and cold lrus
  3: required MultiDListObject lrus;
}

struct MMS3FIFOCollection {
  1: required map<i32, map<i32, MMS3FIFOObject>> pools;
}


struct MMTinyLFUConfig {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required i32 lruRefreshTime;
  @thrift.AllowUnsafeRequiredFieldQualifier
  2: required bool updateOnWrite;
  @thrift.AllowUnsafeRequiredFieldQualifier
  3: required i32 windowToCacheSizeRatio;
  @thrift.AllowUnsafeRequiredFieldQualifier
  4: required i32 tinySizePercent;
  5: bool updateOnRead = true;
  6: bool tryLockUpdate = false;
  7: double lruRefreshRatio = 0.0;
  8: i32 mmReconfigureIntervalSecs = 0;
  9: bool newcomerWinsOnTie = true;
  10: i32 protectionFreq_ = 3;
  11: i32 protectionSegmentSizePct = 80;
}

struct MMTinyLFUObject {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required MMTinyLFUConfig config;

  // number of evictions for this MM object.
  2: i64 evictions = 0;

  // Warm, hot and cold lrus
  @thrift.AllowUnsafeRequiredFieldQualifier
  3: required MultiDListObject lrus;
}

struct MMTinyLFUCollection {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required map<i32, map<i32, MMTinyLFUObject>> pools;
}

struct ChainedHashTableObject {
  // fields in ChainedHashTable::Config
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required i32 bucketsPower;
  @thrift.AllowUnsafeRequiredFieldQualifier
  2: required i32 locksPower;
  3: i64 numKeys;

  // this magic id ensures on a warm roll, user cannot
  // start the cache with a different hash function
  4: i32 hasherMagicId = 0;
}

struct MMTTLBucketObject {
  4: i64 expirationTime;
  5: i64 creationTime;
  @thrift.AllowUnsafeRequiredFieldQualifier
  6: required DListObject dList;
}

struct TTLBucketCollection {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required map<i64, MMTTLBucketObject> buckets;
  2: i64 minEpoch = 0;
  3: i64 maxTTL = 0;
  4: i64 interval = 0;
}
