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

#include <folly/logging/xlog.h>

#include <memory>

#include "cachelib/allocator/memory/Slab.h"

namespace facebook {
namespace cachelib {

class SlabAllocator;
namespace tests {
class AllocTestBase;
}

// This CompressedPtr4B makes decompression fast by staying away from division
// and modulo arithmetic and doing those during the compression time. We most
// often decompress a CompressedPtr4B than compress a pointer while creating
// one. This is used for pointer compression by the memory allocator.

// We compress pointers by storing the tier index, slab index and alloc index of
// the allocation inside the slab.

// In original design (without memory tiers):
// Each slab addresses 22 bits of allocations (kNumSlabBits). This is split into
// allocation index and allocation size. If we have the min allocation size of
// 64 bytes (kMinAllocPower = 6 bits), remaining kNumSlabBits(22) -
// kMinAllocPower(6) = 16 bits for storing the alloc index. This leaves the
// remaining 32 - (kNumSlabBits - kMinAllocPower) = 16 bits  for the  slab
// index. Hence we can index 256 GiB of memory.

// In multi-tier design:
// kNumSlabIds and kMinAllocPower remains unchanged. The tier id occupies the
// 32nd bit only since its value cannot exceed kMaxTiers(2). This leaves the
// remaining 32 - (kNumSlabBits - kMinAllocPower) - 1 bit for tier id = 15 bits
// for the slab index. Hence we can index 128 GiB of memory per tier in
// multi-tier configuration.

class CACHELIB_PACKED_ATTR CompressedPtr4B {
 public:
  using PtrType = uint32_t;
  // Thrift doesn't support unsigned type
  using SerializedPtrType = int64_t;

  // Total number of bits to represent a CompressPtr.
  static constexpr size_t kNumBits = NumBits<PtrType>::value;

  // true if the compressed ptr expands to nullptr.
  bool isNull() const noexcept { return ptr_ == kNull; }

  bool operator==(const PtrType ptr) const noexcept { return ptr_ == ptr; }
  bool operator==(const CompressedPtr4B ptr) const noexcept {
    return ptr_ == ptr.ptr_;
  }
  bool operator!=(const PtrType ptr) const noexcept { return !(ptr == ptr_); }

  // maximum addressable memory for pointer compression to work.
  static constexpr size_t getMaxAddressableSize() noexcept {
    return static_cast<size_t>(1)
           << (numSlabIdxBits(false) + Slab::kNumSlabBits);
  }

  // default construct to nullptr.
  CompressedPtr4B() = default;

  // Restore from serialization
  explicit CompressedPtr4B(SerializedPtrType ptr)
      : ptr_(static_cast<PtrType>(ptr)) {}

  SerializedPtrType saveState() const noexcept {
    return static_cast<SerializedPtrType>(ptr_);
  }

  PtrType getRaw() const noexcept { return ptr_; }

 private:
  // null pointer representation. This is almost never guaranteed to be a
  // valid pointer that we can compress to.
  static constexpr PtrType kNull = 0xffffffff;

  // default construct to null.
  PtrType ptr_{kNull};

  // create a compressed pointer for a valid memory allocation.
  CompressedPtr4B(uint32_t slabIdx,
                  uint32_t allocIdx,
                  bool isMultiTiered,
                  TierId tid = 0)
      : ptr_(compress(slabIdx, allocIdx, isMultiTiered, tid)) {}

  constexpr explicit CompressedPtr4B(PtrType ptr) noexcept : ptr_{ptr} {}

  // number of bits for the Allocation offset in a slab.  With slab size of 22
  // bits and minimum allocation size of 64 bytes, this will be the bottom 16
  // bits of the compressed ptr.
  static constexpr unsigned int kNumAllocIdxBits =
      Slab::kNumSlabBits - Slab::kMinAllocPower;

  // Use 32nd bit position for TierId
  static constexpr unsigned int kNumTierIdxOffset = 31;

  static constexpr PtrType kAllocIdxMask = ((PtrType)1 << kNumAllocIdxBits) - 1;

  // kNumTierIdxBits most significant bits
  static constexpr PtrType kTierIdxMask = (PtrType)1 << kNumTierIdxOffset;

  // Number of bits for the slab index.
  // If CacheLib is single tiered, slab index will be the top 16 bits
  // of the compressed ptr.
  // Else if CacheLib is multi-tiered, the topmost 32nd bit will be
  // reserved for tier id. The following 15 bits will be reserved for
  // the slab index.
  static constexpr unsigned int numSlabIdxBits(bool isMultiTiered) {
    return kNumTierIdxOffset - kNumAllocIdxBits + (!isMultiTiered);
  }

  // Compress the given slabIdx and allocIdx into a 32-bit compressed
  // pointer.
  static PtrType compress(uint32_t slabIdx,
                          uint32_t allocIdx,
                          bool isMultiTiered,
                          TierId tid) noexcept {
    XDCHECK_LE(allocIdx, kAllocIdxMask);
    XDCHECK_LT(slabIdx, (1u << numSlabIdxBits(isMultiTiered)) - 1);
    if (!isMultiTiered) {
      return (slabIdx << kNumAllocIdxBits) + allocIdx;
    }
    return (static_cast<uint32_t>(tid) << kNumTierIdxOffset) +
           (slabIdx << kNumAllocIdxBits) + allocIdx;
  }

  // Get the slab index of the compressed ptr
  uint32_t getSlabIdx(bool isMultiTiered) const noexcept {
    XDCHECK(!isNull());
    auto noTierIdPtr = isMultiTiered ? ptr_ & ~kTierIdxMask : ptr_;
    return static_cast<uint32_t>(noTierIdPtr >> kNumAllocIdxBits);
  }

  // Get the allocation index of the compressed ptr
  uint32_t getAllocIdx() const noexcept {
    XDCHECK(!isNull());
    // Note: tid check not required in ptr_ since only
    //       the lower 16 bits are being read here.
    return static_cast<uint32_t>(ptr_ & kAllocIdxMask);
  }

  uint32_t getTierId(bool isMultiTiered) const noexcept {
    XDCHECK(!isNull());
    return isMultiTiered ? static_cast<uint32_t>(ptr_ >> kNumTierIdxOffset) : 0;
  }

  void setTierId(TierId tid) noexcept {
    ptr_ += static_cast<uint32_t>(tid) << kNumTierIdxOffset;
  }

  friend SlabAllocator;
  // Allow access to private members by unit tests
  friend class tests::AllocTestBase;
};

// CompressedPtr5B indexes more slabs than CompressedPtr4B.
// 16 bits are used to index allocations (min size 64 bytes) within a 4MB slab.
// 16 bits are used to index 4MB slabs which can represent upto 256GB of memory.
// If single-tiered, 7 bits are used to map 256 GB regions totaling to 32 TB.
// If CXL multi-tiered, 6 bits are used to map 256 GB regions totaling to 16 TB.
// 8th bit is reserved for the flag to indicate DRAM/NVM item.
class CACHELIB_PACKED_ATTR CompressedPtr5B {
 public:
  using PtrType = uint64_t;
  using IdxPtrType32 = uint32_t;
  using RegionPtrType8 = uint8_t;
  // Thrift doesn't support unsigned type
  using SerializedPtrType = int64_t;

  static constexpr unsigned int kIdxNumBits = NumBits<IdxPtrType32>::value;
  static constexpr unsigned int kRegionNumBits = NumBits<RegionPtrType8>::value;

  // true if the compressed ptr expands to nullptr.
  bool isNull() const noexcept {
    return (ptr_ == kNull) && (regionIdx_ == kRegionNull);
  }

  bool operator==(const SerializedPtrType ptr) const noexcept {
    return (ptr_ == deserializePtr(ptr)) &&
           (regionIdx_ == deserializeRegion(ptr));
  }

  bool operator==(const CompressedPtr5B ptr) const noexcept {
    return (ptr_ == ptr.ptr_) && (regionIdx_ == ptr.regionIdx_);
  }

  bool operator!=(const SerializedPtrType ptr) const noexcept {
    return !(*this == ptr);
  }

  // maximum addressable memory for pointer compression to work.
  static constexpr size_t getMaxAddressableSize() noexcept {
    return static_cast<size_t>(1)
           << (numSlabIdxBits(/* isMultiTiered */ false) + Slab::kNumSlabBits);
  }

  // default construct to nullptr.
  CompressedPtr5B() = default;

  // Restore from serialization
  explicit CompressedPtr5B(SerializedPtrType ptr)
      : ptr_(deserializePtr(ptr)), regionIdx_(deserializeRegion(ptr)) {}

  SerializedPtrType saveState() const noexcept { return getRaw(); }

  PtrType getRaw() const noexcept {
    return (static_cast<PtrType>(regionIdx_) << kIdxNumBits) +
           static_cast<PtrType>(ptr_);
  }

 private:
  // null pointer representation. This is almost never guaranteed to be a
  // valid pointer that we can compress to.
  static constexpr IdxPtrType32 kNull = 0xffffffff;
  static constexpr RegionPtrType8 kRegionNull = 0xff;

  // default construct to null.
  IdxPtrType32 ptr_{kNull};
  RegionPtrType8 regionIdx_{kRegionNull};

  // create a compressed pointer for a valid memory allocation.
  CompressedPtr5B(uint32_t slabIdx,
                  uint32_t allocIdx,
                  bool isMultiTiered,
                  TierId tid = 0)
      : ptr_(static_cast<IdxPtrType32>(
            compress(slabIdx, allocIdx, isMultiTiered, tid))),
        regionIdx_(getRegionIdx(slabIdx, isMultiTiered, tid)) {}

  explicit CompressedPtr5B(PtrType ptr) noexcept
      : ptr_(deserializePtr(ptr)), regionIdx_(deserializeRegion(ptr)) {}

  // number of bits for the Allocation offset in a slab.  With slab size of 22
  // bits and minimum allocation size of 64 bytes, this will be the bottom 16
  // bits of the compressed ptr.
  static constexpr unsigned int kNumAllocIdxBits =
      Slab::kNumSlabBits - Slab::kMinAllocPower;

  // Use 7th bit position in regionIdx for TierId.
  // Reserverd the 8th bit for DRAM/NVM item flag.
  static constexpr unsigned int kNumTierIdxOffset = kRegionNumBits - 2;

  static constexpr IdxPtrType32 kAllocIdxMask =
      ((IdxPtrType32)1 << kNumAllocIdxBits) - 1;

  // kNumTierIdxBits 7th bits
  static constexpr RegionPtrType8 kTierIdxMask = static_cast<RegionPtrType8>(1)
                                                 << kNumTierIdxOffset;

  // Slab index uses 16 bits to index 4MB slabs within a region and additional
  // bits to map region (7 bits in sigle-tier case. 6 bits in multi-tier case
  // where the 7th bit is the tier id)
  static constexpr unsigned int numSlabIdxBits(bool isMultiTiered) {
    return kNumTierIdxOffset + (kIdxNumBits - kNumAllocIdxBits) +
           (!isMultiTiered);
  }

  // Compress the given slabIdx and allocIdx into a 64-bit compressed
  // pointer.
  static PtrType compress(uint32_t slabIdx,
                          uint32_t allocIdx,
                          bool isMultiTiered,
                          TierId tid) noexcept {
    XDCHECK_LE(allocIdx, kAllocIdxMask);
    XDCHECK_LT(slabIdx, (1u << numSlabIdxBits(isMultiTiered)) - 1);
    if (!isMultiTiered) {
      return (slabIdx << kNumAllocIdxBits) + allocIdx;
    }
    return (static_cast<PtrType>(tid) << (kNumTierIdxOffset + kIdxNumBits)) +
           (slabIdx << kNumAllocIdxBits) + allocIdx;
  }

  static IdxPtrType32 deserializePtr(SerializedPtrType ptr) {
    XDCHECK(ptr >= 0);
    return static_cast<IdxPtrType32>(ptr);
  }

  static RegionPtrType8 deserializeRegion(SerializedPtrType ptr) {
    XDCHECK(ptr >= 0);
    return static_cast<RegionPtrType8>(ptr >> kIdxNumBits);
  }

  static RegionPtrType8 getRegionIdx(uint32_t slabIdx,
                                     bool isMultiTiered,
                                     TierId tid) noexcept {
    if (isMultiTiered) {
      return static_cast<RegionPtrType8>(tid << kNumTierIdxOffset) +
             static_cast<RegionPtrType8>(
                 ((slabIdx >> (kIdxNumBits - kNumAllocIdxBits)) &
                  ~kTierIdxMask));
    }
    return static_cast<RegionPtrType8>(slabIdx >>
                                       (kIdxNumBits - kNumAllocIdxBits));
  }

  // Get the slab index of the compressed ptr
  uint32_t getSlabIdx(bool isMultiTiered) const noexcept {
    XDCHECK(!isNull());
    // This slab index will not contain the tid for CXL tiering cases.
    // TODO (pranav): Maybe problem for CXL use case? Review
    IdxPtrType32 noTierIdPtr =
        isMultiTiered ? static_cast<IdxPtrType32>(regionIdx_) & ~kTierIdxMask
                      : static_cast<IdxPtrType32>(regionIdx_);
    return (noTierIdPtr << (kIdxNumBits - kNumAllocIdxBits)) +
           static_cast<IdxPtrType32>(ptr_ >> kNumAllocIdxBits);
  }

  // Get the allocation index of the compressed ptr
  uint32_t getAllocIdx() const noexcept {
    XDCHECK(!isNull());
    // Note: tid check not required in ptr_ since only
    //       the lower 16 bits are being read here.
    return static_cast<uint32_t>(ptr_ & kAllocIdxMask);
  }

  uint32_t getTierId(bool isMultiTiered) const noexcept {
    XDCHECK(!isNull());
    return isMultiTiered
               ? static_cast<uint32_t>(regionIdx_ >> kNumTierIdxOffset)
               : 0;
  }

  // Taken from CompressedPtr. Why adding here instead of setting?
  // We are trusting users to not call this twice?
  void setTierId(TierId tid) noexcept {
    regionIdx_ += static_cast<uint32_t>(tid) << kNumTierIdxOffset;
  }

  friend SlabAllocator;
  friend class facebook::cachelib::tests::AllocTestBase;
};

template <typename PtrType, typename AllocatorT, typename CompressedPtrType>
class PtrCompressor {
 public:
  explicit PtrCompressor(const AllocatorT& allocator) noexcept
      : allocator_(allocator) {}

  const CompressedPtrType compress(const PtrType* uncompressed) const {
    return allocator_.template compress<CompressedPtrType>(
        uncompressed, false /* isMultiTiered */);
  }

  PtrType* unCompress(const CompressedPtrType& compressed) const {
    return static_cast<PtrType*>(
        allocator_.template unCompress<CompressedPtrType>(
            compressed, false /* isMultiTiered */));
  }

  // Prefetches the memory of a compressed pointer that may be stale.
  void prefetch(const CompressedPtrType& compressed) const noexcept {
    allocator_.template prefetch<CompressedPtrType>(compressed,
                                                    false /* isMultiTiered */);
  }

  bool operator==(const PtrCompressor& rhs) const noexcept {
    return &allocator_ == &rhs.allocator_;
  }

  bool operator!=(const PtrCompressor& rhs) const noexcept {
    return !(*this == rhs);
  }

 private:
  // memory allocator that does the pointer compression.
  const AllocatorT& allocator_;
};
} // namespace cachelib
} // namespace facebook
