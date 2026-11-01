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

#include <folly/Random.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "cachelib/common/Hash.h"
#include "cachelib/common/Utils.h"
#include "cachelib/navy/bighash/BigHash.h"
#include "cachelib/navy/driver/Driver.h"
#include "cachelib/navy/testing/BufferGen.h"
#include "cachelib/navy/testing/Callbacks.h"
#include "cachelib/navy/testing/MockDevice.h"
#include "cachelib/navy/testing/SeqPoints.h"

using testing::_;
using testing::AtLeast;
using testing::InSequence;
using testing::NiceMock;
using testing::Return;
using testing::StrictMock;

namespace facebook::cachelib::navy::tests {
namespace {
void setLayout(BigHash::Config& config, uint32_t bs, uint32_t numBuckets) {
  config.bucketSize = bs;
  config.cacheSize = uint64_t{bs} * numBuckets;
}

// Generate key for given bucket index
std::string genKey(uint32_t num_buckets, uint32_t bid) {
  char keyBuf[64];
  while (true) {
    auto id = folly::Random::rand32();
    sprintf(keyBuf, "key_%08X", id);
    HashedKey hk(keyBuf);
    if ((hk.keyHash() % num_buckets) == bid) {
      break;
    }
  }
  return keyBuf;
}
} // namespace

TEST(BigHash, InsertAndRemove) {
  BigHash::Config config;
  setLayout(config, 128, 2);
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 128);
  config.device = device.get();

  BigHash bh(std::move(config));

  Buffer value;
  uint32_t lat = 0;
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("key"), value, lat));

  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok, bh.lookup(makeHK("key"), value, lat));
  EXPECT_EQ(makeView("12345"), value.view());

  EXPECT_EQ(Status::Ok, bh.remove(makeHK("key")));
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("key"), value, lat));
  EXPECT_EQ(Status::NotFound, bh.remove(makeHK("key")));
}

// Exercises the offloaded bucket-checksum path (DSA when built with DTO
// support, software otherwise), including the bloom-filter-rebuild overlap
// in insert/remove and offloaded verification in readBucket.
TEST(BigHash, InsertRemoveChecksumOffload) {
  BigHash::Config config;
  setLayout(config, 128, 2);
  config.checksumOffload = true;
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 128);
  config.device = device.get();

  BigHash bh(std::move(config));

  Buffer value;
  uint32_t lat = 0;
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok, bh.lookup(makeHK("key"), value, lat));
  EXPECT_EQ(makeView("12345"), value.view());

  EXPECT_EQ(Status::Ok, bh.remove(makeHK("key")));
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("key"), value, lat));
}

// without bloom filters, could exist always returns true.
TEST(BigHash, CouldExistWithoutBF) {
  BigHash::Config config;
  setLayout(config, 128, 2);
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 128);
  config.device = device.get();

  BigHash bh(std::move(config));

  Buffer value;
  uint32_t lat = 0;
  EXPECT_EQ(true, bh.couldExist(makeHK("key")));
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("key"), value, lat));

  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(true, bh.couldExist(makeHK("key")));
  EXPECT_EQ(Status::Ok, bh.remove(makeHK("key")));
  EXPECT_EQ(true, bh.couldExist(makeHK("key")));
}

TEST(BigHash, CouldExistWithBF) {
  BigHash::Config config;
  size_t bucketCount = 2;
  setLayout(config, 128, bucketCount);
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 128);
  config.device = device.get();

  auto bf = std::make_unique<BloomFilter>(bucketCount, 2, 4);
  config.bloomFilter = std::move(bf);

  BigHash bh(std::move(config));

  Buffer value;
  uint32_t lat = 0;
  // bloom filter is initially un-initialized. So it will only return true for
  // keys that were inserted.
  EXPECT_EQ(false, bh.couldExist(makeHK("key")));
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("key"), value, lat));

  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(true, bh.couldExist(makeHK("key")));
  EXPECT_EQ(Status::Ok, bh.remove(makeHK("key")));
  EXPECT_EQ(false, bh.couldExist(makeHK("key")));
}

TEST(BigHash, SimpleStats) {
  BigHash::Config config;
  setLayout(config, 64, 1);
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 64);
  config.device = device.get();

  BigHash bh(std::move(config));

  Buffer value;
  uint32_t lat = 0;
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("key"), value, lat));
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  {
    MockCounterVisitor helper;
    EXPECT_CALL(helper, call(_, _)).Times(AtLeast(0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_items"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_inserts"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_succ_inserts"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_lookups"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_succ_lookups"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_removes"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_succ_removes"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_evictions"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_logical_written"), 8));
    EXPECT_CALL(helper, call(strPiece("navy_bh_physical_written"), 64));
    EXPECT_CALL(helper, call(strPiece("navy_bh_io_errors"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_bf_false_positive_pct"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_checksum_errors"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_used_size_bytes"), 28));
    bh.getCounters({toCallback(helper)});
  }

  EXPECT_EQ(Status::Ok, bh.remove(makeHK("key")));
  {
    MockCounterVisitor helper;
    EXPECT_CALL(helper, call(_, _)).Times(AtLeast(0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_items"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_inserts"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_succ_inserts"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_lookups"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_succ_lookups"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_removes"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_succ_removes"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_evictions"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_logical_written"), 8));
    EXPECT_CALL(helper, call(strPiece("navy_bh_physical_written"), 128));
    EXPECT_CALL(helper, call(strPiece("navy_bh_io_errors"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_bf_false_positive_pct"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_checksum_errors"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_used_size_bytes"), 0));
    bh.getCounters({toCallback(helper)});
  }
}

TEST(BigHash, LatencyStats) {
  BigHash::Config config;
  setLayout(config, 64, 1);
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 64);
  config.device = device.get();

  BigHash bh(std::move(config));

  Buffer value;
  uint32_t lat = 0;
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok, bh.lookup(makeHK("key"), value, lat));

  MockCounterVisitor helper;
  EXPECT_CALL(helper, call(_, _)).Times(AtLeast(0));
  EXPECT_CALL(helper, call(strPiece("navy_bh_lookup_latency_us_p99"), _));
  EXPECT_CALL(helper, call(strPiece("navy_bh_insert_latency_us_p99"), _));
  bh.getCounters({toCallback(helper)});
}

TEST(BigHash, EvictionStats) {
  BigHash::Config config;
  setLayout(config, 64, 1);
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 64);
  config.device = device.get();

  BigHash bh(std::move(config));

  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key1"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key2"), makeView("123456789"), 0 /* poolId */,
                      0 /* expiryTime */));
  {
    MockCounterVisitor helper;
    EXPECT_CALL(helper, call(_, _)).Times(AtLeast(0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_items"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_inserts"), 2));
    EXPECT_CALL(helper, call(strPiece("navy_bh_succ_inserts"), 2));
    EXPECT_CALL(helper, call(strPiece("navy_bh_lookups"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_succ_lookups"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_removes"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_succ_removes"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_evictions"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_logical_written"), 22));
    EXPECT_CALL(helper, call(strPiece("navy_bh_physical_written"), 128));
    EXPECT_CALL(helper, call(strPiece("navy_bh_io_errors"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_bf_false_positive_pct"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_checksum_errors"), 0));
    bh.getCounters({toCallback(helper)});
  }
}

TEST(BigHash, DeviceErrorStats) {
  BigHash::Config config;
  setLayout(config, 64, 1);
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 64);
  config.device = device.get();

  BigHash bh(std::move(config));

  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key1"), makeView("1"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_CALL(*device, writeImpl(0, 64, _, _)).WillOnce(Return(false));
  EXPECT_EQ(Status::DeviceError,
            bh.insert(makeHK("key2"), makeView("1"), 0 /* poolId */,
                      0 /* expiryTime */));
  {
    MockCounterVisitor helper;
    EXPECT_CALL(helper, call(_, _)).Times(AtLeast(0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_items"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_inserts"), 2));
    EXPECT_CALL(helper, call(strPiece("navy_bh_succ_inserts"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_lookups"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_succ_lookups"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_removes"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_succ_removes"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_evictions"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_logical_written"), 5));
    EXPECT_CALL(helper, call(strPiece("navy_bh_physical_written"), 64));
    EXPECT_CALL(helper, call(strPiece("navy_bh_io_errors"), 1));
    EXPECT_CALL(helper, call(strPiece("navy_bh_bf_false_positive_pct"), 0));
    EXPECT_CALL(helper, call(strPiece("navy_bh_checksum_errors"), 0));
    bh.getCounters({toCallback(helper)});
  }
}

TEST(BigHash, DoubleInsert) {
  BigHash::Config config;
  setLayout(config, 128, 2);
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 128);
  config.device = device.get();

  MockDestructor helper;
  config.destructorCb = toCallback(helper);

  BigHash bh(std::move(config));

  Buffer value;
  uint32_t lat = 0;

  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok, bh.lookup(makeHK("key"), value, lat));
  EXPECT_EQ(makeView("12345"), value.view());

  EXPECT_CALL(helper,
              call(makeHK("key"), makeView("12345"), DestructorEvent::Removed));

  // Insert the same key a second time will overwrite the previous value.
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key"), makeView("45678"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok, bh.lookup(makeHK("key"), value, lat));
  EXPECT_EQ(makeView("45678"), value.view());
}

TEST(BigHash, DestructorCallback) {
  BigHash::Config config;
  setLayout(config, 64, 1);
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 64);
  config.device = device.get();

  MockDestructor helper;
  EXPECT_CALL(
      helper,
      call(makeHK("key 1"), makeView("value 1"), DestructorEvent::Recycled));
  EXPECT_CALL(
      helper,
      call(makeHK("key 2"), makeView("value 2"), DestructorEvent::Removed));
  config.destructorCb = toCallback(helper);

  BigHash bh(std::move(config));
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key 1"), makeView("value 1"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key 2"), makeView("value 2"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok, bh.remove(makeHK("key 2")));
}

TEST(BigHash, Reset) {
  BigHash::Config config;
  size_t bucketCount = 2;
  constexpr uint32_t alignSize = 128;
  setLayout(config, alignSize, bucketCount);

  auto bf = std::make_unique<BloomFilter>(bucketCount, 2, 4);
  config.bloomFilter = std::move(bf);

  auto device =
      std::make_unique<NiceMock<MockDevice>>(config.cacheSize, alignSize);
  auto readFirstBucket = [&dev = device->getRealDeviceRef()] {
    // Read only the bucket after the initial generation and checksum fields
    // since the former can change with reset() while the latter can change
    // with every write.
    Buffer buf{alignSize, alignSize};
    dev.read(0, alignSize, buf.data());
    buf.trimStart(8);
    buf.shrink(92);
    return buf;
  };
  config.device = device.get();

  BigHash bh(std::move(config));

  Buffer value;
  uint32_t lat = 0;

  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok, bh.lookup(makeHK("key"), value, lat));
  EXPECT_EQ(makeView("12345"), value.view());
  auto oldBucketContent = readFirstBucket();

  bh.reset();
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("key"), value, lat));

  // bh reset should reset the bloom filter as well.
  EXPECT_EQ(1, bh.bfRejectCount());

  // The new bucket content must be identical to that of the old since
  // after a reset, our first write is equivalent to writing to a brand
  // new bucket.
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  auto newBucketContent = readFirstBucket();
  EXPECT_EQ(oldBucketContent.view(), newBucketContent.view());
}

TEST(BigHash, WriteInTwoBuckets) {
  // Following will generate a list of numbers and their hashes, I pick
  // two that collide and another one that doesn't for this test.
  //
  // for (uint8_t i = 'A'; i < 'z'; ++i) {
  //   Buffer key{1};
  //   *key.data() = i;
  //   std::bitset<sizeof(uint64_t)> out{hashBuffer(key.view())};
  //   std::cerr << "key: " << i << ", hash: " << out << std::endl;
  // }
  //
  // key: A, hash: 10111010
  // key: B, hash: 10110001
  // key: C, hash: 11100010
  // key: D, hash: 01101100

  BigHash::Config config;
  config.cacheBaseOffset = 256;
  setLayout(config, 128, 2);
  auto device = std::make_unique<StrictMock<MockDevice>>(
      config.cacheBaseOffset + config.cacheSize, 128);
  {
    InSequence inSeq;
    EXPECT_CALL(*device, allocatePlacementHandle());
    EXPECT_CALL(*device, readImpl(256, 128, _));
    EXPECT_CALL(*device, writeImpl(256, 128, _, _));
    EXPECT_CALL(*device, readImpl(384, 128, _));
    EXPECT_CALL(*device, writeImpl(384, 128, _, _));
    EXPECT_CALL(*device, readImpl(256, 128, _));
    EXPECT_CALL(*device, writeImpl(256, 128, _, _));
  }
  config.device = device.get();

  BigHash bh(std::move(config));

  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("A"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("B"), makeView("45678"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("C"), makeView("67890"), 0 /* poolId */,
                      0 /* expiryTime */));
}

TEST(BigHash, RemoveNotFound) {
  // Remove that is not found will not write to disk, but rebuild bloom filter
  // and second remove would not read the device.
  BigHash::Config config;
  setLayout(config, 128, 1);
  auto device = std::make_unique<StrictMock<MockDevice>>(config.cacheSize, 128);
  {
    InSequence inSeq;
    EXPECT_CALL(*device, allocatePlacementHandle());
    EXPECT_CALL(*device, readImpl(0, 128, _));
    EXPECT_CALL(*device, writeImpl(0, 128, _, _));
    EXPECT_CALL(*device, readImpl(0, 128, _));
    EXPECT_CALL(*device, writeImpl(0, 128, _, _));
    EXPECT_CALL(*device, readImpl(0, 128, _));
  }
  config.device = device.get();

  BigHash bh(std::move(config));

  bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
            0 /* expiryTime */);
  bh.remove(makeHK("key"));
  bh.remove(makeHK("key"));
}

TEST(BigHash, CorruptBucket) {
  // Write a bucket, then corrupt a byte so we won't be able to read it
  BigHash::Config config;
  constexpr uint32_t alignSize = 128;
  setLayout(config, alignSize, 2);
  auto device =
      std::make_unique<NiceMock<MockDevice>>(config.cacheSize, alignSize);
  config.device = device.get();

  BigHash bh(std::move(config));

  bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
            0 /* expiryTime */);

  Buffer value;
  uint32_t lat = 0;
  EXPECT_EQ(Status::Ok, bh.lookup(makeHK("key"), value, lat));
  EXPECT_EQ(makeView("12345"), value.view());

  // Corrupt data. Use device directly to avoid alignment checks
  uint8_t badBytes[4] = {13, 17, 19, 23};

  Buffer badBuf{alignSize, alignSize};
  memcpy(badBuf.data(), badBytes, sizeof(badBytes));
  device->getRealDeviceRef().write(0, std::move(badBuf));

  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("key"), value, lat));
}

TEST(BigHash, Recovery) {
  BigHash::Config config;
  config.cacheSize = 16 * 1024;
  auto device = createMemoryDevice(config.cacheSize, nullptr /* encryption */);
  config.device = device.get();

  BigHash bh(std::move(config));

  Buffer value;
  uint32_t lat = 0;
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok, bh.lookup(makeHK("key"), value, lat));
  EXPECT_EQ(makeView("12345"), value.view());

  folly::IOBufQueue queue;
  auto rw = createMemoryRecordWriter(queue);
  bh.persist(*rw);

  auto rr = createMemoryRecordReader(queue);
  ASSERT_TRUE(bh.recover(*rr));

  EXPECT_EQ(Status::Ok, bh.lookup(makeHK("key"), value, lat));
  EXPECT_EQ(makeView("12345"), value.view());
}

TEST(BigHash, RecoveryBadConfig) {
  folly::IOBufQueue queue;
  {
    BigHash::Config config;
    config.cacheSize = 16 * 1024;
    config.bucketSize = 4 * 1024;
    auto device =
        createMemoryDevice(config.cacheSize, nullptr /* encryption */);
    config.device = device.get();

    BigHash bh(std::move(config));

    Buffer value;
    uint32_t lat = 0;
    EXPECT_EQ(Status::Ok,
              bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
                        0 /* expiryTime */));
    EXPECT_EQ(Status::Ok, bh.lookup(makeHK("key"), value, lat));
    EXPECT_EQ(makeView("12345"), value.view());

    auto rw = createMemoryRecordWriter(queue);
    bh.persist(*rw);
  }
  {
    BigHash::Config config;
    // Config is different. Number of buckets the same, but size is different.
    config.cacheSize = 32 * 1024;
    config.bucketSize = 8 * 1024;
    auto device =
        createMemoryDevice(config.cacheSize, nullptr /* encryption */);
    config.device = device.get();

    BigHash bh(std::move(config));
    auto rr = createMemoryRecordReader(queue);
    ASSERT_FALSE(bh.recover(*rr));
  }
}

TEST(BigHash, RecoveryCorruptedData) {
  BigHash::Config config;
  config.cacheSize = 1024 * 1024;
  auto device = createMemoryDevice(config.cacheSize, nullptr /* encryption */);
  config.device = device.get();

  BigHash bh(std::move(config));

  Buffer value;
  uint32_t lat = 0;

  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key"), makeView("12345"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok, bh.lookup(makeHK("key"), value, lat));
  EXPECT_EQ(makeView("12345"), value.view());

  auto ioBuf = folly::IOBuf::createCombined(512);
  std::generate(ioBuf->writableData(), ioBuf->writableTail(),
                std::minstd_rand());

  folly::IOBufQueue queue;
  auto rw = createMemoryRecordWriter(queue);
  rw->writeRecord(std::move(ioBuf));

  auto rr = createMemoryRecordReader(queue);
  ASSERT_FALSE(bh.recover(*rr));
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("key"), value, lat));
}

TEST(BigHash, ConcurrentRead) {
  // In one bucket, we can have multiple readers at the same time.
  BigHash::Config config;
  setLayout(config, 128, 1);
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 128);
  config.device = device.get();

  BigHash bh(std::move(config));
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key 1"), makeView("1"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key 2"), makeView("2"), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key 3"), makeView("3"), 0 /* poolId */,
                      0 /* expiryTime */));

  struct MockLookupHelper {
    MOCK_METHOD2(call, void(HashedKey, BufferView));
  };
  MockLookupHelper helper;
  EXPECT_CALL(helper, call(makeHK("key 1"), makeView("1")));
  EXPECT_CALL(helper, call(makeHK("key 2"), makeView("2")));
  EXPECT_CALL(helper, call(makeHK("key 3"), makeView("3")));

  auto runRead = [&bh, &helper](HashedKey hk) {
    Buffer value;
    uint32_t lat = 0;
    EXPECT_EQ(Status::Ok, bh.lookup(hk, value, lat));
    helper.call(hk, value.view());
  };

  auto t1 = std::thread(runRead, makeHK("key 1"));
  auto t2 = std::thread(runRead, makeHK("key 2"));
  auto t3 = std::thread(runRead, makeHK("key 3"));
  t1.join();
  t2.join();
  t3.join();
}

TEST(BigHash, BloomFilterRecoveryFail) {
  BigHash::Config config;
  setLayout(config, 128, 2);
  auto device = std::make_unique<StrictMock<MockDevice>>(config.cacheSize, 128);
  EXPECT_CALL(*device, allocatePlacementHandle());
  EXPECT_CALL(*device, readImpl(_, _, _)).Times(0);
  config.device = device.get();
  config.bloomFilter = std::make_unique<BloomFilter>(2, 1, 4);

  BigHash bh(std::move(config));

  Buffer value;
  uint32_t lat = 0;
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("100"), value, lat));

  // After a bad recovery, filters will not be affected
  auto ioBuf = folly::IOBuf::createCombined(512);
  std::generate(ioBuf->writableData(), ioBuf->writableTail(),
                std::minstd_rand());

  folly::IOBufQueue queue;
  auto rw = createMemoryRecordWriter(queue);
  rw->writeRecord(std::move(ioBuf));
  auto rr = createMemoryRecordReader(queue);
  ASSERT_FALSE(bh.recover(*rr));

  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("100"), value, lat));
}

TEST(BigHash, BloomFilter) {
  BigHash::Config config;
  setLayout(config, 128, 2);
  auto device = createMemoryDevice(config.cacheSize, nullptr /* encryption */);
  config.device = device.get();
  config.bloomFilter = std::make_unique<BloomFilter>(2, 1, 4);

  MockDestructor helper;
  EXPECT_CALL(helper, call(makeHK("100"), _, DestructorEvent::Recycled));
  EXPECT_CALL(helper, call(makeHK("101"), _, DestructorEvent::Removed));
  config.destructorCb = toCallback(helper);

  BigHash bh(std::move(config));
  BufferGen bg;

  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("100"), bg.gen(20).view(), 0 /* poolId */,
                      0 /* expiryTime */));

  // Check that eviction triggers BF rebuild. Use the following setup:
  // - Insert "100". BF rejects "101" and accepts "102" and "103".
  // - Insert "101": bucket 0 contains "100" and "101". BF rejects "110".
  // - Insert "110", "100" evicted: bucket contains "101" and "110".
  //
  // Expect that BF rejects "102" and "103". If not rebuilt, it would accept.
  EXPECT_EQ(0, bh.bfRejectCount());

  Buffer value;
  uint32_t lat = 0;
  // Rejected by BF
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("101"), value, lat));
  EXPECT_EQ(1, bh.bfRejectCount());
  // False positive, rejected by key
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("102"), value, lat));
  EXPECT_EQ(1, bh.bfRejectCount());
  // False positive, rejected by key
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("103"), value, lat));
  EXPECT_EQ(1, bh.bfRejectCount());

  // Insert "101"
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("101"), bg.gen(20).view(), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("110"), value, lat));
  EXPECT_EQ(2, bh.bfRejectCount());

  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("110"), bg.gen(20).view(), 0 /* poolId */,
                      0 /* expiryTime */));
  EXPECT_EQ(Status::Ok, bh.lookup(makeHK("101"), value, lat));
  EXPECT_EQ(Status::Ok, bh.lookup(makeHK("110"), value, lat));
  EXPECT_EQ(2, bh.bfRejectCount());

  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("100"), value, lat));
  EXPECT_EQ(3, bh.bfRejectCount());
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("102"), value, lat));
  EXPECT_EQ(4, bh.bfRejectCount());
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("103"), value, lat));
  EXPECT_EQ(5, bh.bfRejectCount());

  // If we remove, BF will be rebuilt and will reject "101":
  EXPECT_EQ(Status::Ok, bh.remove(makeHK("101")));
  EXPECT_EQ(5, bh.bfRejectCount());
  EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("101"), value, lat));
  EXPECT_EQ(6, bh.bfRejectCount());
}

// Validated against the real metadata writer, since that is what the
// reservation is checked against.
TEST(BigHash, EstimatePersistSize) {
  constexpr uint64_t kMetadataSize = 16 * 1024 * 1024;
  constexpr uint64_t kBlockSize = 4096;

  BigHash::Config config;
  config.cacheSize = 8 * 1024 * 1024;
  config.bucketSize = 4096;
  auto device = createMemoryDevice(config.cacheSize, nullptr /* encryption */);
  config.device = device.get();
  config.bloomFilter =
      std::make_unique<BloomFilter>(config.numBuckets(), 4, 128);

  BigHash bh(std::move(config));

  // Read through getCounters, which publishes the value cached by reset().
  const uint64_t estimated =
      bh.getCounters(CounterVisitor{[](folly::StringPiece, double) {}});

  auto metaDevice = createMemoryDevice(kMetadataSize, nullptr /* encryption */);
  auto rw = createMetadataRecordWriter(*metaDevice, kMetadataSize);
  bh.persist(*rw);
  const uint64_t written = rw->getCurPos();

  EXPECT_GT(written, kBlockSize);
  // getCurPos() is block-granular; the sub-block tail is written by the
  // writer's destructor, so it under-reports by less than one block.
  EXPECT_GE(estimated, written);
  EXPECT_LT(estimated - written, kBlockSize);
}

// persist the bighash and ensure recovery has the bloom filter
TEST(BigHash, BloomFilterRecovery) {
  std::unique_ptr<Device> actual;
  folly::IOBufQueue queue;

  // Add a value to the first bucket and persist.
  {
    BigHash::Config config;
    setLayout(config, 128, 2);
    auto device =
        std::make_unique<StrictMock<MockDevice>>(config.cacheSize, 128);
    EXPECT_CALL(*device, allocatePlacementHandle());
    EXPECT_CALL(*device, readImpl(0, 128, _));
    EXPECT_CALL(*device, writeImpl(0, 128, _, _));
    config.device = device.get();
    config.bloomFilter = std::make_unique<BloomFilter>(2, 1, 4);

    BigHash bh(std::move(config));
    EXPECT_EQ(Status::Ok,
              bh.insert(makeHK("100"), makeView("cat"), 0 /* poolId */,
                        0 /* expiryTime */));
    Buffer value;
    uint32_t lat = 0;
    EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("200"), value, lat));
    EXPECT_EQ(1, bh.bfRejectCount());
    auto rw = createMemoryRecordWriter(queue);
    bh.persist(*rw);

    actual = device->releaseRealDevice();
  }

  // Recover. BF is also recovered.
  {
    BigHash::Config config;
    setLayout(config, 128, 2);
    auto device = std::make_unique<MockDevice>(0, 128);
    device->setRealDevice(std::move(actual));
    EXPECT_CALL(*device, readImpl(0, 128, _)).Times(0);
    EXPECT_CALL(*device, readImpl(128, 128, _)).Times(0);
    config.device = device.get();
    config.bloomFilter = std::make_unique<BloomFilter>(2, 1, 4);

    BigHash bh(std::move(config));
    auto rr = createMemoryRecordReader(queue);
    ASSERT_TRUE(bh.recover(*rr));

    EXPECT_EQ(Status::NotFound, bh.remove(makeHK("200")));
    EXPECT_EQ(1, bh.bfRejectCount());

    EXPECT_EQ(Status::NotFound, bh.remove(makeHK("200")));
    EXPECT_EQ(2, bh.bfRejectCount());
    Buffer value;
    uint32_t lat = 0;
    EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("200"), value, lat));
    EXPECT_EQ(3, bh.bfRejectCount());

    // Test lookup, second bucket
    EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("201"), value, lat));
    EXPECT_EQ(4, bh.bfRejectCount());
    EXPECT_EQ(Status::NotFound, bh.lookup(makeHK("201"), value, lat));
    EXPECT_EQ(5, bh.bfRejectCount());

    actual = device->releaseRealDevice();
  }
}

TEST(BigHash, DestructorCallbackOutsideLock) {
  BigHash::Config config;
  setLayout(config, 64, 1);
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 64);
  config.device = device.get();

  std::atomic<bool> done = false, started = false;
  config.destructorCb = [&](HashedKey, BufferView, DestructorEvent event) {
    started = true;
    // only hangs the insertion not removal
    while (!done && event == DestructorEvent::Recycled) {
      ;
    }
  };

  BigHash bh(std::move(config));
  EXPECT_EQ(Status::Ok,
            bh.insert(makeHK("key 1"), makeView("value 1"), 0 /* poolId */,
                      0 /* expiryTime */));

  // insert will hang in the destructor, but lock should be released once
  // destructorCB starts
  std::thread t([&]() {
    EXPECT_EQ(Status::Ok,
              bh.insert(makeHK("key 1"), makeView("value 2"), 0 /* poolId */,
                        0 /* expiryTime */));
  });

  // wait until destructor started, which means bucket lock is released
  while (!started) {
    ;
  }
  // remove should not be blocked since bucket lock has been released
  EXPECT_EQ(Status::Ok, bh.remove(makeHK("key 1")));

  done = true;
  t.join();
}

TEST(BigHash, RandomAlloc) {
  BigHash::Config config;
  setLayout(config, 1024, 4);
  auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 128);
  config.device = device.get();

  BigHash bh(std::move(config));

  BufferGen bg;
  auto data = bg.gen(40);
  for (uint32_t bid = 0; bid < 4; bid++) {
    for (size_t i = 0; i < 20; i++) {
      auto keyStr = genKey(4, bid);
      sprintf((char*)data.data(),
              "data_%8s PAYLOAD: ", &keyStr[keyStr.size() - 8]);
      EXPECT_EQ(Status::Ok,
                bh.insert(makeHK(keyStr.c_str()), data.view(), 0 /* poolId */,
                          0 /* expiryTime */));
    }
  }

  size_t succ_cnt = 0;
  std::unordered_map<std::string, size_t> getCnts;
  static constexpr size_t loopCnt = 10000;
  for (size_t i = 0; i < loopCnt; i++) {
    Buffer value;
    auto [status, keyStr] = bh.getRandomAlloc(value);
    if (status != navy::Status::Ok) {
      continue;
    }
    succ_cnt++;
    getCnts[keyStr]++;
  }

  std::vector<size_t> cnts;
  std::transform(
      getCnts.begin(), getCnts.end(), std::back_inserter(cnts),
      [](const std::pair<std::string, size_t>& p) { return p.second; });
  auto [avg, stddev] = util::getMeanDeviation(cnts);

  EXPECT_GT(succ_cnt, (size_t)(static_cast<double>(loopCnt) * 0.8));
  EXPECT_LT(stddev, avg * 0.2);
}

// Make sure estimate write size always returns the bucket size.
// Modify this test if we change the implementation.
TEST(BigHash, EstimateWriteSize) {
  {
    uint32_t bucketSize = 2048;
    BigHash::Config config;
    setLayout(config, bucketSize, 4);
    auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 128);
    config.device = device.get();

    BigHash bh(std::move(config));
    EXPECT_EQ(bh.estimateWriteSize(makeHK("key"), makeView("12345")),
              bucketSize);
    EXPECT_EQ(bh.estimateWriteSize(makeHK("key2"), makeView("1")), bucketSize);
  }
  {
    uint32_t bucketSize = 8192;
    BigHash::Config config;
    setLayout(config, bucketSize, 4);
    auto device = std::make_unique<NiceMock<MockDevice>>(config.cacheSize, 128);
    config.device = device.get();

    BigHash bh(std::move(config));
    EXPECT_EQ(bh.estimateWriteSize(makeHK("key3"), makeView("12345")),
              bucketSize);
    EXPECT_EQ(bh.estimateWriteSize(makeHK("key4"), makeView("1")), bucketSize);
  }
}
} // namespace facebook::cachelib::navy::tests
