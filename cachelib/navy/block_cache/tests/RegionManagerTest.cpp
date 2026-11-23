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

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "cachelib/common/inject_pause.h"
#include "cachelib/navy/block_cache/LruPolicy.h"
#include "cachelib/navy/block_cache/RegionManager.h"
#include "cachelib/navy/block_cache/tests/TestHelpers.h"
#include "cachelib/navy/testing/BufferGen.h"
#include "cachelib/navy/testing/MockDevice.h"
#include "cachelib/navy/testing/SeqPoints.h"

namespace facebook::cachelib::navy::tests {
class RegionManagerTestPeer {
 public:
  // Exercise the copy helper even when the runtime probe cannot find DSA.
  static void enableCopyOffload(RegionManager& rm) {
    rm.flushCopyOffload_ = true;
  }
};

namespace {
const Region kRegion0{RegionId{0}, 100};
const Region kRegion1{RegionId{1}, 100};
const Region kRegion2{RegionId{2}, 100};
const Region kRegion3{RegionId{3}, 100};
constexpr uint16_t kFlushRetryLimit = 10;

double getRegionManagerCounter(const RegionManager& rm,
                               folly::StringPiece counterName) {
  double result = -1;
  rm.getCounters({[&](folly::StringPiece name, double value) {
    if (name == counterName) {
      result = value;
    }
  }});
  return result;
}
} // namespace

TEST(RegionManager, ReclaimLruAsFifo) {
  auto policy = std::make_unique<LruPolicy>(4);
  auto& ep = *policy;
  ep.track(kRegion0);
  ep.track(kRegion1);
  ep.track(kRegion2);
  ep.track(kRegion3);

  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  auto device =
      createMemoryDevice(kNumRegions * kRegionSize, nullptr /* encryption */);
  RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
  RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
  auto rm = std::make_unique<RegionManager>(
      kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
      std::move(cleanupCb), std::move(policy),
      kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
      true /* workerFlushAsync */);

  // without touch, the first region inserted is reclaimed
  EXPECT_EQ(kRegion0.id(), rm->evict());
  EXPECT_EQ(kRegion1.id(), rm->evict());
  EXPECT_EQ(kRegion2.id(), rm->evict());
  EXPECT_EQ(kRegion3.id(), rm->evict());
}

TEST(RegionManager, ReclaimLru) {
  auto policy = std::make_unique<LruPolicy>(4);
  auto& ep = *policy;
  ep.track(kRegion0);
  ep.track(kRegion1);
  ep.track(kRegion2);
  ep.track(kRegion3);

  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  auto device =
      createMemoryDevice(kNumRegions * kRegionSize, nullptr /* encryption */);
  RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
  RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
  auto rm = std::make_unique<RegionManager>(
      kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
      std::move(cleanupCb), std::move(policy),
      kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
      true /* workerFlushAsync */);

  rm->touch(kRegion0.id());
  rm->touch(kRegion1.id());

  EXPECT_EQ(kRegion2.id(), rm->evict());
  EXPECT_EQ(kRegion3.id(), rm->evict());
  EXPECT_EQ(kRegion0.id(), rm->evict());
  EXPECT_EQ(kRegion1.id(), rm->evict());
}

TEST(RegionManager, Recovery) {
  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  auto device =
      createMemoryDevice(kNumRegions * kRegionSize, nullptr /* encryption */);

  folly::IOBufQueue ioq;
  {
    std::vector<uint32_t> hits(4);
    auto policy = std::make_unique<MockPolicy>(&hits);
    expectRegionsTracked(*policy, {0, 1, 2, 3});
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */);

    // Empty region, like it was evicted and reclaimed
    rm->getRegion(RegionId{1})
        .attachBuffer(std::make_unique<Buffer>(kRegionSize));
    rm->getRegion(RegionId{2})
        .attachBuffer(std::make_unique<Buffer>(kRegionSize));
    for (int i = 0; i < 20; i++) {
      auto [desc, _, _2] = rm->getRegion(RegionId{1}).openAndAllocate(101);
      rm->getRegion(RegionId{1}).close(std::move(desc));
    }
    for (int i = 0; i < 30; i++) {
      auto [desc, _, _2] = rm->getRegion(RegionId{2}).openAndAllocate(101);
      rm->getRegion(RegionId{2}).close(std::move(desc));
    }

    auto rw = createMemoryRecordWriter(ioq);
    rm->persist(*rw);
  }

  {
    std::vector<uint32_t> hits(4);
    auto policy = std::make_unique<MockPolicy>(&hits);
    // Region 0 - 3 will be tracked at least once since the first time
    // is when RegionManager is initialized. When the RM is recovered,
    {
      testing::InSequence s;
      EXPECT_CALL(*policy, reset());
      // First all regions are tracked when region manager is created
      expectRegionsTracked(*policy, {0, 1, 2, 3});
      EXPECT_CALL(*policy, reset());
      // Non-empty regions are tracked at last.
      expectRegionsTracked(*policy, {0, 3, 1, 2});
    }

    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */);

    auto rr = createMemoryRecordReader(ioq);
    rm->recover(*rr);

    EXPECT_EQ(0, rm->getRegion(RegionId{0}).getLastEntryEndOffset());
    EXPECT_EQ(0, rm->getRegion(RegionId{0}).getNumItems());

    EXPECT_EQ(2020, rm->getRegion(RegionId{1}).getLastEntryEndOffset());
    EXPECT_EQ(20, rm->getRegion(RegionId{1}).getNumItems());

    EXPECT_EQ(3030, rm->getRegion(RegionId{2}).getLastEntryEndOffset());
    EXPECT_EQ(30, rm->getRegion(RegionId{2}).getNumItems());

    // this is a region that was not assigned to anything.
    EXPECT_EQ(0, rm->getRegion(RegionId{3}).getLastEntryEndOffset());
    EXPECT_EQ(0, rm->getRegion(RegionId{3}).getNumItems());
  }
}

namespace {
void testReadWrite(bool directFlush,
                   bool flushCopyOffload,
                   bool forceCopyOffloadForTest = false) {
  constexpr uint64_t kBaseOffset = 1024;
  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;

  auto device = createMemoryDevice(kBaseOffset + kNumRegions * kRegionSize,
                                   nullptr /* encryption */);
  auto devicePtr = device.get();
  RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
  RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
  auto rm = std::make_unique<RegionManager>(
      kNumRegions, kRegionSize, kBaseOffset, *device, 1, 1, 0,
      std::move(evictCb), std::move(cleanupCb), std::make_unique<LruPolicy>(4),
      kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
      true /* workerFlushAsync */, false /* allowReadDuringReclaim */,
      false /* recoverEvictionPolicy */, directFlush, flushCopyOffload);
  if (forceCopyOffloadForTest) {
    RegionManagerTestPeer::enableCopyOffload(*rm);
  }

  ENABLE_INJECT_PAUSE_IN_SCOPE();

  injectPauseSet("pause_reclaim_done");

  constexpr uint32_t kLocalOffset = 3 * 1024;
  constexpr uint32_t kSize = 1024;
  BufferGen bg;
  RegionId rid;
  // do reclaim couple of times to get RegionId of 1
  rm->startReclaim();
  EXPECT_TRUE(injectPauseWait("pause_reclaim_done"));
  ASSERT_EQ(OpenStatus::Ready, rm->getCleanRegion(rid, false).first);
  ASSERT_EQ(0, rid.index());

  // getCleanRegion should have triggered next reclaim
  EXPECT_TRUE(injectPauseWait("pause_reclaim_done"));
  ASSERT_EQ(OpenStatus::Ready, rm->getCleanRegion(rid, false).first);
  ASSERT_EQ(1, rid.index());
  EXPECT_TRUE(injectPauseWait("pause_reclaim_done"));

  auto& region = rm->getRegion(rid);
  auto [wDesc, _, allocView] = region.openAndAllocate(4 * kSize);
  EXPECT_EQ(OpenStatus::Ready, wDesc.status());
  auto buf = bg.gen(kSize);
  auto wAddr = RelAddress{rid, kLocalOffset};
  buf.view().copyTo(allocView.data() + kLocalOffset);
  auto rDesc = rm->openForRead(rid, rm->getSeqNumber());
  ASSERT_EQ(OpenStatus::Ready, rDesc.status());
  auto bufRead = rm->read(rDesc, wAddr, kSize);
  EXPECT_TRUE(bufRead.size() == kSize);
  EXPECT_EQ(buf.view(), bufRead.view());
  region.close(std::move(rDesc));

  // flush buffer
  region.close(std::move(wDesc));
  EXPECT_EQ(Region::FlushRes::kSuccess, rm->flushBuffer(rid));
  // Check device directly at the offset we expect data to be written
  auto expectedOfs = kBaseOffset + kRegionSize + kLocalOffset;
  Buffer bufReadDirect{kSize};
  EXPECT_TRUE(devicePtr->read(expectedOfs, kSize, bufReadDirect.data()));
  EXPECT_EQ(buf.view(), bufReadDirect.view());
}
} // namespace

TEST(RegionManager, ReadWrite) {
  testReadWrite(false /* directFlush */, false /* flushCopyOffload */);
}

TEST(RegionManager, ReadWriteCopyOffload) {
  for (bool directFlush : {false, true}) {
    SCOPED_TRACE(directFlush);
    testReadWrite(directFlush, true /* flushCopyOffload */);
  }
}

TEST(RegionManager, ReadWriteWithActiveCopyOffload) {
  testReadWrite(false /* directFlush */, true /* flushCopyOffload */,
                true /* forceCopyOffloadForTest */);
}

TEST(RegionManager, CopyOffloadWriteFailureAndRetry) {
  constexpr uint32_t kNumRegions = 2;
  constexpr uint32_t kRegionSize = 4 * 1024;
  testing::NiceMock<MockDevice> device(kNumRegions * kRegionSize, 1024);
  RegionManager rm(
      kNumRegions, kRegionSize, 0, device, 1, 1, 0,
      [](RegionId, BufferView) { return 0; }, [](RegionId, BufferView) {},
      std::make_unique<LruPolicy>(kNumRegions), 1 /* numInMemBuffers */, 0,
      kFlushRetryLimit, true /* workerFlushAsync */,
      false /* allowReadDuringReclaim */, false /* recoverEvictionPolicy */,
      false /* directFlush */, true /* flushCopyOffload */);
  RegionManagerTestPeer::enableCopyOffload(rm);

  BufferGen bg;
  std::vector<Buffer> expected;
  expected.push_back(bg.gen(kRegionSize));
  expected.push_back(bg.gen(kRegionSize));
  uint32_t writes = 0;
  EXPECT_CALL(device, writeImpl(testing::_, testing::_, testing::_, testing::_))
      .Times(3)
      .WillRepeatedly(
          [&](uint64_t offset, uint32_t size, const void* data, int) {
            EXPECT_EQ(kRegionSize, size);
            const BufferView view{size, static_cast<const uint8_t*>(data)};
            EXPECT_EQ(expected.at(offset / kRegionSize).view(), view);
            if (++writes == 1) {
              return false;
            }
            return device.getRealDeviceRef().write(offset, view);
          });

  for (uint32_t i = 0; i < kNumRegions; ++i) {
    SCOPED_TRACE(i);
    const RegionId rid{i};
    auto [buffer, waiter] = rm.claimBufferFromPool(false /* addWaiter */);
    ASSERT_NE(nullptr, buffer);
    auto& region = rm.getRegion(rid);
    region.attachBuffer(std::move(buffer));
    auto [desc, addr, view] = region.openAndAllocate(kRegionSize);
    ASSERT_EQ(OpenStatus::Ready, desc.status());
    expected[i].view().copyTo(view.data());
    region.close(std::move(desc));

    rm.doFlush(rid, false /* async */);
    EXPECT_FALSE(region.hasBuffer());
    auto stored = device.getRealDeviceRef().read(i * kRegionSize, kRegionSize);
    EXPECT_EQ(expected[i].view(), stored.view());
  }

  ASSERT_EQ(3, writes);
  EXPECT_EQ(1, getRegionManagerCounter(rm, "navy_bc_inmem_flush_retries"));
}

TEST(RegionManager, RecoveryLRUOrder) {
  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  auto device =
      createMemoryDevice(kNumRegions * kRegionSize, nullptr /* encryption */);

  folly::IOBufQueue ioq;
  {
    auto policy = std::make_unique<LruPolicy>(kNumRegions);
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */);

    // Mark 1 and 2 clean (num entries == 0), 0 and 3 used. After recovery, LRU
    // should return clean before used, in order of index.
    rm->getRegion(RegionId{0})
        .attachBuffer(std::make_unique<Buffer>(kRegionSize));
    rm->getRegion(RegionId{3})
        .attachBuffer(std::make_unique<Buffer>(kRegionSize));
    for (int i = 0; i < 10; i++) {
      auto [desc, _, _2] = rm->getRegion(RegionId{0}).openAndAllocate(200);
      rm->getRegion(RegionId{0}).close(std::move(desc));
    }
    for (int i = 0; i < 20; i++) {
      auto [desc, _, _2] = rm->getRegion(RegionId{3}).openAndAllocate(150);
      rm->getRegion(RegionId{3}).close(std::move(desc));
    }

    auto rw = createMemoryRecordWriter(ioq);
    rm->persist(*rw);
  }

  {
    auto policy = std::make_unique<LruPolicy>(kNumRegions);
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */);

    auto rr = createMemoryRecordReader(ioq);
    rm->recover(*rr);

    EXPECT_EQ(RegionId{1}, rm->evict());
    EXPECT_EQ(RegionId{2}, rm->evict());
    EXPECT_EQ(RegionId{0}, rm->evict());
    EXPECT_EQ(RegionId{3}, rm->evict());
    EXPECT_EQ(RegionId{}, rm->evict()); // Invalid
  }
}

TEST(RegionManager, Fragmentation) {
  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  auto device =
      createMemoryDevice(kNumRegions * kRegionSize, nullptr /* encryption */);

  folly::IOBufQueue ioq;
  uint32_t fragmentationSize = 2 * kRegionSize;
  {
    auto policy = std::make_unique<LruPolicy>(kNumRegions);
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */);

    // Mark 1 and 2 clean (num entries == 0), 0 and 3 used. After recovery, LRU
    // should return clean before used, in order of index.
    rm->getRegion(RegionId{0})
        .attachBuffer(std::make_unique<Buffer>(kRegionSize));
    rm->getRegion(RegionId{3})
        .attachBuffer(std::make_unique<Buffer>(kRegionSize));
    for (int i = 0; i < 10; i++) {
      auto [desc, _, _2] = rm->getRegion(RegionId{0}).openAndAllocate(200);
      rm->getRegion(RegionId{0}).close(std::move(desc));
      fragmentationSize -= 200;
    }
    for (int i = 0; i < 20; i++) {
      auto [desc, _, _2] = rm->getRegion(RegionId{3}).openAndAllocate(150);
      rm->getRegion(RegionId{3}).close(std::move(desc));
      fragmentationSize -= 150;
    }

    // Even though we allocated, but we haven't tracked any. So fragmentation
    // stats are still zero.
    rm->getCounters({[](folly::StringPiece name, double count) {
      if (name == "navy_bc_external_fragmentation") {
        EXPECT_EQ(0, count);
      }
    }});

    auto rw = createMemoryRecordWriter(ioq);
    rm->persist(*rw);
  }

  {
    auto policy = std::make_unique<LruPolicy>(kNumRegions);
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */);

    rm->getCounters({[](folly::StringPiece name, double count) {
      if (name == "navy_bc_external_fragmentation") {
        EXPECT_EQ(0, count);
      }
    }});
    auto rr = createMemoryRecordReader(ioq);
    rm->recover(*rr);
    rm->getCounters(
        {[fragmentationSize](folly::StringPiece name, double count) {
          if (name == "navy_bc_external_fragmentation") {
            EXPECT_EQ(fragmentationSize, count);
          }
        }});

    EXPECT_EQ(RegionId{1}, rm->evict());
    EXPECT_EQ(RegionId{2}, rm->evict());
    EXPECT_EQ(RegionId{0}, rm->evict());
    EXPECT_EQ(RegionId{3}, rm->evict());
    EXPECT_EQ(RegionId{}, rm->evict()); // Invalid
  }
}

using testing::_;
using testing::Return;
TEST(RegionManager, cleanupRegionFailureSync) {
  // This test case tests if the flush is blocked on cleaning up and
  // detaching buffer while there are outstanding readers when
  // async flush is failed with an error due to the injected write failure
  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  constexpr uint16_t kNumInMemBuffer = 2;
  auto device = std::make_unique<MockDevice>(kNumRegions * kRegionSize, 1024);
  auto policy = std::make_unique<LruPolicy>(kNumRegions);
  RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
  RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
  auto rm = std::make_unique<RegionManager>(
      kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
      std::move(cleanupCb), std::move(policy), kNumInMemBuffer, 0,
      kFlushRetryLimit, true /* workerFlushAsync */);

  ENABLE_INJECT_PAUSE_IN_SCOPE();

  injectPauseSet("pause_reclaim_done");
  injectPauseSet("pause_flush_failure");

  BufferGen bg;
  RegionId rid;
  // Reclaim to get Region 0
  rm->startReclaim();
  EXPECT_TRUE(injectPauseWait("pause_reclaim_done"));

  ASSERT_EQ(OpenStatus::Ready, rm->getCleanRegion(rid, false).first);
  ASSERT_EQ(0, rid.index());
  // getCleanRegion should have triggered next reclaim
  EXPECT_TRUE(injectPauseWait("pause_reclaim_done"));

  // Write to Region 0
  auto& region = rm->getRegion(rid);
  auto [wDesc, addr, allocView] = region.openAndAllocate(kRegionSize);
  EXPECT_EQ(OpenStatus::Ready, wDesc.status());
  auto buf = bg.gen(1024);
  buf.view().copyTo(allocView.data());
  region.close(std::move(wDesc));

  SeqPoints sp;
  std::thread readThread{[&sp, &region] {
    auto rDesc = region.openForRead();
    EXPECT_EQ(OpenStatus::Ready, rDesc.status());
    sp.reached(0); // unblock flush

    sp.wait(1); // block here
    region.close(std::move(rDesc));
  }};

  std::thread flushThread{[&sp, &device, &rm, &rid] {
    // Make sure flush will fail
    EXPECT_CALL(*device, writeImpl(_, _, _, _)).WillRepeatedly(Return(false));
    sp.wait(0); // Flush after active reader
    rm->doFlush(rid, false /* async */);
  }};

  std::thread countThread{[&sp, &rm] {
    bool retried = false;
    // Wait for a cleanup retry up to 10s
    for (int i = 0; i < 100; i++) {
      rm->getCounters({[&retried](folly::StringPiece name, double count,
                                  CounterVisitor::CounterType type) {
        if (name == "navy_bc_inmem_flush_retries" &&
            type == CounterVisitor::CounterType::RATE) {
          if (count >= kFlushRetryLimit) {
            retried = true;
          }
        }
      }});
      if (retried) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    // Verify there is a cleanup retry
    EXPECT_TRUE(retried);

    // Flush cannot be completed until the region is open for read
    EXPECT_FALSE(injectPauseWait("pause_flush_failure", 1, true, 1000));

    // Unblock readThread to close the region
    sp.reached(1);
  }};

  // Flush can complete now
  EXPECT_TRUE(injectPauseWait("pause_flush_failure"));

  readThread.join();
  flushThread.join();
  countThread.join();

  // Verify other counters
  rm->getCounters({[](folly::StringPiece name, double count,
                      CounterVisitor::CounterType type) {
    if (name == "navy_bc_inmem_flush_retries" &&
        type == CounterVisitor::CounterType::RATE) {
      EXPECT_EQ(kFlushRetryLimit, count);
    }
    if (name == "navy_bc_inmem_flush_failures" &&
        type == CounterVisitor::CounterType::RATE) {
      EXPECT_EQ(1, count);
    }
    if (name == "navy_bc_inmem_waiting_flush") {
      EXPECT_EQ(0, count);
    }
  }});
}

TEST(RegionManager, cleanupRegionFailureAsync) {
  // This test case tests if the flush is blocked on cleaning up and
  // detaching buffer while there are outstanding readers when
  // sync flush is failed with an error due to the injected write failure
  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  constexpr uint16_t kNumInMemBuffer = 2;
  auto device = std::make_unique<MockDevice>(kNumRegions * kRegionSize, 1024);
  auto policy = std::make_unique<LruPolicy>(kNumRegions);
  RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
  RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
  auto rm = std::make_unique<RegionManager>(
      kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
      std::move(cleanupCb), std::move(policy), kNumInMemBuffer, 0,
      kFlushRetryLimit, true /* workerFlushAsync */);

  ENABLE_INJECT_PAUSE_IN_SCOPE();

  injectPauseSet("pause_reclaim_done");
  injectPauseSet("pause_flush_begin");
  injectPauseSet("pause_flush_failure");

  BufferGen bg;
  RegionId rid;
  // Reclaim to get Region 0
  rm->startReclaim();
  EXPECT_TRUE(injectPauseWait("pause_reclaim_done"));

  ASSERT_EQ(OpenStatus::Ready, rm->getCleanRegion(rid, false).first);
  ASSERT_EQ(0, rid.index());
  // getCleanRegion should have triggered next reclaim
  EXPECT_TRUE(injectPauseWait("pause_reclaim_done"));

  // Write to Region 0
  auto& region = rm->getRegion(rid);
  auto [wDesc, _addr, allocView] = region.openAndAllocate(kRegionSize);
  EXPECT_EQ(OpenStatus::Ready, wDesc.status());
  auto buf = bg.gen(1024);
  buf.view().copyTo(allocView.data());
  region.close(std::move(wDesc));

  SeqPoints sp;
  std::thread readThread{[&sp, &region] {
    auto rDesc = region.openForRead();
    EXPECT_EQ(OpenStatus::Ready, rDesc.status());
    sp.reached(0); // unblock flush

    sp.wait(1); // block here
    region.close(std::move(rDesc));
  }};

  std::thread flushThread{[&sp, &device, &rm, &rid] {
    // Make sure flush will fail
    EXPECT_CALL(*device, writeImpl(_, _, _, _)).WillRepeatedly(Return(false));
    sp.wait(0); // Flush after active reader
    rm->doFlush(rid, true /* async */);
  }};

  std::thread countThread{[&sp, &rm] {
    bool retried = false;
    EXPECT_TRUE(injectPauseWait("pause_flush_begin"));
    // Wait for a cleanup retry upto 10s
    for (int i = 0; i < 100; i++) {
      rm->getCounters({[&retried](folly::StringPiece name, double count,
                                  CounterVisitor::CounterType type) {
        if (name == "navy_bc_inmem_flush_retries" &&
            type == CounterVisitor::CounterType::RATE) {
          if (count >= kFlushRetryLimit) {
            retried = true;
          }
        }
      }});
      if (retried) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    // Verify there is a cleanup retry
    EXPECT_TRUE(retried);

    // Flush cannot be completed while the region is open for read
    EXPECT_FALSE(injectPauseWait("pause_flush_failure", 1, true, 1000));

    // Unblock readThread to close the region
    sp.reached(1);
  }};

  // Flush can complete now
  EXPECT_TRUE(injectPauseWait("pause_flush_failure"));

  readThread.join();
  flushThread.join();
  countThread.join();

  // Verify other counters
  rm->getCounters({[](folly::StringPiece name, double count,
                      CounterVisitor::CounterType type) {
    if (name == "navy_bc_inmem_flush_retries" &&
        type == CounterVisitor::CounterType::RATE) {
      EXPECT_EQ(kFlushRetryLimit, count);
    }
    if (name == "navy_bc_inmem_flush_failures" &&
        type == CounterVisitor::CounterType::RATE) {
      EXPECT_EQ(1, count);
    }
    if (name == "navy_bc_inmem_waiting_flush") {
      EXPECT_EQ(0, count);
    }
  }});
}

// Tests for eviction policy persist/recover (recoverEvictionPolicy flag)

TEST(RegionManager, EvictionPolicyRecoverPreservesFifoOrder) {
  // Verify that when recoverEvictionPolicy=true, the FIFO eviction order is
  // preserved across persist/recover rather than being rebuilt in region-ID
  // order.
  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  auto device =
      createMemoryDevice(kNumRegions * kRegionSize, nullptr /* encryption */);

  folly::IOBufQueue ioq;

  // Phase 1: Create regions in a specific order and persist
  {
    auto policy = std::make_unique<FifoPolicy>();
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */, false /* allowReadDuringReclaim */,
        true /* recoverEvictionPolicy */);

    // Populate regions 1 and 2 with items so they're non-empty
    rm->getRegion(RegionId{1})
        .attachBuffer(std::make_unique<Buffer>(kRegionSize));
    rm->getRegion(RegionId{2})
        .attachBuffer(std::make_unique<Buffer>(kRegionSize));
    for (int i = 0; i < 5; i++) {
      auto [desc, allocOk, view] =
          rm->getRegion(RegionId{1}).openAndAllocate(101);
      rm->getRegion(RegionId{1}).close(std::move(desc));
    }
    for (int i = 0; i < 3; i++) {
      auto [desc, allocOk, view] =
          rm->getRegion(RegionId{2}).openAndAllocate(101);
      rm->getRegion(RegionId{2}).close(std::move(desc));
    }

    // At constructor time all regions are empty, so FIFO queue is [0,1,2,3].
    // Evict region 0 so the persisted FIFO order is [1, 2, 3].
    auto evicted = rm->evict();
    EXPECT_EQ(RegionId{0}, evicted);

    auto rw = createMemoryRecordWriter(ioq);
    rm->persist(*rw);
  }

  // Phase 2: Recover with recoverEvictionPolicy=true
  {
    auto policy = std::make_unique<FifoPolicy>();
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */, false /* allowReadDuringReclaim */,
        true /* recoverEvictionPolicy */);

    auto rr = createMemoryRecordReader(ioq);
    rm->recover(*rr);

    // INVARIANT: This test relies on the persisted FIFO order [1, 2, 3]
    // being distinguishable from what resetEvictionPolicy() would produce.
    // With recoverEvictionPolicy=false, the queue would be [0, 3, 1, 2]
    // (empty regions 0,3 first, then non-empty 1,2 in ID order). If a
    // future change to resetEvictionPolicy() makes it produce [1, 2, 3]
    // by coincidence, this test will pass even when recovery is broken —
    // update the setup to maintain the distinguishability.
    EXPECT_EQ(RegionId{1}, rm->evict());
    EXPECT_EQ(RegionId{2}, rm->evict());
    EXPECT_EQ(RegionId{3}, rm->evict());
  }
}

TEST(RegionManager, EvictionPolicyRecoverDisabledFallsBack) {
  // Verify that when recoverEvictionPolicy=false (default), recovery falls
  // back to resetEvictionPolicy() even when policy data is present.
  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  auto device =
      createMemoryDevice(kNumRegions * kRegionSize, nullptr /* encryption */);

  folly::IOBufQueue ioq;

  // Phase 1: Persist with recoverEvictionPolicy=true (writes policy data)
  {
    auto policy = std::make_unique<FifoPolicy>();
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */, false /* allowReadDuringReclaim */,
        true /* recoverEvictionPolicy */);

    // Populate region 2 so it's non-empty
    rm->getRegion(RegionId{2})
        .attachBuffer(std::make_unique<Buffer>(kRegionSize));
    for (int i = 0; i < 5; i++) {
      auto [desc, allocOk, view] =
          rm->getRegion(RegionId{2}).openAndAllocate(101);
      rm->getRegion(RegionId{2}).close(std::move(desc));
    }

    auto rw = createMemoryRecordWriter(ioq);
    rm->persist(*rw);
  }

  // Phase 2: Recover with recoverEvictionPolicy=false (default)
  {
    auto policy = std::make_unique<FifoPolicy>();
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */, false /* allowReadDuringReclaim */,
        false /* recoverEvictionPolicy */);

    auto rr = createMemoryRecordReader(ioq);
    rm->recover(*rr);

    // With flag disabled, resetEvictionPolicy() runs: empty regions first
    // (0, 1, 3 in ID order), then non-empty (2).
    EXPECT_EQ(RegionId{0}, rm->evict());
    EXPECT_EQ(RegionId{1}, rm->evict());
    EXPECT_EQ(RegionId{3}, rm->evict());
    EXPECT_EQ(RegionId{2}, rm->evict());
  }
}

TEST(RegionManager, EvictionPolicyRecoverForwardCompat) {
  // Verify that new code reading old data (no policy data) gracefully falls
  // back to resetEvictionPolicy().
  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  auto device =
      createMemoryDevice(kNumRegions * kRegionSize, nullptr /* encryption */);

  folly::IOBufQueue ioq;

  // Phase 1: Persist with recoverEvictionPolicy=false (no policy data written)
  {
    auto policy = std::make_unique<FifoPolicy>();
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */);

    // Populate region 1
    rm->getRegion(RegionId{1})
        .attachBuffer(std::make_unique<Buffer>(kRegionSize));
    for (int i = 0; i < 5; i++) {
      auto [desc, allocOk, view] =
          rm->getRegion(RegionId{1}).openAndAllocate(101);
      rm->getRegion(RegionId{1}).close(std::move(desc));
    }

    auto rw = createMemoryRecordWriter(ioq);
    rm->persist(*rw);
  }

  // Phase 2: Recover with recoverEvictionPolicy=true but no policy data exists
  {
    auto policy = std::make_unique<FifoPolicy>();
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */, false /* allowReadDuringReclaim */,
        true /* recoverEvictionPolicy */);

    auto rr = createMemoryRecordReader(ioq);
    rm->recover(*rr);

    // No policy data present => falls back to resetEvictionPolicy()
    // Empty regions first (0, 2, 3), then non-empty (1)
    EXPECT_EQ(RegionId{0}, rm->evict());
    EXPECT_EQ(RegionId{2}, rm->evict());
    EXPECT_EQ(RegionId{3}, rm->evict());
    EXPECT_EQ(RegionId{1}, rm->evict());
  }
}

TEST(RegionManager, EvictionPolicyRecoverUnsupportedPolicyFallsBack) {
  // Verify that when the eviction policy doesn't support persistence
  // (LruPolicy throws), persist writes no policy data and recover falls back.
  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  auto device =
      createMemoryDevice(kNumRegions * kRegionSize, nullptr /* encryption */);

  folly::IOBufQueue ioq;

  // Phase 1: Persist with LruPolicy (which throws on persist)
  {
    auto policy = std::make_unique<LruPolicy>(kNumRegions);
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */, false /* allowReadDuringReclaim */,
        true /* recoverEvictionPolicy */);

    // Populate region 0
    rm->getRegion(RegionId{0})
        .attachBuffer(std::make_unique<Buffer>(kRegionSize));
    for (int i = 0; i < 5; i++) {
      auto [desc, allocOk, view] =
          rm->getRegion(RegionId{0}).openAndAllocate(101);
      rm->getRegion(RegionId{0}).close(std::move(desc));
    }

    // Should not throw — persist catches the exception from LruPolicy
    auto rw = createMemoryRecordWriter(ioq);
    rm->persist(*rw);
  }

  // Phase 2: Recover with LruPolicy — should fall back gracefully
  {
    auto policy = std::make_unique<LruPolicy>(kNumRegions);
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */, false /* allowReadDuringReclaim */,
        true /* recoverEvictionPolicy */);

    auto rr = createMemoryRecordReader(ioq);
    // Should not throw — falls back to resetEvictionPolicy()
    rm->recover(*rr);

    // LRU without touch behaves like FIFO: empty first (1, 2, 3), then
    // non-empty (0)
    EXPECT_EQ(RegionId{1}, rm->evict());
    EXPECT_EQ(RegionId{2}, rm->evict());
    EXPECT_EQ(RegionId{3}, rm->evict());
    EXPECT_EQ(RegionId{0}, rm->evict());
  }
}

TEST(RegionManager,
     EvictionPolicyRecoverUnsupportedPolicySegmentedFifoFallsBack) {
  // Same as the LruPolicy variant but using SegmentedFifoPolicy.
  // Both persist and recover throw "Not Implemented", so the code should
  // gracefully fall back to resetEvictionPolicy().
  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  auto device =
      createMemoryDevice(kNumRegions * kRegionSize, nullptr /* encryption */);

  folly::IOBufQueue ioq;

  // Phase 1: Persist with SegmentedFifoPolicy (throws on persist, caught)
  {
    auto policy =
        std::make_unique<SegmentedFifoPolicy>(std::vector<unsigned int>{1, 1});
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 2 /* numPriorities */,
        kFlushRetryLimit, true /* workerFlushAsync */,
        false /* allowReadDuringReclaim */, true /* recoverEvictionPolicy */);

    // Populate region 0 (mirror the LRU fallback test)
    rm->getRegion(RegionId{0})
        .attachBuffer(std::make_unique<Buffer>(kRegionSize));
    for (int i = 0; i < 5; i++) {
      auto [desc, allocOk, view] =
          rm->getRegion(RegionId{0}).openAndAllocate(101);
      rm->getRegion(RegionId{0}).close(std::move(desc));
    }

    // Should not throw — persist catches the exception from
    // SegmentedFifoPolicy
    auto rw = createMemoryRecordWriter(ioq);
    rm->persist(*rw);
  }

  // Phase 2: Recover with SegmentedFifoPolicy — should fall back gracefully
  {
    auto policy =
        std::make_unique<SegmentedFifoPolicy>(std::vector<unsigned int>{1, 1});
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device, 1, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 2 /* numPriorities */,
        kFlushRetryLimit, true /* workerFlushAsync */,
        false /* allowReadDuringReclaim */, true /* recoverEvictionPolicy */);

    auto rr = createMemoryRecordReader(ioq);
    // Should not throw — falls back to resetEvictionPolicy()
    EXPECT_NO_THROW(rm->recover(*rr));

    // SFIFO with all regions at default priority 0: empty regions tracked
    // first (1, 2, 3), then non-empty (0). All go into the lowest-priority
    // segment; with {1,1} ratio, no rebalancing moves them up. Same order
    // as the LruPolicy fallback test.
    EXPECT_EQ(RegionId{1}, rm->evict());
    EXPECT_EQ(RegionId{2}, rm->evict());
    EXPECT_EQ(RegionId{3}, rm->evict());
    EXPECT_EQ(RegionId{0}, rm->evict());
  }
}

TEST(RegionManager, EvictionPolicyRecoverRestoresCleanRegions) {
  // Verifies that regions in cleanRegions_ at persist time are restored to
  // the pool on recovery (not leaked into the void). Without restoration,
  // those regions would end up in NEITHER cleanRegions_ NOR the policy
  // queue, permanently leaking ~cleanRegionsPool worth of capacity per
  // restart.
  constexpr uint32_t kNumRegions = 4;
  constexpr uint32_t kRegionSize = 4 * 1024;
  constexpr uint32_t kCleanRegionsPool = 2;
  auto device =
      createMemoryDevice(kNumRegions * kRegionSize, nullptr /* encryption */);

  folly::IOBufQueue ioq;

  // Phase 1: drain 2 regions into cleanRegions_ via reclaim, then persist
  {
    auto policy = std::make_unique<FifoPolicy>();
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device,
        kCleanRegionsPool /* numCleanRegions */, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */, false /* allowReadDuringReclaim */,
        true /* recoverEvictionPolicy */);

    ENABLE_INJECT_PAUSE_IN_SCOPE();
    injectPauseSet("pause_reclaim_done");

    // FIFO at construction tracks all regions in ID order: [0, 1, 2, 3].
    // Reclaim two: cleanRegions_ becomes [0, 1] and policy becomes [2, 3].
    rm->startReclaim();
    EXPECT_TRUE(injectPauseWait("pause_reclaim_done"));
    rm->startReclaim();
    EXPECT_TRUE(injectPauseWait("pause_reclaim_done"));

    // Sanity: cleanRegions_ has 2 entries via the published counter.
    size_t cleanCount = 0;
    rm->getCounters({[&](folly::StringPiece name, double count,
                         CounterVisitor::CounterType) {
      if (name == "navy_bc_num_clean_regions") {
        cleanCount = static_cast<size_t>(count);
      }
    }});
    ASSERT_EQ(2u, cleanCount);

    auto rw = createMemoryRecordWriter(ioq);
    rm->persist(*rw);
  }

  // Phase 2: recover and verify that the previously-clean regions are
  // restored to cleanRegions_ rather than leaked.
  {
    auto policy = std::make_unique<FifoPolicy>();
    RegionEvictCallback evictCb{[](RegionId, BufferView) { return 0; }};
    RegionCleanupCallback cleanupCb{[](RegionId, BufferView) {}};
    auto rm = std::make_unique<RegionManager>(
        kNumRegions, kRegionSize, 0, *device,
        kCleanRegionsPool /* numCleanRegions */, 1, 0, std::move(evictCb),
        std::move(cleanupCb), std::move(policy),
        kNumRegions /* numInMemBuffers */, 0, kFlushRetryLimit,
        true /* workerFlushAsync */, false /* allowReadDuringReclaim */,
        true /* recoverEvictionPolicy */);

    auto rr = createMemoryRecordReader(ioq);
    rm->recover(*rr);

    // Without restoration, cleanRegions_ would be 0 (regions 0, 1 leaked).
    // With restoration, both should be back in cleanRegions_.
    size_t cleanCount = 0;
    rm->getCounters({[&](folly::StringPiece name, double count,
                         CounterVisitor::CounterType) {
      if (name == "navy_bc_num_clean_regions") {
        cleanCount = static_cast<size_t>(count);
      }
    }});
    EXPECT_EQ(2u, cleanCount);
  }
}
} // namespace facebook::cachelib::navy::tests
