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

#include <algorithm>
#include <chrono>
#include <ctime>
#include <future>
#include <magic_enum/magic_enum.hpp>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "cachelib/allocator/CCacheAllocator.h"
#include "cachelib/allocator/FreeMemStrategy.h"
#include "cachelib/allocator/LruTailAgeStrategy.h"
#include "cachelib/allocator/MarginalHitsOptimizeStrategy.h"
#include "cachelib/allocator/MarginalHitsStrategy.h"
#include "cachelib/allocator/PoolRebalancer.h"
#include "cachelib/allocator/Util.h"
#include "cachelib/allocator/tests/TestBase.h"
#include "cachelib/common/EventInterface.h"
#include "cachelib/compact_cache/CCacheCreator.h"

namespace facebook {
namespace cachelib {

std::ostream& operator<<(std::ostream& str, AllocatorApiEvent ev) {
  return str << magic_enum::enum_name(ev);
}

std::ostream& operator<<(std::ostream& str, AllocatorApiResult result) {
  return str << magic_enum::enum_name(result);
}

namespace tests {

using EventInterfaceTypes::SizeT;
using EventInterfaceTypes::TtlT;

// Implementation of EventInterface used for testing.
template <typename Key>
class TestEventInterface : public facebook::cachelib::EventInterface<Key> {
 public:
  void record(AllocatorApiEvent event,
              Key key,
              AllocatorApiResult result,
              SizeT valueSize = folly::none,
              TtlT ttl = 0) override {
    lastEvent = event;
    lastKey = key;
    lastResult = result;
    lastValueSize = valueSize;
    lastTtl = ttl;
  }

  void getStats(std::unordered_map<std::string, uint64_t>&) const {}

  void check(AllocatorApiEvent event,
             Key key,
             AllocatorApiResult result,
             const SizeT& valueSize,
             TtlT ttl) {
    ASSERT_EQ(event, lastEvent);
    ASSERT_EQ(key, Key{lastKey});
    ASSERT_EQ(result, lastResult);
    ASSERT_EQ(valueSize, lastValueSize);
    ASSERT_EQ(ttl, lastTtl);
  }

 private:
  AllocatorApiEvent lastEvent{AllocatorApiEvent::INVALID};
  std::string lastKey{};
  AllocatorApiResult lastResult{AllocatorApiResult::FAILED};
  SizeT lastValueSize{folly::none};
  TtlT lastTtl{0};
};

template <typename AllocatorT>
class EventInterfaceTest : public AllocatorTest<AllocatorT> {
 private:
  const size_t kShmInfoSize = 10 * 1024 * 1024; // 10 MB
  using AllocatorTest<AllocatorT>::testShmIsNotRemoved;
  using AllocatorTest<AllocatorT>::testShmIsRemoved;
  using AllocatorTest<AllocatorT>::testInfoShmIsRemoved;

 public:
  // test events related to item allocation
  void testAllocateAndInsertEvents() {
    typename AllocatorT::Config config;
    // create an allocator worth 100 slabs.
    config.setCacheSize(100 * Slab::kSize);
    auto legacyEventTracker =
        std::make_shared<TestEventInterface<typename AllocatorT::Key>>();
    config.setEventTracker(legacyEventTracker);
    AllocatorT alloc(config);

    auto legacyEventTrackerPtr = legacyEventTracker.get();

    std::set<uint32_t> allocSizes{100, 1000, 2000, 5000};
    auto pid = alloc.addPool(
        "default", alloc.getCacheMemoryStats().ramCacheSize, allocSizes);

    const unsigned int keyLen = 100;
    const uint32_t valueSize = 1024;
    const uint32_t ttl = 101;
    const auto key = this->getRandomNewKey(alloc, keyLen);

    {
      auto handle = alloc.allocate(pid, key, valueSize, ttl);
      ASSERT_NE(handle, nullptr);
      legacyEventTrackerPtr->check(AllocatorApiEvent::ALLOCATE, key,
                                   AllocatorApiResult::ALLOCATED, valueSize,
                                   ttl);

      ASSERT_TRUE(alloc.insert(handle));
      legacyEventTrackerPtr->check(AllocatorApiEvent::INSERT, key,
                                   AllocatorApiResult::INSERTED, valueSize,
                                   ttl);
    }

    // replace to an item configured with 0 ttl.
    {
      auto handle = alloc.allocate(pid, key, valueSize, 0);
      ASSERT_NE(handle, nullptr);
      legacyEventTrackerPtr->check(AllocatorApiEvent::ALLOCATE, key,
                                   AllocatorApiResult::ALLOCATED, valueSize, 0);

      ASSERT_TRUE(alloc.insertOrReplace(handle));
      legacyEventTrackerPtr->check(AllocatorApiEvent::INSERT_OR_REPLACE, key,
                                   AllocatorApiResult::REPLACED, valueSize, 0);
    }

    {
      auto handle = alloc.allocate(pid, key, valueSize, ttl);
      ASSERT_NE(handle, nullptr);
      legacyEventTrackerPtr->check(AllocatorApiEvent::ALLOCATE, key,
                                   AllocatorApiResult::ALLOCATED, valueSize,
                                   ttl);

      // An attempt to insert a handle for a key that already exists fails.
      ASSERT_FALSE(alloc.insert(handle));
      legacyEventTrackerPtr->check(AllocatorApiEvent::INSERT, key,
                                   AllocatorApiResult::FAILED, valueSize, ttl);
      // Insert or replace succeeds with replace outcome.
      ASSERT_NE(alloc.insertOrReplace(handle), nullptr);
      legacyEventTrackerPtr->check(AllocatorApiEvent::INSERT_OR_REPLACE, key,
                                   AllocatorApiResult::REPLACED, valueSize,
                                   ttl);
    }

    {
      const auto key2 = this->getRandomNewKey(alloc, keyLen);
      auto handle = alloc.allocate(pid, key2, valueSize, ttl);
      ASSERT_NE(handle, nullptr);
      legacyEventTrackerPtr->check(AllocatorApiEvent::ALLOCATE, key2,
                                   AllocatorApiResult::ALLOCATED, valueSize,
                                   ttl);

      // Insert or replace with a new key succeeds with 'INSERT' result.
      ASSERT_EQ(alloc.insertOrReplace(handle), nullptr);
      legacyEventTrackerPtr->check(AllocatorApiEvent::INSERT_OR_REPLACE, key2,
                                   AllocatorApiResult::INSERTED, valueSize,
                                   ttl);
    }

    {
      const auto key3 = this->getRandomNewKey(alloc, keyLen);
      auto handle = alloc.allocate(pid, key3, valueSize, ttl);
      ASSERT_NE(handle, nullptr);
      legacyEventTrackerPtr->check(AllocatorApiEvent::ALLOCATE, key3,
                                   AllocatorApiResult::ALLOCATED, valueSize,
                                   ttl);

      auto handle2 = alloc.allocateChainedItem(handle, valueSize);
      ASSERT_NE(handle2, nullptr);
      legacyEventTrackerPtr->check(AllocatorApiEvent::ALLOCATE_CHAINED, key3,
                                   AllocatorApiResult::ALLOCATED, valueSize,
                                   ttl);

      // Passing in a null parent handle will not record an event, since it's
      // an invalid use of API and is a no-op.
      ASSERT_THROW(alloc.allocateChainedItem({}, valueSize),
                   std::invalid_argument);
      // Event recorded is still the same as the previou one, indicating no
      // event was recorded.
      legacyEventTrackerPtr->check(AllocatorApiEvent::ALLOCATE_CHAINED, key3,
                                   AllocatorApiResult::ALLOCATED, valueSize,
                                   ttl);
    }
  }

  // make some allocations without evictions and ensure that we are able to
  // fetch them.
  void testFindEvents() {
    typename AllocatorT::Config config;
    // create an allocator worth 100 slabs.
    config.setCacheSize(100 * Slab::kSize);

    auto legacyEventTracker =
        std::make_shared<TestEventInterface<typename AllocatorT::Key>>();
    auto legacyEventTrackerPtr = legacyEventTracker.get();

    config.setEventTracker(legacyEventTracker);

    AllocatorT alloc(config);

    std::set<uint32_t> allocSizes{100, 1000, 2000, 5000};
    auto pid = alloc.addPool(
        "default", alloc.getCacheMemoryStats().ramCacheSize, allocSizes);

    const unsigned int keyLen = 100;
    const uint32_t valueSize = 1024;
    const uint32_t ttl = 102;
    const auto key = this->getRandomNewKey(alloc, keyLen);

    // Try to find non-existing key.
    EXPECT_EQ(alloc.find(key), nullptr);
    legacyEventTrackerPtr->check(AllocatorApiEvent::FIND, key,
                                 AllocatorApiResult::NOT_FOUND, folly::none, 0);

    // Allocate and insert.
    util::allocateAccessible(alloc, pid, key, valueSize, ttl);

    // Find Again
    EXPECT_NE(alloc.find(key), nullptr);
    legacyEventTrackerPtr->check(AllocatorApiEvent::FIND, key,
                                 AllocatorApiResult::FOUND, valueSize, ttl);

    auto handle = alloc.findToWrite(key);
    EXPECT_NE(handle, nullptr);
    handle->updateExpiryTime(0);

    EXPECT_NE(alloc.find(key), nullptr);
    legacyEventTrackerPtr->check(AllocatorApiEvent::FIND, key,
                                 AllocatorApiResult::FOUND, valueSize, 0);
  }

  // make some allocations without evictions, remove them and ensure that they
  // cannot be accessed through find.
  void testRemoveEvents() {
    typename AllocatorT::Config config;
    // create an allocator worth 100 slabs.
    config.setCacheSize(100 * Slab::kSize);
    auto legacyEventTracker =
        std::make_shared<TestEventInterface<typename AllocatorT::Key>>();
    auto legacyEventTrackerPtr = legacyEventTracker.get();

    config.setEventTracker(legacyEventTracker);

    AllocatorT alloc(config);

    std::set<uint32_t> allocSizes{100, 1000, 2000, 5000};
    auto pid = alloc.addPool(
        "default", alloc.getCacheMemoryStats().ramCacheSize, allocSizes);

    const unsigned int keyLen = 100;
    const uint32_t valueSize = 1024;
    const uint32_t ttl = 103;
    const auto key = this->getRandomNewKey(alloc, keyLen);

    // Try to remove non-existing key.
    EXPECT_EQ(alloc.remove(key), AllocatorT::RemoveRes::kNotFoundInRam);
    legacyEventTrackerPtr->check(AllocatorApiEvent::REMOVE, key,
                                 AllocatorApiResult::NOT_FOUND, folly::none, 0);

    // Allocate and insert.
    util::allocateAccessible(alloc, pid, key, valueSize, ttl);

    // Find Again
    EXPECT_EQ(alloc.remove(key), AllocatorT::RemoveRes::kSuccess);
    legacyEventTrackerPtr->check(AllocatorApiEvent::REMOVE, key,
                                 AllocatorApiResult::REMOVED, valueSize, ttl);
  }
};
} // namespace tests
} // namespace cachelib
} // namespace facebook
