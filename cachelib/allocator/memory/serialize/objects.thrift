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

struct SlabAllocatorObject {
  @thrift.AllowUnsafeRequiredFieldQualifier
  2: required i64 memorySize;
  @thrift.AllowUnsafeRequiredFieldQualifier
  4: required bool canAllocate;
  @thrift.AllowUnsafeRequiredFieldQualifier
  5: required map<byte, i64> memoryPoolSize;
  @thrift.AllowUnsafeRequiredFieldQualifier
  7: required i64 slabSize;
  @thrift.AllowUnsafeRequiredFieldQualifier
  8: required i64 minAllocSize;
  @thrift.AllowUnsafeRequiredFieldQualifier
  9: required i32 nextSlabIdx;
  @thrift.AllowUnsafeRequiredFieldQualifier
  10: required list<i32> freeSlabIdxs;
  11: list<i32> advisedSlabIdxs;
}

struct AllocationClassObject {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required byte classId;
  @thrift.AllowUnsafeRequiredFieldQualifier
  2: required i64 allocationSize; // to accommodate uint32_t allocationSize
  @thrift.AllowUnsafeRequiredFieldQualifier
  4: required i64 currOffset;
  @thrift.AllowUnsafeRequiredFieldQualifier
  8: required bool canAllocate;
  9: objects.SListObject freedAllocationsObject;
  @thrift.AllowUnsafeRequiredFieldQualifier
  10: required i32 currSlabIdx;
  @thrift.AllowUnsafeRequiredFieldQualifier
  11: required list<i32> allocatedSlabIdxs;
  @thrift.AllowUnsafeRequiredFieldQualifier
  12: required list<i32> freeSlabIdxs;
}

struct MemoryPoolObject {
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required byte id;
  @thrift.AllowUnsafeRequiredFieldQualifier
  2: required i64 maxSize;
  @thrift.AllowUnsafeRequiredFieldQualifier
  3: required i64 currSlabAllocSize;
  @thrift.AllowUnsafeRequiredFieldQualifier
  4: required i64 currAllocSize;
  @thrift.AllowUnsafeRequiredFieldQualifier
  6: required list<i64> acSizes;
  @thrift.AllowUnsafeRequiredFieldQualifier
  7: required list<AllocationClassObject> ac;
  8: i64 numSlabResize = 0;
  9: i64 numSlabRebalance = 0;
  @thrift.AllowUnsafeRequiredFieldQualifier
  10: required list<i32> freeSlabIdxs;
  11: i64 numSlabsAdvised = 0;
}

struct MemoryPoolManagerObject {
  1: list<MemoryPoolObject> pools;
  2: map<string, byte> poolsByName;
  3: byte nextPoolId;
}

struct MemoryAllocatorObject {
  // fields in MemoryAllocator::Config
  @thrift.AllowUnsafeRequiredFieldQualifier
  1: required set<i64> allocSizes;
  @thrift.AllowUnsafeRequiredFieldQualifier
  4: required bool enableZeroedSlabAllocs;
  5: bool lockMemory = false;

  @thrift.AllowUnsafeRequiredFieldQualifier
  2: required SlabAllocatorObject slabAllocator;
  @thrift.AllowUnsafeRequiredFieldQualifier
  3: required MemoryPoolManagerObject memoryPoolManager;
}
