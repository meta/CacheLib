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

#include "cachelib/navy/bighash/BigHash.h"

#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/io/RecordIO.h>

#include <chrono>

#include "cachelib/common/Hash.h"
#include "cachelib/navy/bighash/Bucket.h"
#include "cachelib/navy/common/Hash.h"
#include "cachelib/navy/common/Utils.h"

namespace facebook::cachelib::navy {
constexpr uint32_t kBigHashValidBucketCheckerBucketsPerBit = 100;

BigHash::Config& BigHash::Config::validate() {
  if (cacheSize < bucketSize) {
    throw std::invalid_argument(
        fmt::format("cache size: {} cannot be smaller than bucket size: {}",
                    cacheSize,
                    bucketSize));
  }

  if (!folly::isPowTwo(bucketSize)) {
    throw std::invalid_argument(
        fmt::format("invalid bucket size: {}", bucketSize));
  }

  if (cacheSize > uint64_t{bucketSize} << 32) {
    throw std::invalid_argument(fmt::format(
        "Can't address big hash with 32 bits. Cache size: {}, bucket size: {}",
        cacheSize,
        bucketSize));
  }

  if (cacheBaseOffset % bucketSize != 0 || cacheSize % bucketSize != 0) {
    throw std::invalid_argument(fmt::format(
        "cacheBaseOffset and cacheSize need to be a multiple of bucketSize. "
        "cacheBaseOffset: {}, cacheSize:{}, bucketSize: {}.",
        cacheBaseOffset,
        cacheSize,
        bucketSize));
  }

  if (device == nullptr) {
    throw std::invalid_argument("device cannot be null");
  }

  if (bloomFilter && bloomFilter->numFilters() != numBuckets()) {
    throw std::invalid_argument(
        fmt::format("bloom filter #filters mismatch #buckets: {} vs {}",
                    bloomFilter->numFilters(),
                    numBuckets()));
  }
  return *this;
}

BigHash::BigHash(Config&& config)
    : BigHash{std::move(config.validate()), ValidConfigTag{}} {}

BigHash::BigHash(Config&& config, ValidConfigTag)
    : numMutexes_(size_t{1} << config.numMutexesPower),
      checkExpired_(std::move(config.checkExpired)),
      destructorCb_{[cb = std::move(config.destructorCb)](
                        HashedKey hk, BufferView value, DestructorEvent event) {
        if (cb) {
          cb(hk, value, event);
        }
      }},
      bucketSize_{config.bucketSize},
      checksumOffload_{config.checksumOffload},
      cacheBaseOffset_{config.cacheBaseOffset},
      numBuckets_{config.numBuckets()},
      bloomFilter_{std::move(config.bloomFilter)},
      device_{*config.device},
      placementHandle_{device_.allocatePlacementHandle()},
      mutex_(numMutexes_),
      bfLock_(numMutexes_) {
  XLOGF(INFO,
        "BigHash created: buckets: {}, bucket size: {}, base offset: {}, num "
        "mutexes: {}",
        numBuckets_,
        bucketSize_,
        cacheBaseOffset_,
        numMutexes_);
  reset();
}

void BigHash::reset() {
  XLOG(INFO, "Reset BigHash");
  generationTime_ = getSteadyClock();

  if (bloomFilter_) {
    bloomFilter_->reset();
  }

  validBucketChecker_ = std::make_unique<ValidBucketChecker>(
      numBuckets_, kBigHashValidBucketCheckerBucketsPerBit);
  persistSizeEstimate_.set(estimatePersistSize());

  itemCount_.set(0);
  insertCount_.set(0);
  succInsertCount_.set(0);
  lookupCount_.set(0);
  succLookupCount_.set(0);
  removeCount_.set(0);
  succRemoveCount_.set(0);
  evictionCount_.set(0);
  evictionExpiredCount_.set(0);
  logicalWrittenCount_.set(0);
  physicalWrittenCount_.set(0);
  ioErrorCount_.set(0);
  bfFalsePositiveCount_.set(0);
  bfProbeCount_.set(0);
  checksumErrorCount_.set(0);
  usedSizeBytes_.set(0);
}

double BigHash::bfFalsePositivePct() const {
  const auto probes = bfProbeCount_.get();
  if (bloomFilter_ && probes > 0) {
    return 100.0 * bfFalsePositiveCount_.get() / probes;
  } else {
    return 0;
  }
}

uint64_t BigHash::getMaxItemSize() const {
  auto itemOverhead = BucketStorage::slotSize(sizeof(details::BucketEntry));
  return bucketSize_ - sizeof(Bucket) - itemOverhead;
}

std::pair<Status, std::string> BigHash::getRandomAlloc(Buffer& value) {
  BucketId bid(folly::Random::rand64(0, numBuckets_));

  Bucket* bucket{nullptr};
  Buffer buffer;
  {
    std::unique_lock lock{getMutex(bid)};
    buffer = readBucket(bid);
    if (buffer.isNull()) {
      ioErrorCount_.inc();
      return std::make_pair(Status::NotFound, "");
    }

    bucket = reinterpret_cast<Bucket*>(buffer.data());
  }

  auto [key, valueView] = bucket->getRandomAlloc();
  if (key.empty() || valueView.isNull()) {
    return std::make_pair(Status::NotFound, "");
  }

  value = Buffer{valueView};
  return std::make_pair(Status::Ok, key);
}

uint64_t BigHash::getCounters(const CounterVisitor& visitor) const {
  const uint64_t persistSizeEstimate = persistSizeEstimate_.get();
  visitor("navy_bh_size", getSize());
  visitor("navy_bh_items", itemCount_.get());
  visitor("navy_bh_inserts", insertCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_succ_inserts",
          succInsertCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_lookups", lookupCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_succ_lookups",
          succLookupCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_removes", removeCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_succ_removes",
          succRemoveCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_evictions",
          evictionCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_evictions_expired",
          evictionExpiredCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_logical_written",
          logicalWrittenCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_physical_written",
          physicalWrittenCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_io_errors",
          ioErrorCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_bf_false_positive_pct", bfFalsePositivePct());
  visitor("navy_bh_bf_lookups",
          bfProbeCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_bf_rebuilds",
          bfRebuildCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_metadata_est_bytes", persistSizeEstimate);
  visitor("navy_bh_checksum_errors",
          checksumErrorCount_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_used_size_bytes", usedSizeBytes_.get());
  visitor("navy_bh_disabled_bucket_lookup", disabledBucketLookup_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_disabled_bucket_insert", disabledBucketInsert_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_disabled_bucket_remove", disabledBucketRemove_.get(),
          CounterVisitor::CounterType::RATE);
  visitor("navy_bh_disabled_buckets",
          validBucketChecker_->numDisabledBuckets());
  bucketExpirationsDist_x100_.visitQuantileEstimator(
      visitor, "navy_bh_expired_loop_x100");
  bhLifetimeSecs_.visitQuantileEstimator(visitor, "navy_bh_item_lifetime_secs");
  lookupLatency_.visitQuantileEstimator(visitor, "navy_bh_lookup_latency_us");
  insertLatency_.visitQuantileEstimator(visitor, "navy_bh_insert_latency_us");

  return persistSizeEstimate;
}

uint64_t BigHash::estimatePersistSize() const {
  // Mirrors persist() below.
  constexpr uint64_t kHeaderBytes = folly::recordio_helpers::headerSize();

  // The checker bitmap ships inside BigHashPersistentData as a list<byte>: one
  // wire byte per element.
  serialization::BigHashPersistentData pd;
  uint64_t size =
      kHeaderBytes + serializedProtoSize(pd) + validBucketChecker_->numBytes();

  if (bloomFilter_) {
    size += bloomFilter_->estimatePersistSize<ProtoSerializer>(kHeaderBytes);
  }

  return size;
}

void BigHash::persist(RecordWriter& rw) {
  XLOG(INFO, "Starting bighash persist");
  serialization::BigHashPersistentData pd;
  *pd.version() = kFormatVersion;
  *pd.generationTime() = generationTime_.count();
  *pd.itemCount() = itemCount_.get();
  *pd.bucketSize() = bucketSize_;
  *pd.cacheBaseOffset() = cacheBaseOffset_;
  *pd.numBuckets() = numBuckets_;
  *pd.usedSizeBytes() = usedSizeBytes_.get();
  *pd.validBucketCheckerState() = validBucketChecker_->persist();
  serializeProto(pd, rw);

  if (bloomFilter_) {
    bloomFilter_->persist<ProtoSerializer>(rw);
    XLOG(INFO, "bloom filter persist done");
  }

  XLOG(INFO, "Finished bighash persist");
}

bool BigHash::recover(RecordReader& rr) {
  XLOG(INFO, "Starting bighash recovery");
  try {
    auto pd = deserializeProto<serialization::BigHashPersistentData>(rr);
    if (*pd.version() != kFormatVersion) {
      throw std::logic_error{
          fmt::format("invalid format version {}, expected {}",
                      *pd.version(),
                      kFormatVersion)};
    }

    auto configEquals =
        static_cast<uint64_t>(*pd.bucketSize()) == bucketSize_ &&
        static_cast<uint64_t>(*pd.cacheBaseOffset()) == cacheBaseOffset_ &&
        static_cast<uint64_t>(*pd.numBuckets()) == numBuckets_;
    if (!configEquals) {
      auto configStr = serializeToJson(pd);
      XLOGF(ERR, "Recovery config: {}", configStr.c_str());
      throw std::logic_error{"config mismatch"};
    }

    generationTime_ = std::chrono::nanoseconds{*pd.generationTime()};
    itemCount_.set(*pd.itemCount());
    usedSizeBytes_.set(*pd.usedSizeBytes());
    if (bloomFilter_) {
      bloomFilter_->recover<ProtoSerializer>(rr);
      XLOG(INFO, "Recovered bloom filter");
    }

    if (!validBucketChecker_->recover(*pd.validBucketCheckerState())) {
      throw std::logic_error{"failed to recover valid bucket checker"};
    }
    XLOGF(INFO, "Recovered valid bucket checker. {} buckets disabled.",
          validBucketChecker_->numDisabledBuckets());
  } catch (const std::exception& e) {
    XLOGF(ERR, "Exception: {}", e.what());
    XLOG(ERR, "Failed to recover bighash. Resetting cache.");

    reset();
    return false;
  }
  XLOG(INFO, "Finished bighash recovery");
  return true;
}

Status BigHash::insert(HashedKey hk,
                       BufferView value,
                       uint8_t /* poolId */,
                       uint32_t /* expiryTime */,
                       uint32_t /* lastAccessTimeSecs */) {
  auto start = getSteadyClock();
  SCOPE_EXIT {
    insertLatency_.trackValue(toMicros(getSteadyClock() - start).count());
  };
  const auto bid = getBucketId(hk);
  insertCount_.inc();

  if (!validBucketChecker_->isBucketValid(bid.index())) {
    disabledBucketInsert_.inc();
    return Status::Rejected;
  }

  uint32_t removed{0};
  uint32_t evicted{0};
  uint32_t evictExpired{0};

  uint32_t oldRemainingBytes = 0;
  uint32_t newRemainingBytes = 0;

  // we copy the items and trigger the destructorCb after bucket lock is
  // released to avoid possible heavy operations or locks in the destructor.
  std::vector<std::tuple<Buffer, Buffer, DestructorEvent>> removedItems;
  DestructorCallback cb = [&removedItems](HashedKey key, BufferView val,
                                          DestructorEvent event) {
    // must make a copy for the key, o/w data might be deleted
    removedItems.emplace_back(Buffer{makeView(key.key())}, val, event);
  };

  {
    std::unique_lock lock{getMutex(bid)};
    auto buffer = readBucket(bid);
    if (buffer.isNull()) {
      ioErrorCount_.inc();
      return Status::DeviceError;
    }

    auto* bucket = reinterpret_cast<Bucket*>(buffer.data());
    oldRemainingBytes = bucket->remainingBytes();
    removed = bucket->remove(hk, cb);
    std::tie(evicted, evictExpired) =
        bucket->insert(hk, value, checkExpired_, cb);
    newRemainingBytes = bucket->remainingBytes();

    // The bloom filter update runs as overlap work: concurrently with the
    // bucket checksum when offloaded to DSA, right before it otherwise. In
    // both cases it completes before the bucket is written to the device.
    const auto res = writeBucket(bid, std::move(buffer), [&] {
      if (removed + evicted == 0) {
        // In case nothing was removed or evicted, we can just add
        bfSet(bid, hk.keyHash());
      } else {
        bfRebuild(bid, bucket);
      }
    });
    if (!res) {
      bfClear(bid);
      ioErrorCount_.inc();
      return Status::DeviceError;
    }
  }

  for (const auto& item : removedItems) {
    destructorCb_(makeHK(std::get<0>(item)) /* key */,
                  std::get<1>(item).view() /* value */,
                  std::get<2>(item) /* event */);
  }

  if (oldRemainingBytes < newRemainingBytes) {
    usedSizeBytes_.sub(newRemainingBytes - oldRemainingBytes);
  } else {
    usedSizeBytes_.add(oldRemainingBytes - newRemainingBytes);
  }
  itemCount_.add(1);
  itemCount_.sub(evicted + removed);
  evictionCount_.add(evicted);
  evictionExpiredCount_.add(evictExpired);
  if (evictExpired > 0) {
    bucketExpirationsDist_x100_.trackValue(evictExpired * 100);
  }
  logicalWrittenCount_.add(hk.key().size() + value.size());
  physicalWrittenCount_.add(bucketSize_);
  succInsertCount_.inc();
  return Status::Ok;
}

bool BigHash::couldExist(HashedKey hk) {
  const auto bid = getBucketId(hk);
  bool canExist = !bfReject(bid, hk.keyHash());

  // the caller is not likely to issue a subsequent lookup when we return
  // false. hence tag this as a lookup. If we return the key can exist, the
  // caller will perform a lookupAsync and will be counted within lookup api.
  if (!canExist) {
    lookupCount_.inc();
  }
  return canExist;
}

uint64_t BigHash::estimateWriteSize(HashedKey, BufferView) const {
  return bucketSize_;
}

Status BigHash::lookup(HashedKey hk,
                       Buffer& value,
                       uint32_t& lastAccessTimeSecs) {
  auto start = getSteadyClock();
  SCOPE_EXIT {
    lookupLatency_.trackValue(toMicros(getSteadyClock() - start).count());
  };
  lastAccessTimeSecs = 0;
  const auto bid = getBucketId(hk);
  lookupCount_.inc();

  if (!validBucketChecker_->isBucketValid(bid.index())) {
    disabledBucketLookup_.inc();
    return Status::NotFound;
  }

  Bucket* bucket{nullptr};
  Buffer buffer;

  // scope of the lock is only needed until we read and mutate state for the
  // bucket. Once the bucket is read, the buffer is local and we can find
  // without holding the lock.
  {
    std::shared_lock lock{getMutex(bid)};
    if (bfReject(bid, hk.keyHash())) {
      return Status::NotFound;
    }

    buffer = readBucket(bid);
    if (buffer.isNull()) {
      ioErrorCount_.inc();
      return Status::DeviceError;
    }

    bucket = reinterpret_cast<Bucket*>(buffer.data());
  }

  auto valueView = bucket->find(hk);
  if (valueView.isNull()) {
    bfFalsePositiveCount_.inc();
    return Status::NotFound;
  }
  value = Buffer{valueView};
  succLookupCount_.inc();
  return Status::Ok;
}

Status BigHash::remove(HashedKey hk) {
  const auto bid = getBucketId(hk);
  removeCount_.inc();

  if (!validBucketChecker_->isBucketValid(bid.index())) {
    disabledBucketRemove_.inc();
    return Status::NotFound;
  }

  uint32_t oldRemainingBytes = 0;
  uint32_t newRemainingBytes = 0;

  // we copy the items and trigger the destructorCb after bucket lock is
  // released to avoid possible heavy operations or locks in the destructor.
  Buffer valueCopy;
  DestructorCallback cb = [&valueCopy](HashedKey, BufferView value,
                                       DestructorEvent) {
    valueCopy = Buffer{value};
  };

  if (bfReject(bid, hk.keyHash())) {
    return Status::NotFound;
  }

  {
    std::unique_lock lock{getMutex(bid)};

    auto buffer = readBucket(bid);
    if (buffer.isNull()) {
      ioErrorCount_.inc();
      return Status::DeviceError;
    }

    auto* bucket = reinterpret_cast<Bucket*>(buffer.data());
    oldRemainingBytes = bucket->remainingBytes();
    if (!bucket->remove(hk, cb)) {
      bfFalsePositiveCount_.inc();
      return Status::NotFound;
    }
    newRemainingBytes = bucket->remainingBytes();

    // The bloom filter rebuild runs as overlap work: concurrently with the
    // bucket checksum when offloaded to DSA, right before it otherwise. In
    // both cases it completes before the bucket is written to the device.
    const auto res =
        writeBucket(bid, std::move(buffer), [&] { bfRebuild(bid, bucket); });
    if (!res) {
      bfClear(bid);
      ioErrorCount_.inc();
      return Status::DeviceError;
    }
  }

  if (!valueCopy.isNull()) {
    destructorCb_(hk, valueCopy.view(), DestructorEvent::Removed);
  }

  XDCHECK_LE(oldRemainingBytes, newRemainingBytes);
  usedSizeBytes_.sub(newRemainingBytes - oldRemainingBytes);
  itemCount_.dec();

  // We do not bump logicalWrittenCount_ because logically a
  // remove operation does not write, but for BigHash, it does
  // incur physical writes.
  physicalWrittenCount_.add(bucketSize_);
  succRemoveCount_.inc();
  return Status::Ok;
}

inline void BigHash::bfSet(BucketId bid, uint64_t keyHash) {
  if (!bloomFilter_) {
    return;
  }

  std::lock_guard<folly::SpinLock> lg{getBfLock(bid)};
  bloomFilter_->set(bid.index(), keyHash);
}

inline void BigHash::bfClear(BucketId bid) {
  if (!bloomFilter_) {
    return;
  }

  std::lock_guard<folly::SpinLock> lg{getBfLock(bid)};
  bloomFilter_->clear(bid.index());
}

bool BigHash::bfReject(BucketId bid, uint64_t keyHash) const {
  if (!bloomFilter_) {
    return false;
  }

  std::lock_guard<folly::SpinLock> lg{getBfLock(bid)};
  bfProbeCount_.inc();
  if (!bloomFilter_->couldExist(bid.index(), keyHash)) {
    bfRejectCount_.inc();
    return true;
  }
  return false;
}

void BigHash::bfRebuild(BucketId bid, const Bucket* bucket) {
  if (!bloomFilter_) {
    return;
  }

  std::lock_guard<folly::SpinLock> lg{getBfLock(bid)};
  bfRebuildCount_.inc();
  XDCHECK(bloomFilter_);
  bloomFilter_->clear(bid.index());
  auto itr = bucket->getFirst();
  while (!itr.done()) {
    bloomFilter_->set(bid.index(), itr.keyHash());
    itr = bucket->getNext(itr);
  }
}

void BigHash::flush() {
  XLOG(INFO, "Flush big hash");
  device_.flush();
}

Buffer BigHash::readBucket(BucketId bid) {
  auto buffer = device_.makeIOBuffer(bucketSize_);
  XDCHECK(!buffer.isNull());

  const bool res =
      device_.read(getBucketOffset(bid), buffer.size(), buffer.data());
  if (!res) {
    validBucketChecker_->disableBucket(bid.index());
    return {};
  }

  auto* bucket = reinterpret_cast<Bucket*>(buffer.data());

  auto checksumCheck = [this](auto* b, auto bufferView) {
    const uint32_t computed = checksumOffload_
                                  ? Bucket::computeChecksumOffloaded(
                                        bufferView, [] {})
                                  : Bucket::computeChecksum(bufferView);
    const bool checksumSuccess = computed == b->getChecksum();
    // TODO (T93631284) we only read a bucket if the bloom filter indicates that
    // the bucket could have the element. Hence, if check sum errors out and
    // bloom filter is enabled, we could record the checksum error. However,
    // doing so could lead to false positives on check sum errors for buckets
    // that were not initialized (by writing to it), but were read due to bloom
    // filter having a false positive.  Hence, we can't differentiate between
    // false positives and real check sum errors due device failures
    //
    // if (!checksumSuccess && bloomFilter_) {
    //  validBucketChecker_->disableBucket(bid.index());
    //  checksumErrorCount_.inc();
    // }
    return checksumSuccess;
  };

  if (static_cast<uint64_t>(generationTime_.count()) !=
          bucket->generationTime() ||
      !checksumCheck(bucket, buffer.view())) {
    Bucket::initNew(buffer.mutableView(), generationTime_.count());
  }
  return buffer;
}

bool BigHash::writeBucket(BucketId bid,
                          Buffer buffer,
                          folly::FunctionRef<void()> overlap) {
  auto* bucket = reinterpret_cast<Bucket*>(buffer.data());
  if (checksumOffload_) {
    bucket->setChecksum(
        Bucket::computeChecksumOffloaded(buffer.view(), overlap));
  } else {
    overlap();
    bucket->setChecksum(Bucket::computeChecksum(buffer.view()));
  }
  const bool res =
      device_.write(getBucketOffset(bid), std::move(buffer), placementHandle_);
  if (!res) {
    validBucketChecker_->disableBucket(bid.index());
  }
  return res;
}
} // namespace facebook::cachelib::navy
