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

#include <folly/Random.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "cachelib/allocator/CCacheAllocator.h"
#include "cachelib/allocator/PoolOptimizer.h"
#include "cachelib/compact_cache/CCacheCreator.h"

namespace facebook {
namespace cachelib {
namespace tests {

struct CACHELIB_PACKED_ATTR Int {
  int value;
  /* implicit */ Int(int v = 0) : value(v) {}
  bool operator==(const Int& other) const { return value == other.value; }
  bool operator!=(const Int& other) const { return value != other.value; }
  bool isEmpty() const { return value == 0; }
};

struct SimplePoolOptimizeStrategy : public PoolOptimizeStrategy {
 protected:
  // Choose the pool with highest number of allocs as victim, the pool with
  // lowest number as receiver.
  PoolOptimizeContext pickVictimAndReceiverRegularPoolsImpl(
      const CacheBase& cache) override {
    PoolId victim = -1, receiver = -1;
    uint64_t maxNumAllocs = 0, minNumAllocs = (1UL << 30);
    for (auto pid : cache.getRegularPoolIds()) {
      auto activeAllocs = cache.getPoolStats(pid).numActiveAllocs();
      if (activeAllocs < minNumAllocs) {
        minNumAllocs = activeAllocs;
        receiver = pid;
      }
      if (activeAllocs > maxNumAllocs) {
        maxNumAllocs = activeAllocs;
        victim = pid;
      }
    }
    return {victim, receiver};
  }

  // Choose the compact cache with lowest number of set miss as victim, and the
  // compact cache with highest number of set miss as receiver.
  PoolOptimizeContext pickVictimAndReceiverCompactCachesImpl(
      const CacheBase& cache) override {
    PoolId victim = -1, receiver = -1;
    uint64_t maxSetMiss = 0, minSetMiss = (1UL << 30);
    for (auto pid : cache.getCCachePoolIds()) {
      if (cache.autoResizeEnabledForPool(pid)) {
        auto numSetMiss = cache.getCompactCache(pid).getStats().setMiss;
        if (numSetMiss < minSetMiss) {
          minSetMiss = numSetMiss;
          victim = pid;
        }
        if (numSetMiss >= maxSetMiss) {
          maxSetMiss = numSetMiss;
          receiver = pid;
        }
      }
    }
    return {victim, receiver};
  }
};

template <typename AllocatorT>
class SimplePoolOptimizationTest : public testing::Test {
 public:
  void testStopWorkersStopsPoolOptimizer() {
    struct CountingStrategy : public PoolOptimizeStrategy {
      std::atomic<int> calls{0};

     protected:
      PoolOptimizeContext pickVictimAndReceiverRegularPoolsImpl(
          const CacheBase&) override {
        ++calls;
        return kNoOpContext;
      }
    };
    auto strategy = std::make_shared<CountingStrategy>();
    typename AllocatorT::Config config;
    config.setCacheSize(30 * Slab::kSize);
    config.enablePoolOptimizer(strategy, std::chrono::seconds{1},
                               std::chrono::seconds{1}, 1);
    AllocatorT alloc(config);
    for (int i = 0; i < 100 && strategy->calls == 0; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    ASSERT_GT(strategy->calls.load(), 0);

    ASSERT_TRUE(alloc.stopWorkers());
    const int calls = strategy->calls.load();
    std::this_thread::sleep_for(std::chrono::seconds{2});
    EXPECT_EQ(calls, strategy->calls.load());
  }

  void testPoolOptimizerBasic() {
    typename AllocatorT::Config config;
    config.setCacheSize(30 * Slab::kSize);
    config.enableCompactCache();
    config.enablePoolOptimizer(std::make_shared<SimplePoolOptimizeStrategy>(),
                               std::chrono::seconds{1} /* regular interval */,
                               std::chrono::seconds{1} /* ccache interval */,
                               1 /* ccache resize step percent */);

    const auto itemSize = 1024;
    const auto numItems = 100;

    AllocatorT alloc(config);
    const size_t numBytes = alloc.getCacheMemoryStats().ramCacheSize;
    const size_t initialPoolSize = numBytes / 5;
    auto pid0 = alloc.addPool("pool-0", initialPoolSize);
    auto pid1 = alloc.addPool("pool-1", initialPoolSize);

    ASSERT_EQ(initialPoolSize, alloc.getPoolStats(pid0).poolSize);
    ASSERT_EQ(initialPoolSize, alloc.getPoolStats(pid1).poolSize);

    using CCacheT = typename CCacheCreator<CCacheAllocator, Int, Int>::type;
    auto& ccache0 =
        *alloc.template addCompactCache<CCacheT>("C0", initialPoolSize);
    alloc.setPoolOptimizerFor(alloc.getPoolId("C0"), true);
    auto& ccache1 =
        *alloc.template addCompactCache<CCacheT>("C1", initialPoolSize);
    alloc.setPoolOptimizerFor(alloc.getPoolId("C1"), true);
    auto& ccache2 =
        *alloc.template addCompactCache<CCacheT>("C2", initialPoolSize);

    ASSERT_EQ(initialPoolSize, ccache0.getConfiguredSize());
    ASSERT_EQ(initialPoolSize, ccache1.getConfiguredSize());
    ASSERT_EQ(initialPoolSize, ccache2.getConfiguredSize());

    typename CCacheT::Value dummyValue(0xAA);
    for (uint32_t i = 0; i < numItems; i++) {
      // populate regular pools: more allocs in pool 1 than pool 0
      ASSERT_NE(nullptr,
                util::allocateAccessible(alloc, pid0, "0-" + std::to_string(i),
                                         itemSize));
      ASSERT_NE(nullptr,
                util::allocateAccessible(alloc, pid1, "1-" + std::to_string(i),
                                         itemSize));
      ASSERT_NE(nullptr,
                util::allocateAccessible(alloc, pid1, "1+" + std::to_string(i),
                                         itemSize));

      // populate compact caches: more set miss in pool 2 than pool 1 than pool
      // 0
      ASSERT_EQ(CCacheReturn::NOTFOUND, ccache0.set(i, &dummyValue));
      ASSERT_EQ(CCacheReturn::NOTFOUND, ccache1.set(i * 2, &dummyValue));
      ASSERT_EQ(CCacheReturn::NOTFOUND, ccache1.set(i * 2 + 1, &dummyValue));
      ASSERT_EQ(CCacheReturn::NOTFOUND, ccache2.set(i * 3, &dummyValue));
      ASSERT_EQ(CCacheReturn::NOTFOUND, ccache2.set(i * 3 + 1, &dummyValue));
      ASSERT_EQ(CCacheReturn::NOTFOUND, ccache2.set(i * 3 + 2, &dummyValue));
    }
    ASSERT_LT(alloc.getPoolStats(pid0).numActiveAllocs(),
              alloc.getPoolStats(pid1).numActiveAllocs());
    ASSERT_LT(ccache0.getStats().setMiss, ccache1.getStats().setMiss);
    ASSERT_LT(ccache1.getStats().setMiss, ccache2.getStats().setMiss);

    // sleep to let the pool optimizer work
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // stop pool optimizer worker
    alloc.stopPoolOptimizer();
    {
      // expect the worker to move memory from regular pool 1 to regular pool 0
      const int delta0 = alloc.getPoolStats(pid0).poolSize - initialPoolSize;
      const int delta1 = alloc.getPoolStats(pid1).poolSize - initialPoolSize;
      EXPECT_GT(delta0, 0);
      EXPECT_LT(delta1, 0);
      EXPECT_EQ(delta0 + delta1, 0);
    }

    {
      // expect the worker to move memory from ccache 0 to ccache 1 (because
      // ccache2 has auto-resizing disabled)
      const int delta0 = ccache0.getConfiguredSize() - initialPoolSize;
      const int delta1 = ccache1.getConfiguredSize() - initialPoolSize;
      EXPECT_LT(delta0, 0);
      EXPECT_GT(delta1, 0);
      EXPECT_EQ(delta0 + delta1, 0);
      EXPECT_EQ(initialPoolSize, ccache2.getConfiguredSize());
    }
  }
};
} // namespace tests
} // namespace cachelib
} // namespace facebook
