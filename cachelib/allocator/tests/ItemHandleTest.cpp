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

#include <folly/fibers/Baton.h>
#include <folly/io/async/EventBase.h>
#include <gmock/gmock.h>

#include <algorithm>
#include <mutex>
#include <thread>

#include "cachelib/allocator/CacheItem.h"
#include "cachelib/allocator/tests/TestBase.h"
#include "cachelib/common/FastStats.h"

namespace facebook {
namespace cachelib {

namespace {
struct TestAllocator;

struct TestItem {
  using CacheT = TestAllocator;
  using Item = int;
  using ChainedItem = int;
};

struct TestNvmCache;

using TestReadHandle = detail::ReadHandleImpl<TestItem>;
using TestWriteHandle = detail::WriteHandleImpl<TestItem>;

struct TestAllocator {
  using Item = int;
  using ChainedItem = int;
  using NvmCacheT = TestNvmCache;

  TestAllocator() {
    ON_CALL(*this, release(testing::_, testing::_))
        .WillByDefault(testing::Invoke(
            [this](TestItem*, bool) { tlRef_.tlStats() -= 1; }));
  }

  MOCK_METHOD2(release, void(TestItem*, bool));

  TestWriteHandle acquire(TestItem* it) {
    tlRef_.tlStats() += 1;
    return TestWriteHandle{it, *this};
  }

  TestWriteHandle getHandle(TestItem* it = nullptr) {
    return it != nullptr ? TestWriteHandle{it, *this} : TestWriteHandle{*this};
  }

  void setHandle(const TestReadHandle& hdl, TestItem* k) {
    hdl.getItemWaitContext()->set(acquire(k));
  }

  void setHandle(const TestWriteHandle& hdl, TestWriteHandle h) {
    hdl.getItemWaitContext()->set(std::move(h));
  }

  void markExpired(TestWriteHandle& hdl) { hdl.markExpired(); }

  void adjustHandleCountForThread_private(int i) { tlRef_.tlStats() += i; }

  // This is no-op. It's just used to construct a ItemHandle with wait-context.
  void bumpHandleWaitBlocks() {}

  util::FastStats<int> tlRef_;
};
} // namespace

namespace detail {
template <typename HandleT>
typename HandleT::CacheT& objcacheGetCache(const HandleT& hdl) {
  return hdl.getCache();
}
} // namespace detail

TEST(ItemHandleTest, GetCache) {
  TestAllocator t;
  auto hdl = t.getHandle();

  // getCache should return reference to CacheAllocator
  auto& cache = detail::objcacheGetCache(hdl);
  EXPECT_EQ(&t, &cache);

  EXPECT_FALSE(hdl.isReady());
  TestItem k;
  t.setHandle(hdl, &k);
  EXPECT_TRUE(hdl.isReady());
}

TEST(ItemHandleTest, WaitContext_set) {
  testing::NiceMock<TestAllocator> t;
  TestItem k;
  auto hdl = t.getHandle();
  EXPECT_FALSE(hdl.isReady());
  t.setHandle(hdl, &k);
  EXPECT_TRUE(hdl.isReady());
  EXPECT_EQ(&k, hdl.get());
}

TEST(ItemHandleTest, WaitContext_set_wait) {
  testing::NiceMock<TestAllocator> t;
  TestItem k;
  auto hdl = t.getHandle();

  folly::fibers::Baton run;
  auto thr = std::thread([&]() {
    run.wait();
    t.setHandle(hdl, &k);
  });

  EXPECT_FALSE(hdl.isReady());
  run.post();
  hdl.wait();
  EXPECT_TRUE(hdl.isReady());
  EXPECT_EQ(&k, hdl.get());
  thr.join();
}

TEST(ItemHandleTest, WaitContext_set_waitSemiFuture) {
  testing::NiceMock<TestAllocator> t;
  TestItem k;
  TestReadHandle hdl = t.getHandle();

  folly::fibers::Baton run;
  folly::fibers::Baton refCountChecked;
  auto thr = std::thread([&]() {
    run.wait();
    t.setHandle(hdl, &k);
    refCountChecked.wait();
  });

  folly::EventBase evb;
  bool called = false;
  auto future = std::move(hdl)
                    .toSemiFuture()
                    .deferValue([&](TestReadHandle h) {
                      called = true;
                      EXPECT_TRUE(h.isReady());
                      EXPECT_EQ(&k, h.get());
                      return h;
                    })
                    .via(&evb);

  EXPECT_FALSE(called);
  run.post();
  EXPECT_FALSE(called);
  hdl = std::move(future).getVia(&evb);
  EXPECT_TRUE(called);
  hdl.reset();

  EXPECT_EQ(0, t.tlRef_.getSnapshot());
  t.tlRef_.forEach([](const auto& tlref) { EXPECT_EQ(0, tlref); });

  refCountChecked.post();
  thr.join();
}

TEST(ItemHandleTest, WaitContext_set_waitSemiFuture_ready) {
  testing::NiceMock<TestAllocator> t;
  TestItem k;
  auto hdl = t.getHandle();
  t.setHandle(hdl, &k);
  EXPECT_TRUE(hdl.isReady());

  folly::EventBase evb;
  bool called = false;
  auto future = std::move(hdl)
                    .toSemiFuture()
                    .deferValue([&](TestReadHandle h) {
                      called = true;
                      EXPECT_TRUE(h.isReady());
                      EXPECT_EQ(&k, h.get());
                    })
                    .via(&evb);
  EXPECT_FALSE(called);
  std::move(future).getVia(&evb);
  EXPECT_TRUE(called);
}

namespace detail {
TEST(ItemHandleTest, WaitContext_readycb) {
  testing::NiceMock<TestAllocator> t;
  TestItem k;
  bool cbFired = false;

  auto cb = [&](TestReadHandle it) {
    EXPECT_EQ(&k, it.get());
    cbFired = true;
  };
  auto hdl = t.getHandle();
  auto retCallback = hdl.onReady(std::move(cb));
  // cb expected to consumed. So retCallback should be empty function
  EXPECT_FALSE(retCallback);

  auto thr =
      std::thread([&t, &k, copy = std::move(hdl)]() { t.setHandle(copy, &k); });
  thr.join();

  EXPECT_TRUE(cbFired);
}

TEST(ItemHandleTest, WaitContext_ready_immediate) {
  testing::NiceMock<TestAllocator> t;
  TestItem k;
  bool cbFired = false;

  auto hdl = t.getHandle();
  t.setHandle(hdl, &k);
  EXPECT_TRUE(hdl.isReady());
  EXPECT_FALSE(cbFired);

  // attaching onReady to ready handle just invokes it inline
  auto retCallback = hdl.onReady([&](TestReadHandle it) {
    EXPECT_EQ(&k, it.get());
    cbFired = true;
  });
  // cb not expected to be consumed. So retCallback should be same function
  EXPECT_TRUE(retCallback);
  EXPECT_FALSE(cbFired);
  retCallback(std::move(hdl));
  EXPECT_TRUE(cbFired);
}
} // namespace detail

TEST(ItemHandleTest, Release) {
  TestAllocator t;
  TestItem k;

  EXPECT_CALL(t, release(testing::_, testing::_)).Times(0);

  auto hdl = t.getHandle(&k);
  EXPECT_EQ(&k, hdl.release());
  EXPECT_EQ(nullptr, hdl.get());
  EXPECT_EQ(nullptr, hdl.release());
}

TEST(ItemHandleTest, ReleaseWithWaitContext) {
  // Try several times to possibly get a race
  for (uint32_t i = 0; i < 100; i++) {
    TestAllocator t;
    TestItem k;

    EXPECT_CALL(t, release(&k, false)).WillOnce(testing::Return());

    auto hdl = t.getHandle();
    folly::fibers::Baton setThreadStarted;
    std::thread setThread{[&] {
      setThreadStarted.post();
      std::this_thread::yield();
      t.setHandle(hdl, &k);
    }};

    setThreadStarted.wait();
    auto p = hdl.release();
    EXPECT_EQ(&k, p);

    EXPECT_EQ(nullptr, hdl.get());
    EXPECT_EQ(nullptr, hdl.release());
    auto hdlDup = t.getHandle(p);
    setThread.join();
  }
}

namespace detail {
TEST(ItemHandleTest, onReadyWithNoWaitContext) {
  testing::NiceMock<TestAllocator> t;
  TestItem k;
  TestReadHandle hdl = t.acquire(&k);
  EXPECT_TRUE(hdl.isReady());
  bool cbFired = false;
  auto myCallback = [&](TestReadHandle it) {
    EXPECT_EQ(&k, it.get());
    cbFired = true;
  };
  auto retCallback = hdl.onReady(std::move(myCallback));
  EXPECT_FALSE(cbFired);
  // cb not expected to be consumed. So retCallback should be same function
  EXPECT_TRUE(retCallback);
  retCallback(std::move(hdl));
  EXPECT_TRUE(cbFired);
}
} // namespace detail

TEST(ItemHandleTest, handleState) {
  testing::NiceMock<TestAllocator> t;

  {
    TestWriteHandle hdl{};
    EXPECT_FALSE(hdl.wentToNvm());
    EXPECT_FALSE(hdl.wasExpired());

    t.markExpired(hdl);
    EXPECT_TRUE(hdl.wasExpired());
  }

  {
    auto flashHdl = t.getHandle();
    EXPECT_FALSE(flashHdl.isReady());

    TestWriteHandle toSetHdl{};
    t.setHandle(flashHdl, std::move(toSetHdl));

    EXPECT_TRUE(flashHdl.isReady());
    EXPECT_FALSE(flashHdl.wasExpired());
  }

  {
    auto flashHdl = t.getHandle();
    EXPECT_FALSE(flashHdl.isReady());

    TestWriteHandle toSetHdl{};
    t.markExpired(toSetHdl);
    t.setHandle(flashHdl, std::move(toSetHdl));

    EXPECT_TRUE(flashHdl.isReady());
    EXPECT_TRUE(flashHdl.wasExpired());
  }
}
} // namespace cachelib
} // namespace facebook
