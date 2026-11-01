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

#include <folly/Function.h>
#include <folly/Utility.h>
#include <folly/fibers/TimedMutex.h>

#include <chrono>
#include <stdexcept>

#include "cachelib/common/AtomicCounter.h"
#include "cachelib/common/BloomFilter.h"
#include "cachelib/common/PercentileStats.h"
#include "cachelib/common/Profiled.h"
#include "cachelib/navy/bighash/Bucket.h"
#include "cachelib/navy/common/Buffer.h"
#include "cachelib/navy/common/Device.h"
#include "cachelib/navy/common/Hash.h"
#include "cachelib/navy/common/SizeDistribution.h"
#include "cachelib/navy/common/Types.h"
#include "cachelib/navy/engine/Engine.h"
#include "cachelib/navy/serialization/Serialization.h"

namespace facebook {
namespace cachelib {
namespace navy {
// SharedMutex is write priority by default
using SharedMutex =
    folly::fibers::TimedRWMutexWritePriority<folly::fibers::Baton>;

class ValidBucketChecker;

// BigHash is a small item flash-based cache engine. It divides the device into
// a series of buckets. One can think of it as a on-device hash table.
//
// Each item is hashed to a bucket according to its key. There is no size class,
// and each bucket consists of various variable-sized items. When full, we evict
// the items in their insertion order. An eviction call back is guaranteed to be
// invoked once per item. We currently do not support removeCB. That is coming
// as part of Navy eventually.
//
// Each read and write via BigHash happens in `bucketSize` granularity. This
// means, you will read a full bucket even if your item is only 100 bytes.
// It's also the same for writes. This makes BigHash inherently unsuitable for
// large items that will also need large buckets (several KB and above).
//
// However, this design gives us the ability to forgo an in-memory index and
// instead look up our items directly from disk. In practice, this means BigHash
// is a flash engine optimized for small items.
class BigHash final : public Engine, folly::NonCopyableNonMovable {
 public:
  struct Config {
    uint32_t bucketSize{4 * 1024};

    // Offload bucket checksumming to Intel DSA via the DTO library when
    // available. Most beneficial with large buckets (16KB+), where the bloom
    // filter rebuild overlaps with the accelerator computing the checksum.
    // No effect when built without DTO support.
    bool checksumOffload{false};

    // The range of device that BigHash will access is guaranteed to be
    // within [baseOffset, baseOffset + cacheSize)
    uint64_t cacheBaseOffset{};
    uint64_t cacheSize{};
    Device* device{nullptr};

    ExpiredCheck checkExpired;
    DestructorCallback destructorCb;

    // Optional bloom filter to reduce IO
    std::unique_ptr<BloomFilter> bloomFilter;

    uint8_t numMutexesPower{14};

    uint64_t numBuckets() const { return cacheSize / bucketSize; }

    Config& validate();
  };

  // Contructor can throw std::exception if config is invalid.
  //
  // @param config  config that was validated with Config::validate
  //
  // @throw std::invalid_argument on bad config
  explicit BigHash(Config&& config);
  ~BigHash() override = default;

  // Return the size of usable space
  uint64_t getSize() const override { return bucketSize_ * numBuckets_; }

  // Check if the key could exist in bighash. This can be used as a pre-check
  // to optimize cache lookups to avoid calling lookups in an async IO
  // environment.
  //
  // @param hk   key to be checked
  //
  // @return  false if the key definitely does not exist and true if it could.
  bool couldExist(HashedKey hk) override;

  uint64_t estimateWriteSize(HashedKey, BufferView) const override;

  // Fixed once the bucket count and bloom filter are sized; reset() caches it.
  uint64_t estimatePersistSize() const;

  // Look up a key in BigHash. On success, it will return Status::Ok and
  // populate "value" with the value found. User should pass in a null
  // Buffer as "value" as any existing storage will be freed. If not found,
  // it will return Status::NotFound. And of course, on error, it returns
  // DeviceError.
  // BigHash does not track per-item access time; lastAccessTimeSecs is
  // always set to 0.
  Status lookup(HashedKey hk,
                Buffer& value,
                uint32_t& lastAccessTimeSecs) override;

  // Inserts key and value into BigHash. This will replace an existing
  // key if found. If it failed to write, it will return DeviceError.
  Status insert(HashedKey hk,
                BufferView value,
                uint8_t poolId = 0,
                uint32_t expiryTime = 0,
                uint32_t lastAccessTimeSecs = 0) override;

  // Removes an entry from BigHash if found. Ok on success, NotFound on miss,
  // and DeviceError on error.
  Status remove(HashedKey hk) override;

  // flush the device file
  void flush() override;

  // reset BigHash, this clears the bloom filter and all stats
  // data is invalidated even it is not physically removed.
  void reset() override;

  // serialize BigHash state to a RecordWriter
  void persist(RecordWriter& rw) override;

  // deserialize BigHash state from a RecordReader
  // @return true if recovery succeed, false o/w.
  bool recover(RecordReader& rr) override;

  // returns BigHash stats to the visitor
  uint64_t getCounters(const CounterVisitor& visitor) const override;

  // return the maximum allowed item size
  uint64_t getMaxItemSize() const override;

  // return how manu times a lookup is rejected by the bloom filter
  uint64_t bfRejectCount() const { return bfRejectCount_.get(); }

  // return a Buffer containing NvmItem randomly sampled in the backing store
  std::pair<Status, std::string /* key */> getRandomAlloc(
      Buffer& value) override;

  // Update any stats needed to be updated when eviction is done
  void updateEvictionStats(uint32_t lifetime) override {
    bhLifetimeSecs_.trackValue(lifetime);
  }

 private:
  class BucketId {
   public:
    explicit BucketId(uint32_t idx) : idx_{idx} {}

    bool operator==(const BucketId& rhs) const noexcept {
      return idx_ == rhs.idx_;
    }
    bool operator!=(const BucketId& rhs) const noexcept {
      return !(*this == rhs);
    }

    uint32_t index() const noexcept { return idx_; }

   private:
    uint32_t idx_;
  };

  struct ValidConfigTag {};
  BigHash(Config&& config, ValidConfigTag);

  Buffer readBucket(BucketId bid);

  // Checksums the bucket and writes it to the device. @overlap is CPU work
  // (e.g. bloom filter maintenance) run while the checksum is computed by
  // DSA when checksum offload is enabled, or immediately before the software
  // checksum otherwise. It may read the bucket but must not modify it.
  bool writeBucket(
      BucketId bid, Buffer buffer, folly::FunctionRef<void()> overlap = [] {});

  // The corresponding r/w bucket lock must be held during the entire
  // duration of the read and write operations. For example, during write,
  // if write lock is dropped after a bucket is read from device, user
  // must re-acquire the write lock and re-read the bucket from device
  // again to ensure they have the newest content. Otherwise, one thread
  // could overwrite another's writes.
  //
  // In short, just hold the lock during the entire operation!
  auto& getMutex(BucketId bid) const {
    return mutex_[bid.index() & (numMutexes_ - 1)];
  }

  folly::SpinLock& getBfLock(BucketId bid) const {
    return bfLock_[bid.index() & (numMutexes_ - 1)];
  }

  BucketId getBucketId(HashedKey hk) const {
    return BucketId{static_cast<uint32_t>(hk.keyHash() % numBuckets_)};
  }

  uint64_t getBucketOffset(BucketId bid) const {
    return cacheBaseOffset_ + bucketSize_ * bid.index();
  }

  double bfFalsePositivePct() const;
  void bfSet(BucketId bid, uint64_t bucket);
  void bfClear(BucketId bid);
  void bfRebuild(BucketId bid, const Bucket* bucket);
  bool bfReject(BucketId bid, uint64_t keyHash) const;

  // Number of mutexes for bucket locking
  const size_t numMutexes_;

  // Serialization format version. Never 0. Versions < 10 reserved for testing.
  // Version 11: navy::checksum switched from CRC-32 (IEEE) to CRC-32C
  // (Castagnoli) for DSA offload compatibility. Buckets written by prior
  // versions would fail checksum verification.
  static constexpr uint32_t kFormatVersion = 11;

  const ExpiredCheck checkExpired_{};
  const DestructorCallback destructorCb_{};
  const uint64_t bucketSize_{};
  // Whether to offload bucket checksumming to DSA. See Config::checksumOffload.
  const bool checksumOffload_{};
  const uint64_t cacheBaseOffset_{};
  const uint64_t numBuckets_{};
  std::unique_ptr<BloomFilter> bloomFilter_;
  std::unique_ptr<ValidBucketChecker> validBucketChecker_;
  std::chrono::nanoseconds generationTime_{};
  Device& device_;
  // handle for data placement technologies like FDP
  int placementHandle_;
  mutable std::vector<trace::Profiled<SharedMutex, "cachelib:navy:bh">> mutex_;
  // Spinlocks for bloom filter operations
  // We use spinlock in addition to the mutex to avoid contentions of
  // couldExist which needs to be fast against other long running or
  // even blocking operations including insert/remove. When the race
  // happens against the remove or evict of the given item, there could
  // be a false positive which is ok.
  // Nested lock orders are always mutex-then-spinlock
  mutable std::vector<folly::SpinLock> bfLock_;

  // thread local counters in synchronized path
  mutable TLCounter lookupCount_;
  mutable TLCounter bfProbeCount_;
  mutable TLCounter bfRejectCount_;

  // atomic counters in asynchronous path
  mutable AtomicCounter itemCount_;
  mutable AtomicCounter insertCount_;
  mutable AtomicCounter succInsertCount_;
  mutable AtomicCounter succLookupCount_;
  mutable AtomicCounter removeCount_;
  mutable AtomicCounter succRemoveCount_;
  mutable AtomicCounter evictionCount_;
  mutable AtomicCounter evictionExpiredCount_;
  mutable AtomicCounter logicalWrittenCount_;
  mutable AtomicCounter physicalWrittenCount_;
  mutable AtomicCounter ioErrorCount_;
  mutable AtomicCounter bfFalsePositiveCount_;
  mutable AtomicCounter bfRebuildCount_;
  mutable AtomicCounter checksumErrorCount_;
  mutable AtomicCounter usedSizeBytes_;
  mutable AtomicCounter persistSizeEstimate_;
  mutable AtomicCounter disabledBucketLookup_;
  mutable AtomicCounter disabledBucketInsert_;
  mutable AtomicCounter disabledBucketRemove_;
  // counters to quantify the expired eviction overhead (temporary)
  // PercentileStats generates outputs in integers, so amplify by 100x
  mutable util::PercentileStats bucketExpirationsDist_x100_;
  mutable util::PercentileStats bhLifetimeSecs_;
  mutable util::PercentileStats lookupLatency_;
  mutable util::PercentileStats insertLatency_;

  friend class ValidBucketChecker;
};

// Helper to determine which bucket is valid and can be served from BigHash
class ValidBucketChecker {
 public:
  explicit ValidBucketChecker(uint32_t numBuckets, uint32_t numBucketsPerBit)
      : numBuckets_{numBuckets},
        numBucketsPerBit_{numBucketsPerBit},
        numBytes_{getBytes(numBuckets, numBucketsPerBit)},
        bits_{std::make_unique<uint8_t[]>(numBytes_)} {
    XLOGF(INFO,
          "For ValidBucketChecker, allocating {} bytes for {} buckets at {} "
          "buckets per bit.",
          numBytes_,
          numBuckets_,
          numBucketsPerBit_);
  }

  serialization::ValidBucketCheckerState persist() {
    serialization::ValidBucketCheckerState state;
    state.numBuckets() = numBuckets_;
    state.numBucketsPerBit() = numBucketsPerBit_;
    state.numDisabledBuckets() = numDisabledBuckets_.get();
    state.bytes()->reserve(numBytes_);
    for (uint32_t i = 0; i < numBytes_; ++i) {
      state.bytes()->push_back(bits_[i]);
    }
    return state;
  }

  bool recover(const serialization::ValidBucketCheckerState& state) {
    if (static_cast<uint32_t>(*state.numBuckets()) != numBuckets_ ||
        static_cast<uint32_t>(*state.numBucketsPerBit()) != numBucketsPerBit_ ||
        state.bytes()->size() != numBytes_) {
      XLOGF(ERR,
            "Failed to recover ValidBucketChecker. Expected {} buckets, "
            "{} per bit, but got {}, {}",
            numBuckets_,
            numBucketsPerBit_,
            *state.numBuckets(),
            *state.numBucketsPerBit());
      return false;
    }

    numDisabledBuckets_.set(*state.numDisabledBuckets());

    for (uint32_t i = 0; i < numBytes_; ++i) {
      *(bits_.get() + i) = (*state.bytes())[i];
    }

    return true;
  }

  uint32_t numDisabledBuckets() const { return numDisabledBuckets_.get(); }

  // Size of the bitmap, which persist() emits as a list<byte>.
  uint32_t numBytes() const { return numBytes_; }

  bool isBucketValid(uint32_t idx) const {
    XDCHECK_GE(numBuckets_, idx);
    uint32_t numBits = idx / numBucketsPerBit_;
    auto* byte = bits_.get() + numBits / 8;
    return !util::bitGet(byte, numBits % 8);
  }

  void disableBucket(uint32_t idx) {
    XDCHECK_GE(numBuckets_, idx);
    uint32_t numBits = idx / numBucketsPerBit_;
    auto* byte = bits_.get() + numBits / 8;
    util::bitSet(byte, numBits % 8);
  }

 private:
  static uint32_t getBytes(uint32_t numBuckets, uint32_t numBucketsPerBit) {
    // Align up the bits so we have enough to cover all buckets
    const uint32_t numBits =
        (numBuckets + numBucketsPerBit - 1) / numBucketsPerBit;
    // Align up the bytes so we have enough to cover all bytes
    const uint32_t numBytes = (numBits + 7) / 8;
    return numBytes;
  }

  const uint32_t numBuckets_{};
  const uint32_t numBucketsPerBit_{};
  const uint32_t numBytes_{};

  // 1 means disabled. 0 means active.
  std::unique_ptr<uint8_t[]> bits_;

  // # of buckets disabled since BigHash has been created.
  // This is persisted across restarts.
  AtomicCounter numDisabledBuckets_;
};
} // namespace navy
} // namespace cachelib
} // namespace facebook
