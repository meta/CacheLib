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

#include <fmt/core.h>
#include <folly/Benchmark.h>
#include <folly/Random.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/allocator/KAllocation.h"
#include "cachelib/allocator/tests/NvmTestUtils.h"
#include "cachelib/common/TestUtils.h"
#include "cachelib/object_cache/ObjectCache.h"
#include "cachelib/object_cache/tests/gen-cpp2/test_object_types.h"

namespace facebook::cachelib::objcache2::test {
namespace {
struct Foo {
  int a{};
  int b{};
  int c{};
};

struct Foo2 {
  int d{};
  int e{};
  int f{};
};

struct Foo3 {
  explicit Foo3(int& n) : numDtors_{n} {}
  ~Foo3() { numDtors_++; }
  int& numDtors_;
};

struct FooBase {
  virtual ~FooBase() = default;
};

struct Foo4 : FooBase {
  int a{};
  int b{};
  int c{};
};

struct Foo5 : FooBase {
  int d{};
  int e{};
  int f{};
};

struct MemoryConsumer {
  std::vector<char> data;
  explicit MemoryConsumer(size_t sizeBytes) : data(sizeBytes) {}
};

} // namespace

template <typename AllocatorT>
class ObjectCacheTest : public ::testing::Test {
 public:
  using ObjectCache = ObjectCache<AllocatorT>;
  using ObjectCacheConfig = typename ObjectCache::Config;

  void testMemorySizeControlSteps(
      ObjCacheSizeControlMode mode,
      const std::vector<std::pair<uint64_t, size_t>>& steps,
      uint32_t minCacheSizePercent) {
    std::atomic<uint64_t> memoryBytes{15'000};
    ObjectCacheConfig config;
    config.setCacheName("memory_size_control")
        .setCacheCapacity(100, 10'000, 60'000)
        .setItemDestructor(
            [](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    config.setObjectSizeControllerMode(mode, 20'000, 10'000);
    config.setMinCacheSizePercent(minCacheSizePercent);
    config.setFreeMemCb([&]() { return memoryBytes.load(); });
    config.setRSSMemCb([&]() { return memoryBytes.load(); });
    auto objcache = ObjectCache::create(config);
    ASSERT_TRUE(objcache->stopAllWorkers(std::chrono::seconds{0}));
    auto& controller = *objcache->sizeController_;

    size_t key = 0;
    for (auto [reading, expectedLimit] : steps) {
      SCOPED_TRACE(fmt::format("memory reading {}", reading));
      // Fill the current capacity so growth is eligible on the next pass.
      const auto entriesToInsert =
          objcache->getCurrentEntriesLimit() - objcache->getNumEntries();
      for (size_t i = 0; i < entriesToInsert; ++i) {
        auto [status, object, replaced] = objcache->insertOrReplace(
            fmt::format("key_{}", key++), std::make_unique<Foo>(), 100);
        ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, status);
      }

      // Run exactly one pass; no controller can race the next memory reading.
      memoryBytes = reading;
      const auto runCount = controller.getRunCount();
      ASSERT_TRUE(controller.start(std::chrono::hours{1}, "size_test"));
      const bool ran = test_util::eventuallyTrue(
          [&]() { return controller.getRunCount() > runCount; }, 3);
      if (!ran) {
        memoryBytes = 15'000;
      }
      ASSERT_TRUE(controller.stop(std::chrono::seconds{0}));
      ASSERT_TRUE(ran);
      ASSERT_EQ(expectedLimit, objcache->getCurrentEntriesLimit());
    }
  }

  void testMinCacheSizePercent(ObjCacheSizeControlMode mode) {
    std::atomic<uint64_t> memoryBytes{15'000};
    ObjectCacheConfig config;
    config.setCacheName("minimum_cache_size")
        .setCacheCapacity(200, 10'000, 60'000)
        .setMinCacheSizePercent(70)
        .setItemDestructor(
            [](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    config.setObjectSizeControllerMode(mode, 20'000, 10'000);
    config.setFreeMemCb([&]() { return memoryBytes.load(); });
    config.setRSSMemCb([&]() { return memoryBytes.load(); });
    auto objcache = ObjectCache::create(config);
    ASSERT_TRUE(objcache->stopAllWorkers(std::chrono::seconds{0}));
    EXPECT_EQ("70",
              objcache->serializeConfigParams().at("minCacheSizePercent"));
    auto& controller = *objcache->sizeController_;

    auto runController = [&](size_t expectedLimit) {
      const auto runCount = controller.getRunCount();
      ASSERT_TRUE(controller.start(std::chrono::hours{1}, "minimum_test"));
      const bool ran = test_util::eventuallyTrue(
          [&]() { return controller.getRunCount() > runCount; }, 3);
      // Release pressure before stopping so a regression in FreeMemoryOnly
      // fails the test instead of leaving its worker in an endless loop.
      memoryBytes = mode == ResidentMemory ? 0 : 30'000;
      ASSERT_TRUE(controller.stop(std::chrono::seconds{0}));
      ASSERT_TRUE(ran);
      EXPECT_EQ(expectedLimit, objcache->getCurrentEntriesLimit());
    };
    auto runUnderPressure = [&](size_t expectedLimit) {
      memoryBytes = mode == ResidentMemory ? 30'000 : 0;
      runController(expectedLimit);
    };

    runUnderPressure(200); // No average size is available in an empty cache.
    for (size_t i = 0; i < 100; ++i) {
      auto [status, object, replaced] = objcache->insertOrReplace(
          fmt::format("key_{}", i), std::make_unique<Foo>(), 100);
      ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, status);
    }

    // The 7,000-byte floor holds 70 objects, not 70% of the 200-entry ceiling.
    runUnderPressure(70);
    EXPECT_EQ(7'000, objcache->getTotalObjectSize());
    runUnderPressure(70);

    // Change the cached population to larger objects at the same byte budget.
    for (size_t i = 0; i < 100; ++i) {
      objcache->remove(fmt::format("key_{}", i));
    }
    for (size_t i = 0; i < 70; ++i) {
      auto [status, object, replaced] = objcache->insertOrReplace(
          fmt::format("large_{}", i), std::make_unique<Foo>(), 200);
      ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, status);
    }
    runUnderPressure(35);
    EXPECT_EQ(7'000, objcache->getTotalObjectSize());

    // The floor follows budget updates and rounds up to a whole object:
    // 70% of 5,001 bytes needs 18 objects of 200 bytes each.
    ASSERT_TRUE(objcache->setTotalObjectSizeLimit(5'001));
    runUnderPressure(18);
    EXPECT_EQ(3'600, objcache->getTotalObjectSize());
    runUnderPressure(18);

    memoryBytes = mode == ResidentMemory ? 0 : 30'000;
    runController(mode == FreeMemoryOnly ? 20 : 25);
  }

  void testFreeMemoryShrinkCounters() {
    uint64_t freeMemoryBytes = 9'500;
    ObjectCacheConfig config;
    config.setCacheName("free_memory_shrink_counters")
        .setCacheCapacity(100, 10'000, 60'000)
        .setItemDestructor(
            [](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    config.setObjectSizeControllerMode(FreeMemory, 20'000, 10'000);
    config.setFreeMemCb([&]() { return freeMemoryBytes; });
    auto objcache = ObjectCache::create(config);
    ASSERT_TRUE(objcache->stopAllWorkers(std::chrono::seconds{0}));
    auto& controller = *objcache->sizeController_;

    auto runController = [&](size_t expectedLimit, uint64_t expectedCalls) {
      SCOPED_TRACE(fmt::format("free memory {}", freeMemoryBytes));
      const auto runCount = controller.getRunCount();
      ASSERT_TRUE(controller.start(std::chrono::hours{1}, "counter_test"));
      const bool ran = test_util::eventuallyTrue(
          [&]() { return controller.getRunCount() > runCount; }, 3);
      ASSERT_TRUE(controller.stop(std::chrono::seconds{0}));
      ASSERT_TRUE(ran);
      EXPECT_EQ(expectedLimit, objcache->getCurrentEntriesLimit());
      bool foundCounter = false;
      objcache->getObjectCacheCounters(
          util::CounterVisitor{[&](folly::StringPiece name, double value,
                                   util::CounterVisitor::CounterType type) {
            if (name == "objcache.size_controller.free_memory_shrink_calls") {
              foundCounter = true;
              EXPECT_EQ(expectedCalls, value);
              EXPECT_EQ(util::CounterVisitor::CounterType::RATE, type);
            }
          }});
      EXPECT_TRUE(foundCounter);
    };

    // An empty or warming cache does not run the pressure shrink path.
    runController(100, 0);
    for (size_t i = 0; i < 100; ++i) {
      auto [status, object, replaced] = objcache->insertOrReplace(
          fmt::format("key_{}", i), std::make_unique<Foo>(), 100);
      ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, status);
      if (i == 39) {
        runController(100, 0);
      }
    }

    freeMemoryBytes = 15'000;
    runController(100, 0);
    freeMemoryBytes = 10'000;
    runController(100, 0);
    freeMemoryBytes = 9'999; // Less than one object's worth of pressure.
    runController(100, 0);
    freeMemoryBytes = 9'500;
    runController(95, 1);

    // A budget reduction already requires more shrinking than RAM pressure.
    ASSERT_TRUE(objcache->setTotalObjectSizeLimit(5'000));
    runController(50, 1);
    ASSERT_TRUE(objcache->setTotalObjectSizeLimit(10'000));
    runController(45, 2);

    freeMemoryBytes = 0;
    runController(1, 3);
    runController(1, 3); // The controller keeps one entry for size estimation.
    freeMemoryBytes = 30'000;
    runController(100, 3);
  }

  void testGetAllocSize() {
    std::vector<uint8_t> maxKeySizes{};
    std::vector<uint32_t> allocSizes{};

    for (uint8_t keySize = 8; keySize < 255; keySize++) {
      maxKeySizes.push_back(keySize);
      allocSizes.push_back(ObjectCache::getL1AllocSize(keySize));
    }

    for (size_t i = 0; i < maxKeySizes.size(); i++) {
      EXPECT_TRUE(allocSizes[i] >= ObjectCache::kL1AllocSizeMin);
      EXPECT_TRUE(maxKeySizes[i] + sizeof(ObjectCacheItem) +
                      ObjectCache::getValueAlignmentPadding(maxKeySizes[i]) +
                      sizeof(typename AllocatorT::Item) <=
                  allocSizes[i]);
      EXPECT_TRUE(allocSizes[i] % 8 == 0);
    }
  }

  void testConfigValidation() {
    {
      ObjectCacheConfig config;
      config.setCacheName("test").setCacheCapacity(10'000);
      EXPECT_THROW(config.validate(), std::invalid_argument);

      EXPECT_THROW(ObjectCache::create(config), std::invalid_argument);

      config.setItemDestructor(
          [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
      EXPECT_NO_THROW(ObjectCache::create(config));

      config.setCacheName("");
      EXPECT_THROW(ObjectCache::create(config), std::invalid_argument);
    }

    {
      // test size-aware cache config
      ObjectCacheConfig config;
      config.setCacheName("test").setItemDestructor(
          [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });

      // missing sizeControllerIntervalMs
      EXPECT_THROW(config.setCacheCapacity(10'000, 100'000),
                   std::invalid_argument);

      // missing totalObjectSizeLimit
      EXPECT_THROW(config.setCacheCapacity(10'000, 0, 10),
                   std::invalid_argument);

      config.setCacheCapacity(10'000, 100'000, 10);
      EXPECT_NO_THROW(config.validate());
    }

    {
      ObjectCacheConfig config;
      config.setCacheName("minimum_size_validation")
          .setCacheCapacity(100, 0, 0)
          .setItemDestructor(
              [](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
      EXPECT_NO_THROW(config.validate());
      config.setMinCacheSizePercent(70);
      EXPECT_THROW(config.validate(), std::invalid_argument);
      config.setCacheCapacity(100, 10'000, 60'000);
      EXPECT_NO_THROW(config.validate());
      config.setMinCacheSizePercent(100);
      EXPECT_NO_THROW(config.validate());
      EXPECT_THROW(config.setMinCacheSizePercent(101), std::invalid_argument);
      config.minCacheSizePercent = 101;
      EXPECT_THROW(config.validate(), std::invalid_argument);
    }

    {
      ObjectCacheConfig config;
      // invalid thread count
      EXPECT_THROW(config.enablePersistence(
                       0, "persistent_file",
                       [&](typename ObjectCache::Serializer serializer) {
                         return serializer.template serialize<ThriftFoo>();
                       },
                       [&](typename ObjectCache::Deserializer deserializer) {
                         return deserializer.template deserialize<ThriftFoo>();
                       }),
                   std::invalid_argument);
      // invalid file path
      EXPECT_THROW(config.enablePersistence(
                       1, "",
                       [&](typename ObjectCache::Serializer serializer) {
                         return serializer.template serialize<ThriftFoo>();
                       },
                       [&](typename ObjectCache::Deserializer deserializer) {
                         return deserializer.template deserialize<ThriftFoo>();
                       }),
                   std::invalid_argument);
      // missing serialize callback
      EXPECT_THROW(config.enablePersistence(
                       1, "persistent_file", nullptr,
                       [&](typename ObjectCache::Deserializer deserializer) {
                         return deserializer.template deserialize<ThriftFoo>();
                       }),
                   std::invalid_argument);
      // missing deserialize callback
      EXPECT_THROW(config.enablePersistence(
                       1, "persistent_file",
                       [&](typename ObjectCache::Serializer serializer) {
                         return serializer.template serialize<ThriftFoo>();
                       },
                       nullptr),
                   std::invalid_argument);
    }
  }

  void testSetShardName() {
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(100'000).setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    {
      auto objcache = ObjectCache::create(config);
      auto poolIds = objcache->getL1Cache().getPoolIds();
      EXPECT_EQ(1, poolIds.size());
      EXPECT_EQ("pool", // use default shard names
                objcache->getL1Cache().getPoolName(PoolId(0)));
    }

    {
      std::string shardName = "my_shard";
      config.setNumShards(1).setShardName(shardName);
      auto objcache = ObjectCache::create(config);
      auto poolIds = objcache->getL1Cache().getPoolIds();
      ASSERT_EQ(poolIds.size(), 1);
      EXPECT_EQ(shardName, objcache->getL1Cache().getPoolName(PoolId(0)));
    }

    {
      std::string shardName = "my_shard";
      config.setNumShards(5).setShardName(shardName);
      auto objcache = ObjectCache::create(config);
      auto poolIds = objcache->getL1Cache().getPoolIds();
      for (size_t i = 0; i < poolIds.size(); i++) {
        EXPECT_EQ(fmt::format("{}_{}", shardName, i),
                  objcache->getL1Cache().getPoolName(PoolId(i)));
      }
    }
  }

  void testSetEvictionPolicyConfig() {
    typename ObjectCache::EvictionPolicyConfig evictionPolicyConfig;
    evictionPolicyConfig.updateOnRead = false;
    evictionPolicyConfig.updateOnWrite = true;
    size_t numEntriesLimit = 10;

    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(numEntriesLimit)
        .setItemDestructor(
            [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); })
        .setEvictionPolicyConfig(std::move(evictionPolicyConfig));

    auto objcache = ObjectCache::create(config);
    // add #numEntriesLimit objects
    for (size_t i = 1; i <= numEntriesLimit; i++) {
      auto [allocRes, _, __] = objcache->insertOrReplace(
          fmt::format("Foo_{}", i), std::make_unique<Foo>());
      ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, allocRes);
    }

    {
      // read-only access won't promote the object
      auto found = objcache->template find<Foo>("Foo_1");
      EXPECT_NE(nullptr, found);
    }

    // add one more object to trigger eviction
    auto res = objcache->insertOrReplace(
        fmt::format("Foo_{}", numEntriesLimit + 1), std::make_unique<Foo>());
    ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, std::get<0>(res));

    {
      // Foo_1 should be evicted
      auto found = objcache->template find<Foo>("Foo_1");
      EXPECT_EQ(nullptr, found);
    }

    {
      // write access will promote the object
      auto found = objcache->template findToWrite<Foo>("Foo_2");
      EXPECT_NE(nullptr, found);
    }

    // add one more object to trigger eviction
    res = objcache->insertOrReplace(fmt::format("Foo_{}", numEntriesLimit + 2),
                                    std::make_unique<Foo>());
    ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, std::get<0>(res));

    {
      // Foo_2 should not be evicted
      auto found = objcache->template find<Foo>("Foo_2");
      EXPECT_NE(nullptr, found);
    }
  }

  void testSimple() {
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000);
    config.setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    auto found1 = objcache->template find<Foo>("Foo");
    EXPECT_EQ(nullptr, found1);

    auto foo = std::make_unique<Foo>();
    foo->a = 1;
    foo->b = 2;
    foo->c = 3;
    auto [allocRes, ptr, _] = objcache->insertOrReplace("Foo", std::move(foo));
    EXPECT_EQ(ObjectCache::AllocStatus::kSuccess, allocRes);
    ASSERT_NE(nullptr, ptr);
    EXPECT_EQ(1, ptr->a);
    EXPECT_EQ(2, ptr->b);
    EXPECT_EQ(3, ptr->c);

    auto found2 = objcache->template find<Foo>("Foo");
    ASSERT_NE(nullptr, found2);
    EXPECT_EQ(1, found2->a);
    EXPECT_EQ(2, found2->b);
    EXPECT_EQ(3, found2->c);
  }

  void testMultiType() {
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000).setItemDestructor(
        [&](ObjectCacheDestructorData data) {
          if (data.key == KAllocation::Key{"Foo"}) {
            data.deleteObject<Foo>();
          } else if (data.key == KAllocation::Key{"Foo2"}) {
            data.deleteObject<Foo2>();
          }
        });

    auto objcache = ObjectCache::create(config);

    auto foo = std::make_unique<Foo>();
    foo->a = 1;
    foo->b = 2;
    foo->c = 3;
    auto res1 = objcache->insertOrReplace("Foo", std::move(foo));
    EXPECT_EQ(ObjectCache::AllocStatus::kSuccess, std::get<0>(res1));

    auto found1 = objcache->template find<Foo>("Foo");
    ASSERT_NE(nullptr, found1);
    EXPECT_EQ(1, found1->a);
    EXPECT_EQ(2, found1->b);
    EXPECT_EQ(3, found1->c);

    auto foo2 = std::make_unique<Foo2>();
    foo2->d = 4;
    foo2->e = 5;
    foo2->f = 6;
    auto res2 = objcache->insertOrReplace("Foo2", std::move(foo2));
    EXPECT_EQ(ObjectCache::AllocStatus::kSuccess, std::get<0>(res2));

    auto found2 = objcache->template find<Foo2>("Foo2");
    ASSERT_NE(nullptr, found2);
    EXPECT_EQ(4, found2->d);
    EXPECT_EQ(5, found2->e);
    EXPECT_EQ(6, found2->f);
  }

  void testMultiTypePolymorphism() {
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000).setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<FooBase>(); });

    auto objcache = ObjectCache::create(config);

    auto foo4 = std::make_unique<Foo4>();
    foo4->a = 1;
    foo4->b = 2;
    foo4->c = 3;
    auto res1 = objcache->insertOrReplace("Foo4", std::move(foo4));
    EXPECT_EQ(ObjectCache::AllocStatus::kSuccess, std::get<0>(res1));

    auto found1 = objcache->template find<Foo4>("Foo4");
    ASSERT_NE(nullptr, found1);
    EXPECT_EQ(1, found1->a);
    EXPECT_EQ(2, found1->b);
    EXPECT_EQ(3, found1->c);

    auto foo5 = std::make_unique<Foo5>();
    foo5->d = 4;
    foo5->e = 5;
    foo5->f = 6;
    auto res2 = objcache->insertOrReplace("Foo5", std::move(foo5));
    EXPECT_EQ(ObjectCache::AllocStatus::kSuccess, std::get<0>(res2));

    auto found2 = objcache->template find<Foo5>("Foo5");
    ASSERT_NE(nullptr, found2);
    EXPECT_EQ(4, found2->d);
    EXPECT_EQ(5, found2->e);
    EXPECT_EQ(6, found2->f);
  }

  void testUserItemDestructor() {
    int numDtors = 0;
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000).setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo3>(); });
    auto objcache = ObjectCache::create(config);
    for (int i = 0; i < 10; i++) {
      objcache->insertOrReplace(fmt::format("key_{}", i),
                                std::make_unique<Foo3>(numDtors));
    }
    for (int i = 0; i < 10; i++) {
      ASSERT_TRUE(objcache->remove(fmt::format("key_{}", i)));
    }
    ASSERT_EQ(10, numDtors);
  }

  void testExpiration() {
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000).setItemDestructor(
        [&](ObjectCacheDestructorData data) {
          EXPECT_EQ(data.context, ObjectCacheDestructorContext::kRemoved);
          data.deleteObject<Foo>();
        });
    auto objcache = ObjectCache::create(config);

    auto foo = std::make_unique<Foo>();
    foo->a = 1;
    foo->b = 2;
    foo->c = 3;

    int ttlSecs = 2;

    objcache->insertOrReplace("Foo", std::move(foo), 0 /*object size*/,
                              ttlSecs);

    auto found1 = objcache->template find<Foo>("Foo");
    ASSERT_NE(nullptr, found1);
    EXPECT_EQ(1, found1->a);
    EXPECT_EQ(2, found1->b);
    EXPECT_EQ(3, found1->c);

    std::this_thread::sleep_for(std::chrono::seconds{3});
    auto found2 = objcache->template find<Foo>("Foo");
    ASSERT_EQ(nullptr, found2);
  }

  void testInspectCacheIncludingExpiredUntilReaped() {
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000)
        .setDelayCacheWorkersStart()
        .setItemReaperInterval(std::chrono::milliseconds{10})
        .setItemDestructor(
            [](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    auto foo = std::make_unique<Foo>();
    foo->a = 1;
    auto [status, inserted, replaced] = objcache->insertOrReplace(
        "Foo", std::move(foo), 0 /* objectSize */, 1000 /* ttlSecs */);
    ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, status);
    ASSERT_EQ(nullptr, replaced);
    ASSERT_TRUE(
        objcache->updateExpiryTimeSec(inserted, util::getCurrentTimeSec() - 1));
    inserted.reset();

    EXPECT_EQ(nullptr, objcache->template find<Foo>("Foo"));
    auto expired = objcache->template inspectCache<Foo>("Foo");
    ASSERT_NE(nullptr, expired);
    EXPECT_EQ(1, expired->a);

    const auto traversalsBefore =
        objcache->getL1Cache().getReaperStats().numTraversals;
    objcache->startCacheWorkers();
    ASSERT_EVENTUALLY_TRUE([&] {
      return objcache->getL1Cache().getReaperStats().numTraversals >
             traversalsBefore;
    });
    EXPECT_EQ(expired.get(), objcache->template inspectCache<Foo>("Foo").get());

    expired.reset();
    ASSERT_EVENTUALLY_TRUE(
        [&] { return objcache->template inspectCache<Foo>("Foo") == nullptr; });
  }

  void testPreRemoveCallbackOrdering() {
    std::mutex eventsMutex;
    std::vector<std::string> events;
    auto recordEvent = [&](std::string event) {
      std::lock_guard lock(eventsMutex);
      events.push_back(std::move(event));
    };

    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(2)
        .setDelayCacheWorkersStart()
        .setItemReaperInterval(std::chrono::milliseconds{10})
        .setPreRemoveCb([&](const ObjectCachePreRemoveData& data) {
          EXPECT_EQ(data.context == RemoveContext::kEviction,
                    data.isEviction());
          const char* context = "unknown";
          switch (data.context) {
          case RemoveContext::kEviction:
            context = "evicted";
            break;
          case RemoveContext::kNormal:
            context = "removed";
            break;
          case RemoveContext::kExpired:
            context = "expired";
            break;
          }
          recordEvent(fmt::format("pre:{}:{}", data.key.str(), context));
        })
        .setItemDestructor([&](ObjectCacheDestructorData data) {
          recordEvent(fmt::format("destroy:{}:{}",
                                  data.key.str(),
                                  data.isEviction() ? "evicted" : "removed"));
          data.deleteObject<Foo>();
        });
    auto objcache = ObjectCache::create(config);

    auto [evictedStatus, evicted, evictedReplaced] =
        objcache->insertOrReplace("evicted", std::make_unique<Foo>());
    ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, evictedStatus);
    ASSERT_EQ(nullptr, evictedReplaced);
    evicted.reset();

    auto [explicitStatus, explicitItem, explicitReplaced] =
        objcache->insertOrReplace("explicit", std::make_unique<Foo>());
    ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, explicitStatus);
    ASSERT_EQ(nullptr, explicitReplaced);
    explicitItem.reset();

    auto [expiredStatus, expired, expiredReplaced] =
        objcache->insertOrReplace("expired", std::make_unique<Foo>(),
                                  0 /* objectSize */, 1000 /* ttlSecs */);
    ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, expiredStatus);
    ASSERT_EQ(nullptr, expiredReplaced);
    ASSERT_TRUE(
        objcache->updateExpiryTimeSec(expired, util::getCurrentTimeSec() - 1));
    expired.reset();
    ASSERT_TRUE(objcache->remove("explicit"));
    objcache->startCacheWorkers();
    ASSERT_EVENTUALLY_TRUE([&] {
      std::lock_guard lock(eventsMutex);
      return events.size() >= 6;
    });

    auto [oldStatus, oldItem, oldReplaced] =
        objcache->insertOrReplace("replacement", std::make_unique<Foo>());
    ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, oldStatus);
    ASSERT_EQ(nullptr, oldReplaced);
    oldItem.reset();
    auto [newStatus, newItem, replacedItem] =
        objcache->insertOrReplace("replacement", std::make_unique<Foo>());
    ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, newStatus);
    ASSERT_NE(nullptr, replacedItem);
    replacedItem.reset();

    newItem.reset();
    ASSERT_TRUE(objcache->remove("replacement"));
    objcache->stopAllWorkers();

    const std::vector<std::string> expected{
        "pre:evicted:evicted",         "destroy:evicted:evicted",
        "pre:explicit:removed",        "destroy:explicit:removed",
        "pre:expired:expired",         "destroy:expired:removed",
        "destroy:replacement:removed", "pre:replacement:removed",
        "destroy:replacement:removed",
    };
    std::lock_guard lock(eventsMutex);
    EXPECT_EQ(expected, events);
  }

  void testExpirationWithCustomizedReaper() {
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000)
        .setItemDestructor(
            [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); })
        .setItemReaperInterval(std::chrono::seconds{1});
    auto objcache = ObjectCache::create(config);

    auto foo = std::make_unique<Foo>();
    foo->a = 1;
    foo->b = 2;
    foo->c = 3;

    objcache->insertOrReplace("Foo", std::move(foo), 0 /*object size*/,
                              2 /* seconds */);

    auto found1 = objcache->template find<Foo>("Foo");
    ASSERT_NE(nullptr, found1);
    EXPECT_EQ(1, found1->a);
    EXPECT_EQ(2, found1->b);
    EXPECT_EQ(3, found1->c);

    std::this_thread::sleep_for(std::chrono::seconds{3});
    auto found2 = objcache->template find<Foo>("Foo");
    ASSERT_EQ(nullptr, found2);

    auto stats = objcache->getL1Cache().getReaperStats();
    EXPECT_LE(3, stats.numTraversals);
  }

  void testReplace() {
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000).setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    auto foo1 = std::make_unique<Foo>();
    foo1->a = 1;
    foo1->b = 2;
    foo1->c = 3;

    auto [res1, ptr1, replaced1] =
        objcache->insertOrReplace("Foo", std::move(foo1));
    EXPECT_EQ(ObjectCache::AllocStatus::kSuccess, res1);
    EXPECT_EQ(1, ptr1->a);
    EXPECT_EQ(2, ptr1->b);
    EXPECT_EQ(3, ptr1->c);
    EXPECT_EQ(nullptr, replaced1);

    auto found1 = objcache->template find<Foo>("Foo");
    ASSERT_NE(nullptr, found1);
    EXPECT_EQ(1, found1->a);
    EXPECT_EQ(2, found1->b);
    EXPECT_EQ(3, found1->c);

    auto foo2 = std::make_unique<Foo>();
    foo2->a = 10;
    foo2->b = 20;
    foo2->c = 30;
    auto [res2, ptr2, replaced2] =
        objcache->insertOrReplace("Foo", std::move(foo2));
    EXPECT_EQ(ObjectCache::AllocStatus::kSuccess, res2);
    EXPECT_EQ(10, ptr2->a);
    EXPECT_EQ(20, ptr2->b);
    EXPECT_EQ(30, ptr2->c);
    ASSERT_NE(nullptr, replaced2);
    EXPECT_EQ(1, replaced2->a);
    EXPECT_EQ(2, replaced2->b);
    EXPECT_EQ(3, replaced2->c);

    auto found2 = objcache->template find<Foo>("Foo");
    ASSERT_NE(nullptr, found2);
    EXPECT_EQ(10, found2->a);
    EXPECT_EQ(20, found2->b);
    EXPECT_EQ(30, found2->c);
  }

  void testUniqueInsert() {
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000).setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    // test bad API call
    ASSERT_THROW(objcache->insert("Foo", std::move(std::make_unique<Foo>()),
                                  2 /* TTL seconds */),
                 std::invalid_argument);

    auto foo1 = std::make_unique<Foo>();
    foo1->a = 1;
    foo1->b = 2;
    foo1->c = 3;
    auto res = objcache->insert("Foo", std::move(foo1));
    EXPECT_EQ(ObjectCache::AllocStatus::kSuccess, res.first);

    auto found1 = objcache->template find<Foo>("Foo");
    ASSERT_NE(nullptr, found1);
    EXPECT_EQ(1, found1->a);
    EXPECT_EQ(2, found1->b);
    EXPECT_EQ(3, found1->c);

    auto foo2 = std::make_unique<Foo>();
    foo2->a = 10;
    foo2->b = 20;
    foo2->c = 30;
    res = objcache->insert("Foo", std::move(foo2));
    EXPECT_EQ(ObjectCache::AllocStatus::kKeyAlreadyExists, res.first);
    EXPECT_EQ(10, res.second->a);

    auto found2 = objcache->template find<Foo>("Foo");
    ASSERT_NE(nullptr, found1);
    EXPECT_EQ(1, found2->a);
    EXPECT_EQ(2, found2->b);
    EXPECT_EQ(3, found2->c);

    EXPECT_TRUE(objcache->remove("Foo"));
    EXPECT_FALSE(objcache->remove("Foo"));
  }

  void testObjectSizeTrackingBasics() {
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000 /* l1EntriesLimit*/,
                          10'000'000 /* totalObjectSizeLimit */,
                          100 /* sizeControllerIntervalMs */)
        .setItemDestructor(
            [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);
    EXPECT_EQ(objcache->getTotalObjectSize(), 0);
    auto foo1 = std::make_unique<Foo>();
    foo1->a = 1;
    foo1->b = 2;
    foo1->c = 3;
    auto foo1Size = 64;
    auto foo2 = std::make_unique<Foo>();
    foo2->a = 10;
    foo2->b = 20;
    foo2->c = 30;
    auto foo2Size = 128;

    // will throw without the object size
    ASSERT_THROW(objcache->insert("Foo", std::make_unique<Foo>()),
                 std::invalid_argument);

    // insert foo1
    {
      auto res = objcache->insert("Foo", std::move(foo1), foo1Size);
      ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, res.first);

      auto found = objcache->template find<Foo>("Foo");
      ASSERT_NE(nullptr, found);
      ASSERT_EQ(1, found->a);
      ASSERT_EQ(2, found->b);
      ASSERT_EQ(3, found->c);
    }
    ASSERT_EQ(objcache->getNumEntries(), 1);
    ASSERT_EQ(objcache->getTotalObjectSize(), foo1Size);

    // replace foo1 with foo2
    {
      auto res = objcache->insertOrReplace("Foo", std::move(foo2), foo2Size);
      ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, std::get<0>(res));

      auto found = objcache->template find<Foo>("Foo");
      ASSERT_NE(nullptr, found);
      ASSERT_EQ(10, found->a);
      ASSERT_EQ(20, found->b);
      ASSERT_EQ(30, found->c);
    }
    ASSERT_EQ(objcache->getNumEntries(), 1);
    ASSERT_EQ(objcache->getTotalObjectSize(), foo2Size);

    // remove foo2
    ASSERT_TRUE(objcache->remove("Foo"));
    ASSERT_EQ(nullptr, objcache->template find<Foo>("Foo"));
    ASSERT_EQ(objcache->getNumEntries(), 0);
    ASSERT_EQ(objcache->getTotalObjectSize(), 0);
  }

  void testObjectSizeTrackingUniqueInsert() {
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000 /* l1EntriesLimit*/,
                          10'000'000 /* totalObjectSizeLimit */,
                          100 /* sizeControllerIntervalMs */)
        .setItemDestructor(
            [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    // will throw without the object size
    ASSERT_THROW(objcache->insert("Foo", std::make_unique<Foo>()),
                 std::invalid_argument);

    auto foo1 = std::make_unique<Foo>();
    foo1->a = 1;
    foo1->b = 2;
    foo1->c = 3;
    auto foo1Size = 64;
    auto res = objcache->insert("Foo", std::move(foo1), foo1Size);
    EXPECT_EQ(ObjectCache::AllocStatus::kSuccess, res.first);
    ASSERT_EQ(objcache->getNumEntries(), 1);
    ASSERT_EQ(objcache->getTotalObjectSize(), foo1Size);

    auto found1 = objcache->template find<Foo>("Foo");
    ASSERT_NE(nullptr, found1);
    EXPECT_EQ(1, found1->a);
    EXPECT_EQ(2, found1->b);
    EXPECT_EQ(3, found1->c);

    auto foo2 = std::make_unique<Foo>();
    foo2->a = 10;
    foo2->b = 20;
    foo2->c = 30;
    auto foo2Size = 128;
    res = objcache->insert("Foo", std::move(foo2), foo2Size);
    EXPECT_EQ(ObjectCache::AllocStatus::kKeyAlreadyExists, res.first);
    ASSERT_EQ(objcache->getNumEntries(), 1);
    ASSERT_EQ(objcache->getTotalObjectSize(), foo1Size);

    auto found2 = objcache->template find<Foo>("Foo");
    ASSERT_NE(nullptr, found2);
    EXPECT_EQ(1, found2->a);
    EXPECT_EQ(2, found2->b);
    EXPECT_EQ(3, found2->c);
  }

  template <typename T>
  void checkObjectSizeTracking(ObjectCache& objcache,
                               const std::shared_ptr<T>& object,
                               std::function<void()> mutateCb) {
    objcache.mutateObject(object, std::move(mutateCb));

    ThreadMemoryTracker tMemTracker;
    auto memUsage1 = tMemTracker.getMemUsageBytes();
    auto objectCopy = std::make_unique<T>(*object);
    folly::doNotOptimizeAway(objectCopy);
    auto memUsage2 = tMemTracker.getMemUsageBytes();

    EXPECT_EQ(memUsage2 - memUsage1, objcache.getObjectSize(object));
  }

  void checkTotalObjectSize(ObjectCache& objcache) {
    size_t totalObjectSize = 0;
    for (auto itr = objcache.getL1Cache().begin();
         itr != objcache.getL1Cache().end();
         ++itr) {
      totalObjectSize +=
          ObjectCache::getAlignedItemPtr(itr.asHandle()->getMemory())
              ->objectSize;
    }
    EXPECT_EQ(totalObjectSize, objcache.getTotalObjectSize());
  }

  void checkObjectSizeTrackingUnorderedMap() {
    using ObjectType = std::unordered_map<std::string, std::string>;
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000 /* l1EntriesLimit*/)
        .setItemDestructor([&](ObjectCacheDestructorData data) {
          data.deleteObject<ObjectType>();
        });
    config.objectSizeTrackingEnabled = true;
    auto objcache = ObjectCache::create(config);

    // create an empty map
    ThreadMemoryTracker tMemTracker;
    auto memUsage1 = tMemTracker.getMemUsageBytes();
    auto map = std::make_unique<ObjectType>();
    auto memUsage2 = tMemTracker.getMemUsageBytes();

    auto [_, ptr, __] = objcache->insertOrReplace("cacheKey", std::move(map),
                                                  memUsage2 - memUsage1);
    EXPECT_EQ(memUsage2 - memUsage1, objcache->getObjectSize(ptr));
    EXPECT_EQ(memUsage2 - memUsage1, objcache->getTotalObjectSize());

    auto found = objcache->template findToWrite<ObjectType>("cacheKey");
    ASSERT_NE(nullptr, found);

    // add an entry
    auto cb1 = [&found]() { (*found)["key"] = "tiny"; };
    // replace the entry with a longer string
    auto cb2 = [&found]() {
      (*found)["key"] = "longgggggggggggggggggggggggggggstringgggggggggggg";
    };
    // replace the entry with a shorter string
    auto cb3 = [&found]() {
      auto tmp = std::make_unique<std::string>("short");
      using std::swap;
      swap((*found)["key"], *tmp);
    };
    // remove the entry
    auto cb4 = [&found]() { found->erase("key"); };

    checkObjectSizeTracking<ObjectType>(*objcache, found, std::move(cb1));
    checkTotalObjectSize(*objcache);

    checkObjectSizeTracking<ObjectType>(*objcache, found, std::move(cb2));
    checkTotalObjectSize(*objcache);

    checkObjectSizeTracking<ObjectType>(*objcache, found, std::move(cb3));
    checkTotalObjectSize(*objcache);

    checkObjectSizeTracking<ObjectType>(*objcache, found, std::move(cb4));
    checkTotalObjectSize(*objcache);
  }

  void checkObjectSizeTrackingVector() {
    using ObjectType = std::vector<Foo>;
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000 /* l1EntriesLimit*/)
        .setItemDestructor([&](ObjectCacheDestructorData data) {
          data.deleteObject<ObjectType>();
        });
    config.objectSizeTrackingEnabled = true;
    auto objcache = ObjectCache::create(config);

    // create an empty vector
    ThreadMemoryTracker tMemTracker;
    auto memUsage1 = tMemTracker.getMemUsageBytes();
    auto vec = std::make_unique<ObjectType>();
    auto memUsage2 = tMemTracker.getMemUsageBytes();

    auto [_, ptr, __] = objcache->insertOrReplace("cacheKey", std::move(vec),
                                                  memUsage2 - memUsage1);
    EXPECT_EQ(memUsage2 - memUsage1, objcache->getObjectSize(ptr));
    EXPECT_EQ(memUsage2 - memUsage1, objcache->getTotalObjectSize());

    auto found = objcache->template findToWrite<ObjectType>("cacheKey");
    ASSERT_NE(nullptr, found);

    // add an entry using emplace_back
    auto cb1 = [&found]() { found->emplace_back(Foo{1, 2, 3}); };

    // add another entry using push_back
    auto cb2 = [&found]() { found->push_back(Foo{4, 5, 6}); };

    // remove the entry from the end using pop_back
    auto cb3 = [&found]() {
      found->pop_back();
      found->shrink_to_fit();
    };

    checkObjectSizeTracking<ObjectType>(*objcache, found, std::move(cb1));
    checkTotalObjectSize(*objcache);

    checkObjectSizeTracking<ObjectType>(*objcache, found, std::move(cb2));
    checkTotalObjectSize(*objcache);

    checkObjectSizeTracking<ObjectType>(*objcache, found, std::move(cb3));
    checkTotalObjectSize(*objcache);
  }

  void checkObjectSizeTrackingString() {
    using ObjectType = std::string;
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000 /* l1EntriesLimit*/)
        .setItemDestructor([&](ObjectCacheDestructorData data) {
          data.deleteObject<ObjectType>();
        });
    config.objectSizeTrackingEnabled = true;
    auto objcache = ObjectCache::create(config);

    // create an empty string
    ThreadMemoryTracker tMemTracker;
    auto memUsage1 = tMemTracker.getMemUsageBytes();
    auto str = std::make_unique<ObjectType>();
    auto memUsage2 = tMemTracker.getMemUsageBytes();

    auto [_, ptr, __] = objcache->insertOrReplace("cacheKey", std::move(str),
                                                  memUsage2 - memUsage1);
    EXPECT_EQ(memUsage2 - memUsage1, objcache->getObjectSize(ptr));
    EXPECT_EQ(memUsage2 - memUsage1, objcache->getTotalObjectSize());

    auto found = objcache->template findToWrite<ObjectType>("cacheKey");
    ASSERT_NE(nullptr, found);

    // set a value
    auto cb1 = [&found]() { *found = "tiny"; };
    // replace the value with a longer string
    auto cb2 = [&found]() {
      *found = "longgggggggggggggggggggggggggggstringgggggggggggg";
    };
    // replace the value with a shorter string
    auto cb3 = [&found]() {
      *found = "short";
      (*found).shrink_to_fit();
    };

    checkObjectSizeTracking<ObjectType>(*objcache, found, std::move(cb1));
    checkTotalObjectSize(*objcache);

    checkObjectSizeTracking<ObjectType>(*objcache, found, std::move(cb2));
    checkTotalObjectSize(*objcache);

    checkObjectSizeTracking<ObjectType>(*objcache, found, std::move(cb3));
    checkTotalObjectSize(*objcache);
  }

  void testObjectSizeTrackingWithMutation() {
    if (!folly::usingJEMalloc()) {
      return;
    }

    checkObjectSizeTrackingUnorderedMap();
    checkObjectSizeTrackingVector();
    checkObjectSizeTrackingString();
  }

  void testObjectSizeTrackingWithSizeUpdate() {
    using ObjectType = std::string;
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000 /* l1EntriesLimit*/)
        .setItemDestructor([&](ObjectCacheDestructorData data) {
          data.deleteObject<ObjectType>();
        });
    config.objectSizeTrackingEnabled = true;
    auto objcache = ObjectCache::create(config);
    size_t curCacheSize = 0;

    for (size_t keySize = 1; keySize < KAllocation::kKeyMaxLenSmall;
         keySize++) {
      std::string key(keySize, 'f');
      auto [_, ptr, __] = objcache->insertOrReplace(
          key, std::make_unique<ObjectType>(), sizeof(ObjectType));
      EXPECT_EQ(sizeof(ObjectType), objcache->getObjectSize(ptr));
      EXPECT_EQ(sizeof(ObjectType) + curCacheSize,
                objcache->getTotalObjectSize());

      auto found = objcache->template findToWrite<ObjectType>(key);
      ASSERT_NE(nullptr, found);

      *found = "longgggggggggggggggggggggggggggstringgggggggggggg";
      const size_t newSize = sizeof(*found) + found->size();
      ASSERT_TRUE(objcache->updateObjectSize(ptr, newSize));

      EXPECT_EQ(newSize, objcache->getObjectSize(ptr));
      EXPECT_EQ(newSize + curCacheSize, objcache->getTotalObjectSize());
      curCacheSize += newSize;
    }

    util::StatsMap stats;
    objcache->getObjectCacheCounters(stats.createCountVisitor());
    EXPECT_GT(stats.getCounts().at("objcache.key_padding_bytes"), 0);
  }

  void testRuntimeTotalObjectSizeLimitVisibility() {
    ObjectCacheConfig config;
    config.setCacheName("runtime_resize_visibility_test");
    config.setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    config.setCacheCapacity(10 /* l1EntriesLimit */,
                            200 /* totalObjectSizeLimit */,
                            60'000 /* sizeControllerIntervalMs */);

    auto objcache = ObjectCache::create(config);
    ASSERT_TRUE(objcache->setTotalObjectSizeLimit(100));

    EXPECT_EQ(objcache->serializeConfigParams().at("totalObjectSizeLimit"),
              "100");

    util::StatsMap stats;
    objcache->getObjectCacheCounters(stats.createCountVisitor());
    EXPECT_EQ(stats.getCounts().at("objcache.total_object_size_limit"), 100);
  }

  void testSerializeConfigMaxKeySize() {
    constexpr uint32_t kMaxKeySize = 1024;
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000 /* l1EntriesLimit */)
        .setMaxKeySizeBytes(kMaxKeySize)
        .setItemDestructor(
            [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    EXPECT_EQ(objcache->serializeConfigParams().at("maxKeySizeBytes"),
              std::to_string(kMaxKeySize));
  }

  void testMultithreadObjectSizeTrackingWithMutation() {
    if (!folly::usingJEMalloc()) {
      return;
    }

    using ObjectType = std::unordered_map<std::string, std::string>;

    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000 /* l1EntriesLimit*/)
        .setItemDestructor([&](ObjectCacheDestructorData data) {
          data.deleteObject<ObjectType>();
        });
    config.objectSizeTrackingEnabled = true;
    auto objcache = ObjectCache::create(config);

    // create an empty map
    ThreadMemoryTracker tMemTracker;
    auto memUsage1 = tMemTracker.getMemUsageBytes();
    auto map = std::make_unique<ObjectType>();
    auto memUsage2 = tMemTracker.getMemUsageBytes();

    objcache->insertOrReplace("cacheKey", std::move(map),
                              memUsage2 - memUsage1);

    auto runMutateObjectOps = [&](int i) {
      auto found = objcache->template findToWrite<ObjectType>("cacheKey");
      ASSERT_NE(nullptr, found);
      objcache->mutateObject(found, [&found, i]() {
        (*found)[fmt::format("key_{}", i)] = fmt::format("value_{}", i);
      });
    };

    std::vector<std::thread> rs;
    rs.reserve(10);
    for (int i = 0; i < 10; i++) {
      rs.emplace_back(runMutateObjectOps, i + 1);
    }
    for (int i = 0; i < 10; i++) {
      rs[i].join();
    }

    auto found = objcache->template find<ObjectType>("cacheKey");
    EXPECT_EQ(objcache->getObjectSize(found), objcache->getTotalObjectSize());
  }

  void testPersistence() {
    auto persistBaseFilePath = std::tmpnam(nullptr);
    std::function<ObjectCacheConfig()> makeBaseConfig = []() {
      ObjectCacheConfig config;

      config.setCacheName("test")
          .setCacheCapacity(10'000 /*l1EntriesLimit*/)
          .setItemDestructor([&](ObjectCacheDestructorData data) {
            data.deleteObject<ThriftFoo>();
          });
      config.objectSizeTrackingEnabled = true;
      return config;
    };
    ThriftFoo foo1;
    foo1.a().value() = 1;
    foo1.b().value() = 2;
    foo1.c().value() = 3;

    ThriftFoo foo2;
    foo2.a().value() = 4;
    foo2.b().value() = 5;
    foo2.c().value() = 6;

    auto objectSize1 = 1000;
    auto objectSize2 = 500;
    auto ttlSecs = 10;

    size_t threadsCount = 10;
    ObjectCacheConfig config = makeBaseConfig();
    config.enablePersistence(
        threadsCount, persistBaseFilePath,
        [&](typename ObjectCache::Serializer serializer) {
          return serializer.template serialize<ThriftFoo>();
        },
        [&](typename ObjectCache::Deserializer deserializer) {
          return deserializer.template deserialize<ThriftFoo>();
        });

    {
      auto objcache = ObjectCache::create(config);

      auto object1 = std::make_unique<ThriftFoo>(foo1);
      auto object2 = std::make_unique<ThriftFoo>(foo2);
      objcache->insertOrReplace("Foo1", std::move(object1), objectSize1,
                                ttlSecs);
      objcache->insertOrReplace("Foo2", std::move(object2), objectSize2);
      ASSERT_EQ(objcache->persist(), true);
    }

    // No objects should expire
    {
      auto objcache = ObjectCache::create(config);
      ASSERT_EQ(objcache->recover(), true);

      auto found1 = objcache->template find<ThriftFoo>("Foo1");
      ASSERT_NE(nullptr, found1);
      EXPECT_EQ(1, found1->a_ref());
      EXPECT_EQ(2, found1->b_ref());
      EXPECT_EQ(3, found1->c_ref());
      auto found2 = objcache->template find<ThriftFoo>("Foo2");
      ASSERT_NE(nullptr, found2);
      EXPECT_EQ(4, found2->a_ref());
      EXPECT_EQ(5, found2->b_ref());
      EXPECT_EQ(6, found2->c_ref());

      EXPECT_EQ(objectSize1 + objectSize2, objcache->getTotalObjectSize());
    }

    // Let Foo1 expire
    std::this_thread::sleep_for(std::chrono::seconds{15});
    {
      auto objcache = ObjectCache::create(config);
      ASSERT_EQ(objcache->recover(), true);

      auto found1 = objcache->template find<ThriftFoo>("Foo1");
      ASSERT_EQ(nullptr, found1);

      auto found2 = objcache->template find<ThriftFoo>("Foo2");
      ASSERT_NE(nullptr, found2);
      EXPECT_EQ(4, found2->a_ref());
      EXPECT_EQ(5, found2->b_ref());
      EXPECT_EQ(6, found2->c_ref());
      EXPECT_EQ(objectSize2, objcache->getTotalObjectSize());
    }

    // test recover failure
    {
      ObjectCacheConfig newConfig = makeBaseConfig();
      newConfig.enablePersistence(
          threadsCount, "random_path",
          [&](typename ObjectCache::Serializer serializer) {
            return serializer.template serialize<ThriftFoo>();
          },
          [&](typename ObjectCache::Deserializer deserializer) {
            return deserializer.template deserialize<ThriftFoo>();
          });
      auto objcache = ObjectCache::create(newConfig);
      ASSERT_EQ(objcache->recover(), false);
    }

    // test different thread count won't fail recover
    {
      ObjectCacheConfig newConfig = makeBaseConfig();
      newConfig.enablePersistence(
          threadsCount - 2, persistBaseFilePath,
          [&](typename ObjectCache::Serializer serializer) {
            return serializer.template serialize<ThriftFoo>();
          },
          [&](typename ObjectCache::Deserializer deserializer) {
            return deserializer.template deserialize<ThriftFoo>();
          });

      auto objcache = ObjectCache::create(newConfig);
      ASSERT_EQ(objcache->recover(), true);
      auto found = objcache->template find<ThriftFoo>("Foo2");
      ASSERT_NE(nullptr, found);
      EXPECT_EQ(4, found->a_ref());
      EXPECT_EQ(5, found->b_ref());
      EXPECT_EQ(6, found->c_ref());
      EXPECT_EQ(objectSize2, objcache->getTotalObjectSize());
    }
  }

  void testPersistenceMultiType() {
    auto persistBaseFilePath = std::tmpnam(nullptr);
    ThriftFoo foo1;
    foo1.a().value() = 1;
    foo1.b().value() = 2;
    foo1.c().value() = 3;

    ThriftFoo2 foo2;
    foo2.d().value() = 4;
    foo2.e().value() = 5;
    foo2.f().value() = 6;

    auto objectSize1 = 1000;
    auto objectSize2 = 500;
    auto ttlSecs = 10;

    size_t threadsCount = 10;

    std::function<ObjectCacheConfig()> makeBaseConfig = []() {
      ObjectCacheConfig config;

      config.setCacheName("test")
          .setCacheCapacity(10'000 /*l1EntriesLimit*/)
          .setItemDestructor([&](ObjectCacheDestructorData data) {
            if (data.key == KAllocation::Key{"Foo1"}) {
              data.deleteObject<ThriftFoo>();
            } else {
              data.deleteObject<ThriftFoo2>();
            }
          });
      config.objectSizeTrackingEnabled = true;
      return config;
    };
    ObjectCacheConfig config = makeBaseConfig();
    config.enablePersistence(
        threadsCount, persistBaseFilePath,
        [&](typename ObjectCache::Serializer serializer) {
          if (serializer.key == KAllocation::Key{"Foo1"}) {
            return serializer.template serialize<ThriftFoo>();
          } else {
            return serializer.template serialize<ThriftFoo2>();
          }
        },
        [&](typename ObjectCache::Deserializer deserializer) {
          if (deserializer.key == KAllocation::Key{"Foo1"}) {
            return deserializer.template deserialize<ThriftFoo>();
          } else {
            return deserializer.template deserialize<ThriftFoo2>();
          }
        });

    {
      auto objcache = ObjectCache::create(config);

      auto object1 = std::make_unique<ThriftFoo>(foo1);
      auto object2 = std::make_unique<ThriftFoo2>(foo2);
      objcache->insertOrReplace("Foo1", std::move(object1), objectSize1,
                                ttlSecs);
      objcache->insertOrReplace("Foo2", std::move(object2), objectSize2);
      ASSERT_EQ(objcache->persist(), true);
    }

    // No objects should expire
    {
      auto objcache = ObjectCache::create(config);
      ASSERT_EQ(objcache->recover(), true);

      auto found1 = objcache->template find<ThriftFoo>("Foo1");
      ASSERT_NE(nullptr, found1);
      EXPECT_EQ(1, found1->a_ref());
      EXPECT_EQ(2, found1->b_ref());
      EXPECT_EQ(3, found1->c_ref());
      auto found2 = objcache->template find<ThriftFoo2>("Foo2");
      ASSERT_NE(nullptr, found2);
      EXPECT_EQ(4, found2->d_ref());
      EXPECT_EQ(5, found2->e_ref());
      EXPECT_EQ(6, found2->f_ref());

      EXPECT_EQ(objectSize1 + objectSize2, objcache->getTotalObjectSize());
    }

    // Let Foo1 expire
    std::this_thread::sleep_for(std::chrono::seconds{15});
    {
      auto objcache = ObjectCache::create(config);
      ASSERT_EQ(objcache->recover(), true);

      auto found1 = objcache->template find<ThriftFoo>("Foo1");
      ASSERT_EQ(nullptr, found1);

      auto found2 = objcache->template find<ThriftFoo2>("Foo2");
      ASSERT_NE(nullptr, found2);
      EXPECT_EQ(4, found2->d_ref());
      EXPECT_EQ(5, found2->e_ref());
      EXPECT_EQ(6, found2->f_ref());
      EXPECT_EQ(objectSize2, objcache->getTotalObjectSize());
    }

    // test recover failure
    {
      ObjectCacheConfig newConfig = makeBaseConfig();
      newConfig.enablePersistence(
          threadsCount, "random_path",
          [&](typename ObjectCache::Serializer serializer) {
            if (serializer.key == KAllocation::Key{"Foo1"}) {
              return serializer.template serialize<ThriftFoo>();
            } else {
              return serializer.template serialize<ThriftFoo2>();
            }
          },
          [&](typename ObjectCache::Deserializer deserializer) {
            if (deserializer.key == KAllocation::Key{"Foo1"}) {
              return deserializer.template deserialize<ThriftFoo>();
            } else {
              return deserializer.template deserialize<ThriftFoo2>();
            }
          });
      auto objcache = ObjectCache::create(newConfig);
      ASSERT_EQ(objcache->recover(), false);
    }
    // test different thread count won't fail recover
    {
      ObjectCacheConfig newConfig = makeBaseConfig();
      newConfig.enablePersistence(
          threadsCount - 2, persistBaseFilePath,
          [&](typename ObjectCache::Serializer serializer) {
            if (serializer.key == KAllocation::Key{"Foo1"}) {
              return serializer.template serialize<ThriftFoo>();
            } else {
              return serializer.template serialize<ThriftFoo2>();
            }
          },
          [&](typename ObjectCache::Deserializer deserializer) {
            if (deserializer.key == KAllocation::Key{"Foo1"}) {
              return deserializer.template deserialize<ThriftFoo>();
            } else {
              return deserializer.template deserialize<ThriftFoo2>();
            }
          });

      auto objcache = ObjectCache::create(newConfig);
      ASSERT_EQ(objcache->recover(), true);
      auto found = objcache->template find<ThriftFoo2>("Foo2");
      ASSERT_NE(nullptr, found);
      EXPECT_EQ(4, found->d_ref());
      EXPECT_EQ(5, found->e_ref());
      EXPECT_EQ(6, found->f_ref());
      EXPECT_EQ(objectSize2, objcache->getTotalObjectSize());
    }
  }

  void testPersistenceHighLoad() {
    ObjectCacheConfig config;
    auto persistBaseFilePath = std::tmpnam(nullptr);
    size_t threadsCount = 10;
    int objectNum = 1000;
    size_t totalObjectSize = 0;

    config.setCacheName("test")
        .setCacheCapacity(10'000 /*l1EntriesLimit*/)
        .setItemDestructor([&](ObjectCacheDestructorData data) {
          data.deleteObject<ThriftFoo>();
        })
        .enablePersistence(
            threadsCount, persistBaseFilePath,
            [&](typename ObjectCache::Serializer serializer) {
              return serializer.template serialize<ThriftFoo>();
            },
            [&](typename ObjectCache::Deserializer deserializer) {
              return deserializer.template deserialize<ThriftFoo>();
            });
    config.objectSizeTrackingEnabled = true;

    {
      auto objcache = ObjectCache::create(config);
      for (int i = 0; i < objectNum; i++) {
        int objectSize = i + 10;
        auto object = std::make_unique<ThriftFoo>();
        object->a().value() = i;
        object->b().value() = i + 1;
        object->c().value() = i + 2;
        objcache->insertOrReplace(fmt::format("key_{}", i), std::move(object),
                                  objectSize);
        totalObjectSize += objectSize;
      }
      ASSERT_EQ(objcache->getNumEntries(), objectNum);
      ASSERT_EQ(objcache->getTotalObjectSize(), totalObjectSize);
      ASSERT_EQ(objcache->persist(), true);
    }

    {
      auto objcache = ObjectCache::create(config);
      ASSERT_EQ(objcache->recover(), true);
      for (int i = 0; i < objectNum; i++) {
        auto found =
            objcache->template find<ThriftFoo>(fmt::format("key_{}", i));
        EXPECT_NE(nullptr, found);
        EXPECT_EQ(i, found->a_ref());
        EXPECT_EQ(i + 1, found->b_ref());
        EXPECT_EQ(i + 2, found->c_ref());
      }
      EXPECT_EQ(objcache->getNumEntries(), objectNum);
      EXPECT_EQ(objcache->getTotalObjectSize(), totalObjectSize);
    }
  }

  void testPersistenceWithEvictionOrder() {
    auto persistBaseFilePath = std::tmpnam(nullptr);
    uint8_t numShards = 3;
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000 /*l1EntriesLimit*/)
        .setNumShards(numShards)
        .setItemDestructor([&](ObjectCacheDestructorData data) {
          data.deleteObject<ThriftFoo>();
        })
        .enablePersistenceWithEvictionOrder(
            persistBaseFilePath,
            [&](typename ObjectCache::Serializer serializer) {
              return serializer.template serialize<ThriftFoo>();
            },
            [&](typename ObjectCache::Deserializer deserializer) {
              return deserializer.template deserialize<ThriftFoo>();
            });

    auto dumpEvictionItr = [](PoolId poolId, ObjectCache& objcache) {
      auto evictItr = objcache.getEvictionIterator(poolId);
      std::vector<std::string> content;
      while (evictItr) {
        auto* itemPtr = ObjectCache::getAlignedItemPtr(evictItr->getMemory());
        auto* objectPtr = reinterpret_cast<ThriftFoo*>(itemPtr->objectPtr);
        content.push_back(fmt::format("{}: a {} b {} c {}",
                                      evictItr->getKey(),
                                      folly::copy(objectPtr->a().value()),
                                      folly::copy(objectPtr->b().value()),
                                      folly::copy(objectPtr->c().value())));
        ++evictItr;
      }
      return content;
    };

    std::vector<std::vector<std::string>> evictionItrDumpBefore;
    {
      auto objcache = ObjectCache::create(config);
      auto poolIds = objcache->l1Cache_->getRegularPoolIds();
      ASSERT_EQ(poolIds.size(), numShards);
      size_t numItems = 2000;
      // Create an unevenly distributed shards
      for (size_t i = 0; i < numItems; i++) {
        auto object = std::make_unique<ThriftFoo>();
        object->a().value() = i;
        object->b().value() = i + 1;
        object->c().value() = i + 2;
        auto key = fmt::format("key_{}", i);
        objcache->insertOrReplace(key, std::move(object));
      }

      // random access to shuffle the items' order
      int objectNum = objcache->getNumEntries();
      for (int i = 0; i < objectNum / 2; i++) {
        auto found = objcache->template find<ThriftFoo>(
            fmt::format("key_{}", folly::Random::rand32(0, objectNum)));
        ASSERT_NE(nullptr, found);
      }

      for (auto poolId : poolIds) {
        evictionItrDumpBefore.emplace_back(dumpEvictionItr(poolId, *objcache));
      }

      ASSERT_EQ(objcache->persist(), true);
    }

    std::vector<std::vector<std::string>> evictionItrDumpAfter;
    {
      auto objcache = ObjectCache::create(config);
      ASSERT_EQ(objcache->recover(), true);
      auto poolIds = objcache->l1Cache_->getRegularPoolIds();
      ASSERT_EQ(poolIds.size(), numShards);
      for (auto poolId : poolIds) {
        evictionItrDumpAfter.emplace_back(dumpEvictionItr(poolId, *objcache));
      }
    }

    EXPECT_EQ(evictionItrDumpAfter, evictionItrDumpBefore);
  }

  void testPersistenceNonThrift() {
    auto persistBaseFilePath = std::tmpnam(nullptr);
    size_t threadsCount = 10;
    int objectNum = 1000;
    size_t totalObjectSize = 0;

    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000 /*l1EntriesLimit*/)
        .setItemDestructor(
            [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); })
        .enablePersistence(
            threadsCount, persistBaseFilePath,
            [&](typename ObjectCache::Serializer serializer) {
              return serializer.template serialize<Foo, ThriftFoo>(
                  [](Foo* foo) -> ThriftFoo {
                    ThriftFoo obj;
                    obj.a() = foo->a;
                    obj.b() = foo->b;
                    obj.c() = foo->c;
                    return obj;
                  });
            },
            [&](typename ObjectCache::Deserializer deserializer) {
              return deserializer.template deserialize<Foo, ThriftFoo>(
                  [](ThriftFoo thriftObj) -> Foo {
                    return Foo{*thriftObj.a(), *thriftObj.b(), *thriftObj.c()};
                  });
            });
    config.objectSizeTrackingEnabled = true;

    {
      auto objcache = ObjectCache::create(config);
      for (int i = 0; i < objectNum; i++) {
        int objectSize = i + 10;
        auto object = std::make_unique<Foo>();
        object->a = i;
        object->b = i + 1;
        object->c = i + 2;
        objcache->insertOrReplace(fmt::format("key_{}", i), std::move(object),
                                  objectSize);
        totalObjectSize += objectSize;
      }
      ASSERT_EQ(objcache->getNumEntries(), objectNum);
      ASSERT_EQ(objcache->getTotalObjectSize(), totalObjectSize);
      ASSERT_EQ(objcache->persist(), true);
    }

    {
      auto objcache = ObjectCache::create(config);
      ASSERT_EQ(objcache->recover(), true);
      for (int i = 0; i < objectNum; i++) {
        auto found = objcache->template find<Foo>(fmt::format("key_{}", i));
        EXPECT_NE(nullptr, found);
        EXPECT_EQ(i, found->a);
        EXPECT_EQ(i + 1, found->b);
        EXPECT_EQ(i + 2, found->c);
      }
      EXPECT_EQ(objcache->getNumEntries(), objectNum);
      EXPECT_EQ(objcache->getTotalObjectSize(), totalObjectSize);
    }
  }

  void testPersistenceUserDefinedCb() {
    auto persistBaseFilePath = std::tmpnam(nullptr);
    size_t threadsCount = 10;
    int objectNum = 1000;
    size_t totalObjectSize = 0;

    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000 /*l1EntriesLimit*/)
        .setItemDestructor([&](ObjectCacheDestructorData data) {
          data.deleteObject<std::string>();
        })
        .enablePersistence(
            threadsCount, persistBaseFilePath,
            [&](typename ObjectCache::Serializer serializer) {
              return serializer.template serialize<std::string>(
                  [](std::string* val) -> std::unique_ptr<folly::IOBuf> {
                    return folly::IOBuf::copyBuffer(val->c_str(), val->size());
                  });
            },
            [&](typename ObjectCache::Deserializer deserializer) {
              return deserializer.template deserialize<std::string>(
                  [](folly::StringPiece payload)
                      -> std::unique_ptr<std::string> {
                    return std::make_unique<std::string>(payload.str());
                  });
            });
    config.objectSizeTrackingEnabled = true;

    {
      auto objcache = ObjectCache::create(config);
      for (int i = 0; i < objectNum; i++) {
        int objectSize = i + 10;
        auto object = std::make_unique<std::string>(fmt::format("value_{}", i));
        objcache->insertOrReplace(fmt::format("key_{}", i), std::move(object),
                                  objectSize);
        totalObjectSize += objectSize;
      }
      ASSERT_EQ(objcache->getNumEntries(), objectNum);
      ASSERT_EQ(objcache->getTotalObjectSize(), totalObjectSize);
      ASSERT_EQ(objcache->persist(), true);
    }

    {
      auto objcache = ObjectCache::create(config);
      ASSERT_EQ(objcache->recover(), true);
      for (int i = 0; i < objectNum; i++) {
        auto found =
            objcache->template find<std::string>(fmt::format("key_{}", i));
        EXPECT_NE(nullptr, found);
        EXPECT_EQ(fmt::format("value_{}", i), *found);
      }
      EXPECT_EQ(objcache->getNumEntries(), objectNum);
      EXPECT_EQ(objcache->getTotalObjectSize(), totalObjectSize);
    }
  }

  void testPersistenceDesrFailure() {
    auto persistBaseFilePath = std::tmpnam(nullptr);
    std::string thriftSerErrKey = "key_ThriftSerErr";
    std::string throwExceptionKey = "key_throwException";
    std::string normalKey = "key_normal";

    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000 /*l1EntriesLimit*/)
        .setItemDestructor([&](ObjectCacheDestructorData data) {
          data.deleteObject<std::string>();
        })
        .enablePersistence(
            1, persistBaseFilePath,
            [&](typename ObjectCache::Serializer serializer) {
              return serializer.template serialize<std::string>(
                  [](std::string* val) -> std::unique_ptr<folly::IOBuf> {
                    return folly::IOBuf::copyBuffer(val->c_str(), val->size());
                  });
            },
            [&](typename ObjectCache::Deserializer deserializer) {
              return deserializer.template deserialize<std::string>(
                  [&](folly::StringPiece payload)
                      -> std::unique_ptr<std::string> {
                    if (deserializer.key == KAllocation::Key{thriftSerErrKey}) {
                      Deserializer deserializer{
                          reinterpret_cast<const uint8_t*>(payload.begin()),
                          reinterpret_cast<const uint8_t*>(payload.end())};
                      return std::make_unique<std::string>(
                          deserializer.deserialize<std::string>());
                    } else if (deserializer.key ==
                               KAllocation::Key{throwExceptionKey}) {
                      throw std::runtime_error("test exception");
                    } else {
                      return std::make_unique<std::string>(payload.str());
                    }
                  });
            });

    {
      auto objcache = ObjectCache::create(config);
      objcache->insertOrReplace(thriftSerErrKey,
                                std::make_unique<std::string>("value_1"));
      objcache->insertOrReplace(throwExceptionKey,
                                std::make_unique<std::string>("value_2"));
      objcache->insertOrReplace(normalKey,
                                std::make_unique<std::string>("value_3"));
      ASSERT_EQ(objcache->persist(), true);
      EXPECT_EQ(objcache->getNumEntries(), 3);
    }

    {
      auto objcache = ObjectCache::create(config);
      ASSERT_EQ(objcache->recover(), true);
      auto found = objcache->template find<std::string>(thriftSerErrKey);
      EXPECT_EQ(nullptr, found);
      found = objcache->template find<std::string>(throwExceptionKey);
      EXPECT_EQ(nullptr, found);
      found = objcache->template find<std::string>(normalKey);
      ASSERT_NE(nullptr, found);
      EXPECT_EQ(*found, "value_3");
      EXPECT_EQ(objcache->getNumEntries(), 1);
    }
  }

  void testGetTtl() {
    const uint32_t ttlSecs = 600;

    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000).setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    auto before = util::getCurrentTimeSec();
    std::this_thread::sleep_for(std::chrono::seconds{3});
    objcache->insertOrReplace("Foo", std::make_unique<Foo>(), 0 /*object size*/,
                              ttlSecs);

    // lookup via find API
    auto found1 = objcache->template find<Foo>("Foo");
    ASSERT_NE(nullptr, found1);

    // get TTL info
    EXPECT_EQ(ttlSecs, objcache->getConfiguredTtl(found1).count());
    EXPECT_LE(before + ttlSecs, objcache->getExpiryTimeSec(found1));

    // lookup via findToWrite API
    auto found2 = objcache->template findToWrite<Foo>("Foo");
    ASSERT_NE(nullptr, found2);

    // get TTL info
    EXPECT_EQ(ttlSecs, objcache->getConfiguredTtl(found2).count());
    EXPECT_LE(before + ttlSecs, objcache->getExpiryTimeSec(found2));
  }

  void testUpdateTtl() {
    const uint32_t ttlSecs = 600;

    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000).setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    auto insertionTime = util::getCurrentTimeSec();
    objcache->insertOrReplace("Foo", std::make_unique<Foo>(), 0 /*object size*/,
                              ttlSecs);

    auto found = objcache->template find<Foo>("Foo");
    ASSERT_NE(nullptr, found);

    // get TTL info
    EXPECT_EQ(ttlSecs, objcache->getConfiguredTtl(found).count());
    EXPECT_LE(insertionTime + ttlSecs, objcache->getExpiryTimeSec(found));

    // update expiry time
    auto currExpTime = objcache->getExpiryTimeSec(found);
    EXPECT_TRUE(objcache->updateExpiryTimeSec(found, currExpTime + ttlSecs));
    EXPECT_EQ(2 * ttlSecs, objcache->getConfiguredTtl(found).count());
    EXPECT_EQ(currExpTime + ttlSecs, objcache->getExpiryTimeSec(found));

    // extend TTL
    auto now = util::getCurrentTimeSec();
    std::this_thread::sleep_for(std::chrono::seconds{3});
    EXPECT_TRUE(objcache->extendTtl(found, std::chrono::seconds(3 * ttlSecs)));
    EXPECT_LE(now + ttlSecs, objcache->getExpiryTimeSec(found));
    EXPECT_LE(3 * ttlSecs, objcache->getConfiguredTtl(found).count());
  }

  void testGetLastAccessTime() {
    typename ObjectCache::EvictionPolicyConfig evictionPolicyConfig;
    evictionPolicyConfig.lruRefreshTime = 0;
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000)
        .setItemDestructor(
            [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); })
        .setEvictionPolicyConfig(evictionPolicyConfig);
    auto objcache = ObjectCache::create(config);
    // insert one object
    auto [_, ptr, __] =
        objcache->insertOrReplace("Foo", std::make_unique<Foo>());

    // insert another object
    objcache->insertOrReplace("Foo2", std::make_unique<Foo>());

    // get last access time of the 1st object
    auto lastAccessTime1 = objcache->getLastAccessTimeSec(ptr);

    std::this_thread::sleep_for(std::chrono::seconds{2});
    {
      // lookup will update the last access time
      auto found = objcache->template find<Foo>("Foo");
    }
    // get last access time of the 1st object again
    auto lastAccessTime2 = objcache->getLastAccessTimeSec(ptr);
    EXPECT_LE(lastAccessTime1, lastAccessTime2 - 2);
  }

  void testGetCreationTime() {
    typename ObjectCache::EvictionPolicyConfig evictionPolicyConfig;
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(10'000)
        .setItemDestructor(
            [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); })
        .setEvictionPolicyConfig(evictionPolicyConfig);
    auto objcache = ObjectCache::create(config);

    // insert one object
    objcache->insertOrReplace("Foo1", std::make_unique<Foo>());

    std::this_thread::sleep_for(std::chrono::seconds{2});
    // insert another object
    objcache->insertOrReplace("Foo2", std::make_unique<Foo>());

    auto found1 = objcache->template find<Foo>("Foo1");
    auto createTime1 = objcache->getCreationTimeSec(found1);

    auto found2 = objcache->template find<Foo>("Foo2");
    auto createTime2 = objcache->getCreationTimeSec(found2);
    EXPECT_LE(createTime1, createTime2 - 2);
  }

  void testMultithreadReplace() {
    // Sanity test to see if insertOrReplace across multiple
    // threads are safe.
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000).setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    auto runReplaceOps = [&] {
      for (int i = 0; i < 2000; i++) {
        // Rotate through 5 different keys
        auto key = fmt::format("key_{}", i % 5);
        auto foo2 = std::make_unique<Foo>();
        objcache->insertOrReplace(key, std::move(foo2));
      }
    };

    std::vector<std::thread> ts;
    ts.reserve(10);
    for (int i = 0; i < 10; i++) {
      ts.emplace_back(runReplaceOps);
    }
    for (int i = 0; i < 10; i++) {
      ts[i].join();
    }
  }

  void testMultithreadEviction() {
    // Sanity test to see if evictions across multiple
    // threads are safe.
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(1000).setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    auto runInsertOps = [&](int id) {
      for (int i = 0; i < 2000; i++) {
        auto key = fmt::format("key_{}_{}", id, i);
        auto foo2 = std::make_unique<Foo>();
        objcache->insertOrReplace(key, std::move(foo2));
      }
    };

    std::vector<std::thread> ts;
    ts.reserve(10);
    for (int i = 0; i < 10; i++) {
      ts.emplace_back(runInsertOps, i);
    }
    for (int i = 0; i < 10; i++) {
      ts[i].join();
    }
  }

  void testMultithreadSizeControl() {
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(200 /* l1EntriesLimit*/,
                          100000 /* totalObjectSizeLimit */,
                          100 /* sizeControllerIntervalMs */)
        .setItemDestructor(
            [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });

    auto objcache = ObjectCache::create(config);

    auto runInsertOps = [&](int id) {
      for (int i = 0; i < 2000; i++) {
        auto key = fmt::format("key_{}_{}", id, i);
        auto foo2 = std::make_unique<Foo>();
        objcache->insertOrReplace(key, std::move(foo2), 1000);
      }
      // give enough time for size controller to process
      std::this_thread::sleep_for(std::chrono::milliseconds{200});
    };

    std::vector<std::thread> ts;
    ts.reserve(10);
    for (int i = 0; i < 10; i++) {
      ts.emplace_back(runInsertOps, i);
    }
    for (int i = 0; i < 10; i++) {
      ts[i].join();
    }

    EXPECT_EQ(objcache->getCurrentEntriesLimit(), 100);
  }

  void testMultithreadFindAndReplace() {
    // Sanity test to see if find and insertions at the same time
    // across mutliple threads are safe.
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000).setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    auto runReplaceOps = [&] {
      for (int i = 0; i < 2000; i++) {
        // Rotate through 5 different keys
        auto key = fmt::format("key_{}", i % 5);
        auto foo2 = std::make_unique<Foo>();
        objcache->insertOrReplace(key, std::move(foo2));
      }
    };

    auto runFindOps = [&] {
      for (int i = 0; i < 2000; i++) {
        // Rotate through 5 different keys
        auto key = fmt::format("key_{}", i % 5);
        auto res = objcache->template find<Foo>(key);
      }
    };

    std::vector<std::thread> rs;
    std::vector<std::thread> fs;
    for (int i = 0; i < 10; i++) {
      rs.push_back(std::thread{runReplaceOps});
      fs.push_back(std::thread{runFindOps});
    }
    for (int i = 0; i < 10; i++) {
      rs[i].join();
      fs[i].join();
    }
  }

  void testMultithreadFindAndEviction() {
    // Sanity test to see if find and evictions across multiple
    // threads are safe.
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(1000).setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    auto runInsertOps = [&](int id) {
      for (int i = 0; i < 2000; i++) {
        auto key = fmt::format("key_{}_{}", id, i);
        auto foo2 = std::make_unique<Foo>();
        objcache->insertOrReplace(key, std::move(foo2));
      }
    };

    auto runFindOps = [&](int id) {
      for (int i = 0; i < 2000; i++) {
        auto key = fmt::format("key_{}_{}", id, i);
        auto res = objcache->template find<Foo>(key);
      }
    };

    std::vector<std::thread> rs;
    std::vector<std::thread> fs;
    for (int i = 0; i < 10; i++) {
      rs.push_back(std::thread{runInsertOps, i});
      fs.push_back(std::thread{runFindOps, i});
    }
    for (int i = 0; i < 10; i++) {
      rs[i].join();
      fs[i].join();
    }
  }

  void testMultithreadFindAndReplaceWith10Shards() {
    // Sanity test to see if find and evictions across multiple
    // threads are safe.
    ObjectCacheConfig config;
    config.setCacheName("test")
        .setCacheCapacity(100'000)
        .setNumShards(10)
        .setItemDestructor(
            [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);

    auto runReplaceOps = [&] {
      for (int i = 0; i < 2000; i++) {
        // Rotate through 5 different keys
        auto key = fmt::format("key_{}", i % 5);
        auto foo2 = std::make_unique<Foo>();
        objcache->insertOrReplace(key, std::move(foo2));
      }
    };

    auto runFindOps = [&] {
      for (int i = 0; i < 2000; i++) {
        // Rotate through 5 different keys
        auto key = fmt::format("key_{}", i % 5);
        auto res = objcache->template find<Foo>(key);
      }
    };

    std::vector<std::thread> rs;
    std::vector<std::thread> fs;
    for (int i = 0; i < 10; i++) {
      rs.push_back(std::thread{runReplaceOps});
      fs.push_back(std::thread{runFindOps});
    }
    for (int i = 0; i < 10; i++) {
      rs[i].join();
      fs[i].join();
    }
  }

  void testMultithreadUpdateTtl() {
    // Sanity test to see if update TTL across multiple
    // threads is safe.
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000).setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    auto objcache = ObjectCache::create(config);
    objcache->insertOrReplace("key", std::make_unique<Foo>(), 0, 60);

    auto runUpdateTtlOps = [&] {
      for (int i = 0; i < 2000; i++) {
        auto found = objcache->template find<Foo>("key");
        auto configuredTtlSecs = objcache->getConfiguredTtl(found).count();
        objcache->extendTtl(found, std::chrono::seconds{configuredTtlSecs});
      }
    };

    std::vector<std::thread> ts;
    ts.reserve(10);
    for (int i = 0; i < 10; i++) {
      ts.emplace_back(runUpdateTtlOps);
    }
    for (int i = 0; i < 10; i++) {
      ts[i].join();
    }
  }

  std::unique_ptr<ObjectCache> makeFooCache() {
    ObjectCacheConfig config;
    config.setCacheName("test").setCacheCapacity(10'000);
    config.setItemDestructor(
        [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
    return ObjectCache::create(config);
  }

  std::map<std::string, Foo> populateFooCache(ObjectCache& objcache, int n) {
    std::map<std::string, Foo> expected;
    for (int i = 0; i < n; i++) {
      auto key = fmt::format("Foo_{}", i);
      auto foo = std::make_unique<Foo>();
      foo->a = i;
      foo->b = i * 2;
      foo->c = i * 3;
      expected[key] = *foo;
      auto [allocRes, _, __] = objcache.insertOrReplace(key, std::move(foo));
      EXPECT_EQ(ObjectCache::AllocStatus::kSuccess, allocRes);
    }
    return expected;
  }

  template <typename Iter>
  void verifyVisitsAllFoos(Iter begin,
                           const Iter& end,
                           const std::map<std::string, Foo>& expected) {
    std::map<std::string, Foo> visited;
    for (auto it = std::move(begin); it != end; ++it) {
      ASSERT_TRUE(static_cast<bool>(it));
      visited[it.getKey().str()] = *it.template getObjectPtrAs<Foo>();
    }
    EXPECT_EQ(expected.size(), visited.size());
    for (const auto& [k, v] : expected) {
      auto vIt = visited.find(k);
      ASSERT_NE(visited.end(), vIt);
      EXPECT_EQ(v.a, vIt->second.a);
      EXPECT_EQ(v.b, vIt->second.b);
      EXPECT_EQ(v.c, vIt->second.c);
    }
  }

  void testAccessIteratorVisitsAll() {
    auto objcache = makeFooCache();
    auto expected = populateFooCache(*objcache, 50);
    verifyVisitsAllFoos(objcache->begin(), objcache->end(), expected);
  }

  void testAccessIteratorEmpty() {
    auto objcache = makeFooCache();
    EXPECT_EQ(objcache->begin(), objcache->end());
  }

  void testLockGroupAccessIteratorVisitsAll() {
    auto objcache = makeFooCache();
    auto expected = populateFooCache(*objcache, 50);
    verifyVisitsAllFoos(objcache->beginLockGroup(), objcache->endLockGroup(),
                        expected);
  }

  void testLockGroupAccessIteratorEmpty() {
    auto objcache = makeFooCache();
    EXPECT_EQ(objcache->beginLockGroup(), objcache->endLockGroup());
  }

  void testLockGroupAccessIteratorFilter() {
    auto objcache = makeFooCache();
    populateFooCache(*objcache, 50);

    auto filter = [](folly::StringPiece key) {
      return key == "Foo_0" || key == "Foo_1" || key == "Foo_2";
    };
    std::set<std::string> filteredVisited;
    for (auto it = objcache->beginLockGroup(filter);
         it != objcache->endLockGroup();
         ++it) {
      filteredVisited.insert(it.getKey().str());
    }
    const std::set<std::string> filteredExpected{"Foo_0", "Foo_1", "Foo_2"};
    EXPECT_EQ(filteredExpected, filteredVisited);
  }

  void testLockGroupAccessIteratorPrefixScan() {
    auto objcache = makeFooCache();
    std::set<std::string> expectedAlpha;
    std::set<std::string> expectedBeta;
    for (int i = 0; i < 500; i++) {
      auto alpha = fmt::format("alpha_{}", i);
      auto beta = fmt::format("beta_{}", i);
      expectedAlpha.insert(alpha);
      expectedBeta.insert(beta);
      auto [r1, _1, __1] =
          objcache->insertOrReplace(alpha, std::make_unique<Foo>());
      auto [r2, _2, __2] =
          objcache->insertOrReplace(beta, std::make_unique<Foo>());
      ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, r1);
      ASSERT_EQ(ObjectCache::AllocStatus::kSuccess, r2);
    }

    auto prefixFilter =
        [prefix = folly::StringPiece{"alpha_"}](folly::StringPiece key) {
          return key.startsWith(prefix);
        };
    std::set<std::string> visited;
    for (auto it = objcache->beginLockGroup(prefixFilter);
         it != objcache->endLockGroup();
         ++it) {
      visited.insert(it.getKey().str());
    }
    EXPECT_EQ(expectedAlpha, visited);
  }
};

using AllocatorTypes = ::testing::Types<LruAllocator,
                                        Lru2QAllocator,
                                        TinyLFUAllocator,
                                        LruAllocatorSpinBuckets>;
TYPED_TEST_CASE(ObjectCacheTest, AllocatorTypes);
TYPED_TEST(ObjectCacheTest, MinCacheSizePercentFreeMemory) {
  this->testMinCacheSizePercent(FreeMemory);
}
TYPED_TEST(ObjectCacheTest, MinCacheSizePercentResidentMemory) {
  this->testMinCacheSizePercent(ResidentMemory);
}
TYPED_TEST(ObjectCacheTest, MinCacheSizePercentFreeMemoryOnly) {
  this->testMinCacheSizePercent(FreeMemoryOnly);
}
TYPED_TEST(ObjectCacheTest, MinCacheSizePercentFullBudgetFreeMemory) {
  this->testMemorySizeControlSteps(FreeMemory, {{0, 100}, {0, 100}}, 100);
}
TYPED_TEST(ObjectCacheTest, MinCacheSizePercentFullBudgetResidentMemory) {
  this->testMemorySizeControlSteps(ResidentMemory,
                                   {{30'000, 100}, {30'000, 100}}, 100);
}
TYPED_TEST(ObjectCacheTest, MinCacheSizePercentFullBudgetFreeMemoryOnly) {
  this->testMemorySizeControlSteps(FreeMemoryOnly, {{0, 100}, {0, 100}}, 100);
}
TYPED_TEST(ObjectCacheTest, FreeMemoryShrinkCounters) {
  this->testFreeMemoryShrinkCounters();
}
TYPED_TEST(ObjectCacheTest, MemorySizeControlFreeShrinkBeyondCache) {
  this->testMemorySizeControlSteps(FreeMemory,
                                   {{9'500, 95}, {0, 1}, {20'200, 3}}, 0);
}
TYPED_TEST(ObjectCacheTest, MemorySizeControlRSSShrinkBeyondCache) {
  this->testMemorySizeControlSteps(ResidentMemory,
                                   {{20'500, 95}, {30'000, 1}, {9'800, 3}}, 0);
}
TYPED_TEST(ObjectCacheTest, MemorySizeControlFreeShrinkWholeCache) {
  this->testMemorySizeControlSteps(FreeMemory, {{0, 1}, {20'200, 3}}, 0);
}
TYPED_TEST(ObjectCacheTest, MemorySizeControlRSSShrinkWholeCache) {
  this->testMemorySizeControlSteps(ResidentMemory, {{30'000, 1}, {9'800, 3}},
                                   0);
}
TYPED_TEST(ObjectCacheTest, MemorySizeControlFreeGrowth) {
  this->testMemorySizeControlSteps(
      FreeMemory, {{9'500, 95}, {20'200, 97}, {15'000, 100}}, 0);
}
TYPED_TEST(ObjectCacheTest, MemorySizeControlRSSGrowth) {
  this->testMemorySizeControlSteps(
      ResidentMemory, {{20'500, 95}, {9'800, 97}, {15'000, 100}}, 0);
}
TYPED_TEST(ObjectCacheTest, GetAllocSize) { this->testGetAllocSize(); }
TYPED_TEST(ObjectCacheTest, ConfigValidation) { this->testConfigValidation(); }
TYPED_TEST(ObjectCacheTest, SetShardName) { this->testSetShardName(); }
TYPED_TEST(ObjectCacheTest, SetEvictionPolicyConfig) {
  if (std::is_same_v<TypeParam, LruAllocator>) {
    this->testSetEvictionPolicyConfig();
  }
}
TYPED_TEST(ObjectCacheTest, Simple) { this->testSimple(); }
TYPED_TEST(ObjectCacheTest, AccessIteratorVisitsAll) {
  this->testAccessIteratorVisitsAll();
}
TYPED_TEST(ObjectCacheTest, AccessIteratorEmpty) {
  this->testAccessIteratorEmpty();
}
TYPED_TEST(ObjectCacheTest, LockGroupAccessIteratorVisitsAll) {
  this->testLockGroupAccessIteratorVisitsAll();
}
TYPED_TEST(ObjectCacheTest, LockGroupAccessIteratorEmpty) {
  this->testLockGroupAccessIteratorEmpty();
}
TYPED_TEST(ObjectCacheTest, LockGroupAccessIteratorFilter) {
  this->testLockGroupAccessIteratorFilter();
}
TYPED_TEST(ObjectCacheTest, LockGroupAccessIteratorPrefixScan) {
  this->testLockGroupAccessIteratorPrefixScan();
}
TYPED_TEST(ObjectCacheTest, MultiType) { this->testMultiType(); }
TYPED_TEST(ObjectCacheTest, testMultiTypePolymorphism) {
  this->testMultiTypePolymorphism();
}
TYPED_TEST(ObjectCacheTest, UserItemDestructor) {
  this->testUserItemDestructor();
}
TYPED_TEST(ObjectCacheTest, Expiration) { this->testExpiration(); }
TYPED_TEST(ObjectCacheTest, InspectCacheIncludingExpiredUntilReaped) {
  this->testInspectCacheIncludingExpiredUntilReaped();
}
TYPED_TEST(ObjectCacheTest, PreRemoveCallbackOrdering) {
  if constexpr (std::is_same_v<TypeParam, LruAllocator>) {
    this->testPreRemoveCallbackOrdering();
  } else {
    GTEST_SKIP() << "eviction ordering is specific to LruAllocator";
  }
}
TYPED_TEST(ObjectCacheTest, ExpirationWithCustomizedReaper) {
  this->testExpirationWithCustomizedReaper();
}
TYPED_TEST(ObjectCacheTest, Replace) { this->testReplace(); }
TYPED_TEST(ObjectCacheTest, UniqueInsert) { this->testUniqueInsert(); }
TYPED_TEST(ObjectCacheTest, ObjectSizeTrackingBasics) {
  this->testObjectSizeTrackingBasics();
}
TYPED_TEST(ObjectCacheTest, ObjectSizeTrackingUniqueInsert) {
  this->testObjectSizeTrackingUniqueInsert();
}
TYPED_TEST(ObjectCacheTest, ObjectSizeTrackingWithMutation) {
  this->testObjectSizeTrackingWithMutation();
}
TYPED_TEST(ObjectCacheTest, ObjectSizeTrackingWithSizeUpdate) {
  this->testObjectSizeTrackingWithSizeUpdate();
}
TYPED_TEST(ObjectCacheTest, RuntimeTotalObjectSizeLimitVisibility) {
  this->testRuntimeTotalObjectSizeLimitVisibility();
}
TYPED_TEST(ObjectCacheTest, SerializeConfigMaxKeySize) {
  this->testSerializeConfigMaxKeySize();
}
TYPED_TEST(ObjectCacheTest, MultithreadObjectSizeTrackingWithMutation) {
  this->testMultithreadObjectSizeTrackingWithMutation();
}

TYPED_TEST(ObjectCacheTest, Persistence) { this->testPersistence(); }
TYPED_TEST(ObjectCacheTest, PersistenceMultiType) {
  this->testPersistenceMultiType();
}
TYPED_TEST(ObjectCacheTest, PersistenceHighLoad) {
  this->testPersistenceHighLoad();
}
TYPED_TEST(ObjectCacheTest, PersistenceWithEvictionOrder) {
  if (!std::is_same_v<TypeParam, TinyLFUAllocator>) {
    this->testPersistenceWithEvictionOrder();
  }
}
TYPED_TEST(ObjectCacheTest, PersistenceNonThrift) {
  this->testPersistenceNonThrift();
}
TYPED_TEST(ObjectCacheTest, PersistenceUserDefinedCb) {
  this->testPersistenceUserDefinedCb();
}
TYPED_TEST(ObjectCacheTest, PersistenceDesrFailure) {
  this->testPersistenceDesrFailure();
}
TYPED_TEST(ObjectCacheTest, GetTtl) { this->testGetTtl(); }
TYPED_TEST(ObjectCacheTest, UpdateTtl) { this->testUpdateTtl(); }
TYPED_TEST(ObjectCacheTest, GetLastAccessTime) {
  this->testGetLastAccessTime();
}
TYPED_TEST(ObjectCacheTest, GetCreationTime) { this->testGetCreationTime(); }

TYPED_TEST(ObjectCacheTest, MultithreadReplace) {
  this->testMultithreadReplace();
}
TYPED_TEST(ObjectCacheTest, MultithreadEviction) {
  this->testMultithreadEviction();
}
TYPED_TEST(ObjectCacheTest, MultithreadSizeControl) {
  this->testMultithreadSizeControl();
}
TYPED_TEST(ObjectCacheTest, MultithreadFindAndReplace) {
  this->testMultithreadFindAndReplace();
}
TYPED_TEST(ObjectCacheTest, MultithreadFindAndEviction) {
  this->testMultithreadFindAndEviction();
}
TYPED_TEST(ObjectCacheTest, MultithreadFindAndReplaceWith10Shards) {
  this->testMultithreadFindAndReplaceWith10Shards();
}
TYPED_TEST(ObjectCacheTest, MultithreadUpdateTtl) {
  this->testMultithreadUpdateTtl();
}

using ObjectCache = ObjectCache<LruAllocator>;
TEST(ObjectCacheTest, LruEviction) {
  ObjectCache::Config config;
  config.setCacheName("test").setCacheCapacity(1024);
  config.setItemDestructor(
      [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
  auto objcache = ObjectCache::create(config);

  for (int i = 0; i < 1025; i++) {
    auto foo = std::make_unique<Foo>();
    foo->a = i;
    auto key = fmt::format("key_{}", i);
    objcache->insertOrReplace(key, std::move(foo));
    auto found = objcache->find<Foo>(key);
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(i, found->a);
  }
  auto found = objcache->find<Foo>("key_0");
  EXPECT_EQ(nullptr, found);
}

TEST(ObjectCacheTest, LruEvictionWithSizeControl) {
  ObjectCache::Config config;
  config.setCacheName("test");
  config.setItemDestructor(
      [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
  config.setCacheCapacity(50 /* l1EntriesLimit*/,
                          100 /* totalObjectSizeLimit */,
                          100 /* sizeControllerIntervalMs */);
  // insert objects with equal size
  {
    auto objcache = ObjectCache::create(config);
    for (size_t i = 0; i < 5; i++) {
      auto key = fmt::format("key_{}", i);
      objcache->insertOrReplace(key, std::make_unique<Foo>(), 25);
    }
    ASSERT_EQ(objcache->getTotalObjectSize(), 125);
    ASSERT_EQ(objcache->getCurrentEntriesLimit(), config.l1EntriesLimit);
    ASSERT_EQ(objcache->getNumEntries(), 5);
    // wait for size controller
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    // key_0 should be evicted
    ASSERT_EQ(objcache->getTotalObjectSize(), 100);
    ASSERT_EQ(objcache->getCurrentEntriesLimit(), 4);
    ASSERT_EQ(objcache->getNumEntries(), 4);
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_0"));
    EXPECT_NE(nullptr, objcache->find<Foo>("key_1"));
    EXPECT_NE(nullptr, objcache->find<Foo>("key_2"));
    EXPECT_NE(nullptr, objcache->find<Foo>("key_3"));
    EXPECT_NE(nullptr, objcache->find<Foo>("key_4"));
  }

  // insert and then access objects from the tail
  {
    auto objcache = ObjectCache::create(config);
    for (size_t i = 0; i < 10; i++) {
      auto key = fmt::format("key_{}", i);
      objcache->insertOrReplace(key, std::make_unique<Foo>(), 25);
    }
    // access key_0 ~ key_3 from the tail
    objcache->find<Foo>("key_0");
    objcache->find<Foo>("key_1");
    objcache->find<Foo>("key_2");
    objcache->find<Foo>("key_3");
    ASSERT_EQ(objcache->getTotalObjectSize(), 250);
    ASSERT_EQ(objcache->getCurrentEntriesLimit(), config.l1EntriesLimit);
    ASSERT_EQ(objcache->getNumEntries(), 10);
    // wait for size controller
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    // key_0 ~ key_3 should be kept
    ASSERT_EQ(objcache->getTotalObjectSize(), 100);
    ASSERT_EQ(objcache->getCurrentEntriesLimit(), 4);
    ASSERT_EQ(objcache->getNumEntries(), 4);
    EXPECT_NE(nullptr, objcache->find<Foo>("key_0"));
    EXPECT_NE(nullptr, objcache->find<Foo>("key_1"));
    EXPECT_NE(nullptr, objcache->find<Foo>("key_2"));
    EXPECT_NE(nullptr, objcache->find<Foo>("key_3"));
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_4"));
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_5"));
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_6"));
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_7"));
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_8"));
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_9"));
  }

  // insert objects with different sizes
  {
    auto objcache = ObjectCache::create(config);
    for (size_t i = 0; i < 10; i++) {
      auto key = fmt::format("key_{}", i);
      objcache->insertOrReplace(key, std::make_unique<Foo>(), 25 + i);
    }
    ASSERT_EQ(objcache->getTotalObjectSize(), 295);
    ASSERT_EQ(objcache->getCurrentEntriesLimit(), config.l1EntriesLimit);
    ASSERT_EQ(objcache->getNumEntries(), 10);
    // wait for size controller
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    // key_0 ~ key_6 should be evicted
    ASSERT_EQ(objcache->getTotalObjectSize(), 99);
    ASSERT_EQ(objcache->getCurrentEntriesLimit(), 3);
    ASSERT_EQ(objcache->getNumEntries(), 3);
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_0"));
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_1"));
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_2"));
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_3"));
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_4"));
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_5"));
    EXPECT_EQ(nullptr, objcache->find<Foo>("key_6"));
    EXPECT_NE(nullptr, objcache->find<Foo>("key_7"));
    EXPECT_NE(nullptr, objcache->find<Foo>("key_8"));
    EXPECT_NE(nullptr, objcache->find<Foo>("key_9"));
  }
}

TEST(ObjectCacheTest, RuntimeTotalObjectSizeLimitResize) {
  constexpr size_t kEntriesLimit = 20;
  constexpr int kSizeControllerIntervalMs = 10;

  ObjectCache::Config config;
  config.setCacheName("runtime_resize_test");
  config.setItemDestructor(
      [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
  config.setCacheCapacity(kEntriesLimit, 200 /* totalObjectSizeLimit */,
                          kSizeControllerIntervalMs);

  auto objcache = ObjectCache::create(config);
  for (size_t i = 0; i < 8; i++) {
    objcache->insertOrReplace(fmt::format("key_{}", i), std::make_unique<Foo>(),
                              25);
  }

  ASSERT_EQ(objcache->getTotalObjectSizeLimit(), 200);
  ASSERT_EQ(objcache->getTotalObjectSize(), 200);

  ASSERT_TRUE(objcache->setTotalObjectSizeLimit(100));
  EXPECT_EQ(objcache->getTotalObjectSizeLimit(), 100);
  ASSERT_TRUE(test_util::eventuallyTrue(
      [&]() {
        return objcache->getTotalObjectSize() <= 100 &&
               objcache->getCurrentEntriesLimit() == 4;
      },
      3 /* timeoutSecs */));
  EXPECT_LE(objcache->getTotalObjectSize(), 100);
  EXPECT_EQ(objcache->getCurrentEntriesLimit(), 4);

  ASSERT_TRUE(objcache->setTotalObjectSizeLimit(200));
  EXPECT_EQ(objcache->getTotalObjectSizeLimit(), 200);
  ASSERT_TRUE(test_util::eventuallyTrue(
      [&]() { return objcache->getCurrentEntriesLimit() > 4; },
      3 /* timeoutSecs */));
  EXPECT_GT(objcache->getCurrentEntriesLimit(), 4);
  EXPECT_LE(objcache->getCurrentEntriesLimit(), kEntriesLimit);

  for (size_t i = 8; i < 12; i++) {
    objcache->insertOrReplace(fmt::format("key_{}", i), std::make_unique<Foo>(),
                              25);
  }
  EXPECT_LE(objcache->getTotalObjectSize(), 200);
}

TEST(ObjectCacheTest, RuntimeTotalObjectSizeLimitRejectsZero) {
  ObjectCache::Config config;
  config.setCacheName("runtime_resize_zero_test");
  config.setItemDestructor(
      [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
  config.setCacheCapacity(10 /* l1EntriesLimit */,
                          100 /* totalObjectSizeLimit */,
                          60'000 /* sizeControllerIntervalMs */);

  auto objcache = ObjectCache::create(config);
  for (size_t i = 0; i < 4; i++) {
    objcache->insertOrReplace(fmt::format("key_{}", i), std::make_unique<Foo>(),
                              25);
  }

  ASSERT_EQ(objcache->getTotalObjectSize(), 100);
  EXPECT_FALSE(objcache->setTotalObjectSizeLimit(0));
  EXPECT_EQ(objcache->getTotalObjectSizeLimit(), 100);
  EXPECT_EQ(objcache->getTotalObjectSize(), 100);
  EXPECT_EQ(objcache->getNumEntries(), 4);
}

TEST(ObjectCacheTest, RuntimeTotalObjectSizeLimitUnsupported) {
  ObjectCache::Config config;
  config.setCacheName("runtime_resize_unsupported_test");
  config.setItemDestructor(
      [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
  config.setCacheCapacity(10);

  auto objcache = ObjectCache::create(config);
  // Entry-count-only ObjectCache instances do not have a size controller, so
  // runtime object-size-limit updates are unsupported and must fail cleanly.
  EXPECT_EQ(objcache->getTotalObjectSizeLimit(), 0);
  EXPECT_FALSE(objcache->setTotalObjectSizeLimit(100));
}

TEST(ObjectCacheTest, ExportStats) {
  std::string cacheName = "service_data_exporter_test";
  ObjectCache::Config config;
  config.l1EntriesLimit = 10;
  config.cacheName = cacheName;
  config.setItemDestructor([&](objcache2::ObjectCacheDestructorData data) {
    data.deleteObject<int>();
  });
  auto cache = ObjectCache::create(std::move(config));

  std::string prefix = "cachelib" + cacheName + ".";

  int intervalNameExists = 0;
  cache->exportStats(prefix, std::chrono::seconds{60},
                     [&intervalNameExists, &prefix](auto name, auto value) {
                       if (name == prefix + "objcache.lookups.60" &&
                           value == 0) {
                         intervalNameExists++;
                       }
                     });
  cache->find<int>("some non-existent key");
  cache->exportStats(
      prefix,
      // We will convert a custom interval to 60 seconds interval. So the
      // one "FIND" will become two operations when averaged out to 60 seocnds.
      std::chrono::seconds{30},
      [&intervalNameExists, &prefix](auto name, auto value) {
        if (name == prefix + "objcache.lookups.60" && value == 2) {
          intervalNameExists++;
        }
      });
  EXPECT_EQ(intervalNameExists, 2);
}

// Test the case when init() throws exception. Make sure the exception is
// correctly thrown without hitting segfault.
TEST(ObjectCacheTest, InitException) {
  ObjectCache::Config config;

  config.setCacheName("test");
  config.setItemDestructor(
      [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
  config.setCacheCapacity(50 /* l1EntriesLimit*/,
                          100 /* totalObjectSizeLimit */,
                          100 /* sizeControllerIntervalMs */);
  ObjectCache::NvmCacheConfig nvmConfig;
  nvmConfig.navyConfig = tests::utils::getNvmTestConfig("/tmp");
  // Make region size too large
  nvmConfig.navyConfig.blockCache().setRegionSize(100 * 1024 * 1024);
  config.enableNvm(nvmConfig);

  EXPECT_THROW(ObjectCache::create(config), std::invalid_argument);
}

TEST(ObjectCacheTest, FreeMemSizeControlTest) {
  // Create two caches A and B and fill it completely.
  // Set free mem function to always return a value between the lower and upper
  // range and one below the lower range. The cache with free memory in range
  // should not change in size whereas the one where we return value lower than
  // the lower limit should lead to a reduced cache size.
  const uint64_t kMB = 1024 * 1024;
  auto upperLimitBytes = 15 * kMB;
  auto lowerLimitBytes = 9 * kMB;
  auto itemSize = 1024;               // 1 KB
  uint64_t maxNumEntries = 1024 * 30; // 30 MB
  uint64_t sizeControlInternalMs = 10;

  ObjectCache::Config configA;
  ObjectCache::Config configB;

  configA.setCacheName("testA");
  configB.setCacheName("testB");

  configA.setItemDestructor([&](ObjectCacheDestructorData data) {
    data.deleteObject<MemoryConsumer>();
  });
  configB.setItemDestructor([&](ObjectCacheDestructorData data) {
    data.deleteObject<MemoryConsumer>();
  });

  configA.setCacheCapacity(maxNumEntries, itemSize * maxNumEntries,
                           sizeControlInternalMs);
  configB.setCacheCapacity(maxNumEntries, itemSize * maxNumEntries,
                           sizeControlInternalMs);

  configA.setObjectSizeControllerMode(ObjCacheSizeControlMode::FreeMemory,
                                      upperLimitBytes, lowerLimitBytes);
  configB.setObjectSizeControllerMode(ObjCacheSizeControlMode::FreeMemory,
                                      upperLimitBytes, lowerLimitBytes);

  configA.memoryMode = FreeMemory;
  configB.memoryMode = FreeMemory;

  configA.setFreeMemCb([]() { return 15 * kMB; });
  configB.setFreeMemCb([]() { return 7 * kMB; });

  auto objcacheA = ObjectCache::create(configA);
  auto objcacheB = ObjectCache::create(configB);

  for (size_t i = 0; i < maxNumEntries; i++) {
    auto key = fmt::format("key_{}", i);
    objcacheA->insertOrReplace(key, std::make_unique<MemoryConsumer>(itemSize),
                               itemSize);
  }
  auto numEntriesA = objcacheA->getCurrentEntriesLimit();
  auto totalSizeA = objcacheA->getTotalObjectSize();

  for (size_t i = 0; i < maxNumEntries; i++) {
    auto key = fmt::format("key_{}", i);
    objcacheB->insertOrReplace(key, std::make_unique<MemoryConsumer>(itemSize),
                               itemSize);
  }
  auto numEntriesB = objcacheB->getCurrentEntriesLimit();
  auto totalSizeB = objcacheB->getTotalObjectSize();

  // No change as our free memory byte is in range.
  EXPECT_EQ(numEntriesA, maxNumEntries);
  EXPECT_EQ(totalSizeA, maxNumEntries * itemSize);
  // Since free memory less than lower limit, objcacheB should be smaller.
  EXPECT_LT(numEntriesB, numEntriesA);
  EXPECT_LT(totalSizeB, totalSizeA);
}

TEST(ObjectCacheTest, RSSSizeControlTest) {
  // Create two caches A and B and fill it completely.
  // Set RSS mem function to always return a value above the higher range
  // and one below the lower range. The cache with RSS memory greater
  // than the upper limit leads to a reduced cache size whereas the cache
  // with RSS memory less than the lower limit will have the cache size
  // unchanged.
  const uint64_t kMB = 1024 * 1024;
  auto upperLimitBytes = 15 * kMB;
  auto lowerLimitBytes = 9 * kMB;
  auto itemSize = 1024;               // 1 KB
  uint64_t maxNumEntries = 1024 * 30; // 30 MB
  uint64_t sizeControlInternalMs = 10;

  ObjectCache::Config configA;
  ObjectCache::Config configB;

  configA.setCacheName("testA");
  configB.setCacheName("testB");

  configA.setItemDestructor([&](ObjectCacheDestructorData data) {
    data.deleteObject<MemoryConsumer>();
  });
  configB.setItemDestructor([&](ObjectCacheDestructorData data) {
    data.deleteObject<MemoryConsumer>();
  });

  configA.setCacheCapacity(maxNumEntries, itemSize * maxNumEntries,
                           sizeControlInternalMs);
  configB.setCacheCapacity(maxNumEntries, itemSize * maxNumEntries,
                           sizeControlInternalMs);

  configA.setObjectSizeControllerMode(ObjCacheSizeControlMode::FreeMemory,
                                      upperLimitBytes, lowerLimitBytes);
  configB.setObjectSizeControllerMode(ObjCacheSizeControlMode::FreeMemory,
                                      upperLimitBytes, lowerLimitBytes);

  configA.memoryMode = ResidentMemory;
  configB.memoryMode = ResidentMemory;

  configA.setRSSMemCb([]() { return 16 * kMB; });
  configB.setRSSMemCb([]() { return 7 * kMB; });

  auto objcacheA = ObjectCache::create(configA);
  auto objcacheB = ObjectCache::create(configB);

  for (size_t i = 0; i < maxNumEntries; i++) {
    auto key = fmt::format("key_{}", i);
    objcacheA->insertOrReplace(key, std::make_unique<MemoryConsumer>(itemSize),
                               itemSize);
  }
  auto numEntriesA = objcacheA->getCurrentEntriesLimit();
  auto totalSizeA = objcacheA->getTotalObjectSize();

  for (size_t i = 0; i < maxNumEntries; i++) {
    auto key = fmt::format("key_{}", i);
    objcacheB->insertOrReplace(key, std::make_unique<MemoryConsumer>(itemSize),
                               itemSize);
  }
  auto numEntriesB = objcacheB->getCurrentEntriesLimit();
  auto totalSizeB = objcacheB->getTotalObjectSize();

  // No change as RSS size is less than the lower limit
  EXPECT_EQ(numEntriesB, maxNumEntries);
  EXPECT_EQ(totalSizeB, maxNumEntries * itemSize);

  // Since RSS size is high, objcacheA should be smaller.
  EXPECT_LT(numEntriesA, numEntriesB);
  EXPECT_LT(totalSizeA, totalSizeB);
}

TEST(ObjectCacheTest, PeekToFindTest) {
  ObjectCache::Config config;
  config.setCacheName("test").setCacheCapacity(128);
  config.setItemDestructor(
      [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });
  config.evictionPolicyConfig.updateOnWrite = true;
  auto objcache = ObjectCache::create(config);

  int seq = 0;
  // Fill the cache
  for (; seq < 128; seq++) {
    auto foo = std::make_unique<Foo>();
    foo->a = seq;
    auto key = fmt::format("key_{}", seq);
    objcache->insertOrReplace(key, std::move(foo));
  }

  // FindToWrite will promote it in the LRU list
  auto found = objcache->findToWrite<Foo>("key_0");
  ASSERT_NE(nullptr, found);
  EXPECT_EQ(0, found->a);
  found.reset();

  // Add one entry and key_0 should be still there
  auto foo = std::make_unique<Foo>();
  foo->a = seq;
  auto key = fmt::format("key_{}", seq++);
  objcache->insertOrReplace(key, std::move(foo));

  auto checkExist = objcache->find<Foo>("key_0");
  ASSERT_NE(nullptr, checkExist);
  EXPECT_EQ(0, checkExist->a);
  checkExist.reset();
  // key_1 was evicted instead
  checkExist = objcache->find<Foo>("key_1");
  ASSERT_EQ(nullptr, checkExist);

  // PeekToWrite will not move it in the LRU list
  found = objcache->peekToWrite<Foo>("key_2");
  ASSERT_NE(nullptr, found);
  EXPECT_EQ(2, found->a);
  found.reset();

  // Add one entry and key_2 should be evicted
  foo = std::make_unique<Foo>();
  foo->a = seq;
  key = fmt::format("key_{}", seq++);
  objcache->insertOrReplace(key, std::move(foo));

  checkExist = objcache->find<Foo>("key_2");
  ASSERT_EQ(nullptr, checkExist);
}

TEST(ObjectCacheTest, AggregatePoolStatsWithTwoShards) {
  ObjectCache::Config config;
  config.setCacheName("test")
      .setCacheCapacity(10'000)
      .setNumShards(2)
      .enableAggregatePoolStats()
      .setItemDestructor(
          [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); });

  auto objcache = ObjectCache::create(config);

  auto poolIds = objcache->getL1Cache().getRegularPoolIds();
  EXPECT_EQ(2, poolIds.size());

  // Insert some items to generate stats
  for (int i = 0; i < 10; i++) {
    auto key = fmt::format("key_{}", i);
    objcache->insertOrReplace(key, std::make_unique<Foo>());
  }

  // Test stats export to see what we actually get
  bool foundAggregatedStats = false;
  bool foundIndividualStats = false;

  objcache->exportStats(
      "test_prefix.", std::chrono::seconds{60},
      [&](folly::StringPiece name, uint64_t value) {
        std::string nameStr = name.str();
        if (nameStr.find("pool.aggregated.") != std::string::npos) {
          foundAggregatedStats = true;
          XLOGF(INFO, "Found aggregated stat: {} = {}", name, value);
        } else if (nameStr.find("pool.pool_0.") != std::string::npos ||
                   nameStr.find("pool.pool_1.") != std::string::npos) {
          foundIndividualStats = true;
          XLOGF(INFO, "Found individual stat: {} = {}", name, value);
        }
      });

  EXPECT_TRUE(foundAggregatedStats)
      << "Should find aggregated stats with enableAggregatePoolStats()";
  EXPECT_FALSE(foundIndividualStats) << "Should NOT find individual shard "
                                        "stats with enableAggregatePoolStats()";
}

TEST(ObjectCacheTest, DynamicFreeMemorySizeControlTest) {
  const uint64_t kMB = 1024 * 1024;
  auto upperLimitBytes = 15 * kMB;
  auto lowerLimitBytes = 9 * kMB;
  auto itemSize = 1024;
  uint64_t maxNumEntries = 1024 * 20;
  int sizeControlIntervalMs = 50;

  ObjectCache::Config config;
  config.setCacheName("dynamic_free_mem_test");
  config.setItemDestructor([&](ObjectCacheDestructorData data) {
    data.deleteObject<MemoryConsumer>();
  });

  config.setCacheCapacity(maxNumEntries, itemSize * maxNumEntries,
                          sizeControlIntervalMs);

  config.setObjectSizeControllerMode(ObjCacheSizeControlMode::FreeMemoryOnly,
                                     upperLimitBytes, lowerLimitBytes);
  config.memoryMode = FreeMemoryOnly;

  static std::atomic<uint64_t> currentFreeMem{20 * kMB};
  config.setFreeMemCb([]() {
    auto val = currentFreeMem.load();
    return val;
  });

  auto objcache = ObjectCache::create(config);

  auto waitForEntriesLimitDecrease = [&](size_t initialLimit,
                                         std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
      if (objcache->getCurrentEntriesLimit() < initialLimit) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  };

  auto waitForEntriesLimitIncrease = [&](size_t initialLimit,
                                         std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
      if (objcache->getCurrentEntriesLimit() > initialLimit) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  };

  // Waits for entries limit to stabilize.
  auto waitForEntriesLimitToSettle = [&]() {
    size_t lastValue = objcache->getCurrentEntriesLimit();
    return test_util::eventuallyTrue(
        [&]() {
          auto currentValue = objcache->getCurrentEntriesLimit();
          if (currentValue != lastValue) {
            lastValue = currentValue;
            return false;
          }
          return true;
        },
        2 /* timeoutSecs */);
  };

  EXPECT_EQ(objcache->getCurrentEntriesLimit(), maxNumEntries);
  EXPECT_EQ(objcache->getNumEntries(), 0);

  for (size_t i = 0; i < maxNumEntries; i++) {
    auto key = fmt::format("key_{}", i);
    objcache->insertOrReplace(key, std::make_unique<MemoryConsumer>(itemSize),
                              itemSize);
  }

  EXPECT_EQ(objcache->getNumEntries(), maxNumEntries);
  EXPECT_GE(objcache->getCurrentEntriesLimit(), maxNumEntries);

  // Test 1: Shrink when free memory is below lower limit
  currentFreeMem.store(5 * kMB);

  EXPECT_TRUE(
      waitForEntriesLimitDecrease(maxNumEntries, std::chrono::seconds(2)));

  auto entriesAfterShrink = objcache->getCurrentEntriesLimit();
  auto numEntriesAfterShrink = objcache->getNumEntries();

  EXPECT_LT(entriesAfterShrink, maxNumEntries);
  EXPECT_LT(numEntriesAfterShrink, maxNumEntries);

  // Test 2: Stable zone - verify cache stops oscillating
  currentFreeMem.store(12 * kMB);
  EXPECT_TRUE(waitForEntriesLimitToSettle());

  // Test 3: Expand when free memory is above upper limit
  currentFreeMem.store(20 * kMB);

  for (size_t i = 0; i < 10; i++) {
    auto key = fmt::format("new_key_{}", i);
    objcache->insertOrReplace(key, std::make_unique<MemoryConsumer>(itemSize),
                              itemSize);
  }

  EXPECT_TRUE(
      waitForEntriesLimitIncrease(entriesAfterShrink, std::chrono::seconds(2)));

  auto entriesAfterExpand = objcache->getCurrentEntriesLimit();

  EXPECT_GT(entriesAfterExpand, entriesAfterShrink);
  EXPECT_LE(entriesAfterExpand, maxNumEntries);
}

TEST(ObjectCacheTest, ExpandNeverExceedsL1EntriesLimit) {
  const uint64_t kMB = 1024 * 1024;
  auto upperLimitBytes = 15 * kMB;
  auto lowerLimitBytes = 9 * kMB;
  auto itemSize = 1024;
  uint64_t maxNumEntries = 1024 * 20;
  int sizeControlIntervalMs = 50;

  ObjectCache::Config config;
  config.setCacheName("expand_ceiling_test");
  config.setItemDestructor([&](ObjectCacheDestructorData data) {
    data.deleteObject<MemoryConsumer>();
  });
  config.setCacheCapacity(maxNumEntries, itemSize * maxNumEntries,
                          sizeControlIntervalMs);
  config.setObjectSizeControllerMode(ObjCacheSizeControlMode::FreeMemoryOnly,
                                     upperLimitBytes, lowerLimitBytes);
  config.memoryMode = FreeMemoryOnly;
  // Large expand/shrink to trigger structural placeholder popping
  config.expandCacheBy = 500;
  config.shrinkCacheBy = 500;

  static std::atomic<uint64_t> sExpandTestFreeMem{20 * kMB};
  config.setFreeMemCb([]() { return sExpandTestFreeMem.load(); });

  auto objcache = ObjectCache::create(config);

  // Fill cache
  for (size_t i = 0; i < maxNumEntries; i++) {
    auto key = fmt::format("key_{}", i);
    objcache->insertOrReplace(key, std::make_unique<MemoryConsumer>(itemSize),
                              itemSize);
  }
  EXPECT_EQ(objcache->getCurrentEntriesLimit(), maxNumEntries);

  // Run 3 shrink->expand cycles
  for (int cycle = 0; cycle < 3; cycle++) {
    // Shrink: set free memory below lower limit
    sExpandTestFreeMem.store(5 * kMB);
    auto shrunk = test_util::eventuallyTrue(
        [&]() { return objcache->getCurrentEntriesLimit() < maxNumEntries; },
        3 /* timeoutSecs */);
    EXPECT_TRUE(shrunk) << "Shrink failed on cycle " << cycle;

    // Expand: set free memory above upper limit
    sExpandTestFreeMem.store(20 * kMB);
    // Wait for expand to settle
    size_t lastLimit = objcache->getCurrentEntriesLimit();
    auto settled = test_util::eventuallyTrue(
        [&]() {
          auto cur = objcache->getCurrentEntriesLimit();
          if (cur != lastLimit) {
            lastLimit = cur;
            return false;
          }
          return true;
        },
        3 /* timeoutSecs */);
    EXPECT_TRUE(settled) << "Expand didn't settle on cycle " << cycle;

    EXPECT_LE(objcache->getCurrentEntriesLimit(), maxNumEntries)
        << "currentEntriesLimit exceeded l1EntriesLimit on cycle " << cycle;
  }
}

TEST(ObjectCacheTest, CalculateCacheCapacity) {
  constexpr uint32_t kMaxKeySize = 255;
  constexpr size_t kNumShards = 4;
  constexpr uint32_t kExpectedAllocSize = 304;
  constexpr size_t kExpectedAllocsPerSlab = Slab::kSize / kExpectedAllocSize;

  ASSERT_EQ(ObjectCache::getL1AllocSize(kMaxKeySize), kExpectedAllocSize);

  // Single-slab boundary: entries == allocsPerSlab * numShards.
  {
    constexpr size_t kEntries = kExpectedAllocsPerSlab * kNumShards;
    auto info =
        ObjectCache::calculateCacheCapacity(kEntries, kNumShards, kMaxKeySize);
    EXPECT_EQ(info.l1AllocSize, kExpectedAllocSize);
    EXPECT_EQ(info.allocsPerSlab, kExpectedAllocsPerSlab);
    EXPECT_EQ(info.allocsPerShard, kExpectedAllocsPerSlab);
    EXPECT_EQ(info.slabsPerShard, 1u);
    EXPECT_EQ(info.perPoolSize, Slab::kSize);
    // totalCacheSize = slabsPerShard * numShards slabs + 1 overhead slab.
    EXPECT_EQ(info.totalCacheSize, (kNumShards + 1) * Slab::kSize);
    EXPECT_EQ(info.maxEntriesForOneSlab, kEntries);
  }

  // One entry past the boundary needs a second slab per shard.
  {
    constexpr size_t kEntries = kExpectedAllocsPerSlab * kNumShards + 1;
    auto info =
        ObjectCache::calculateCacheCapacity(kEntries, kNumShards, kMaxKeySize);
    EXPECT_EQ(info.allocsPerShard, kExpectedAllocsPerSlab + 1);
    EXPECT_EQ(info.slabsPerShard, 2u);
    EXPECT_EQ(info.perPoolSize, 2u * Slab::kSize);
    EXPECT_EQ(info.totalCacheSize, (2 * kNumShards + 1) * Slab::kSize);
    EXPECT_EQ(info.maxEntriesForOneSlab, kExpectedAllocsPerSlab * kNumShards);
  }

  // maxEntriesForOneSlab is the largest l1EntriesLimit producing slabsPerShard
  // == 1; one entry beyond it must require a second slab.
  {
    auto base = ObjectCache::calculateCacheCapacity(
        kExpectedAllocsPerSlab * kNumShards, kNumShards, kMaxKeySize);
    auto info = ObjectCache::calculateCacheCapacity(
        base.maxEntriesForOneSlab + 1, kNumShards, kMaxKeySize);
    EXPECT_EQ(info.slabsPerShard, 2u);
  }

  // Multi-slab case.
  {
    constexpr size_t kEntries = kExpectedAllocsPerSlab * kNumShards * 3;
    auto info =
        ObjectCache::calculateCacheCapacity(kEntries, kNumShards, kMaxKeySize);
    EXPECT_EQ(info.slabsPerShard, 3u);
    EXPECT_EQ(info.perPoolSize, 3u * Slab::kSize);
    EXPECT_EQ(info.totalCacheSize, (3 * kNumShards + 1) * Slab::kSize);
  }
}

TEST(ObjectCacheTest, CalculateCacheCapacityMatchesRealCache) {
  constexpr uint32_t kMaxKeySize = 255;
  constexpr size_t kNumShards = 4;
  const size_t allocsPerSlab =
      Slab::kSize / ObjectCache::getL1AllocSize(kMaxKeySize);
  const size_t kEntries = allocsPerSlab * kNumShards * 3;
  auto info =
      ObjectCache::calculateCacheCapacity(kEntries, kNumShards, kMaxKeySize);

  ObjectCache::Config config;
  config.setCacheName("calculate_capacity_test")
      .setItemDestructor(
          [&](ObjectCacheDestructorData data) { data.deleteObject<Foo>(); })
      .setCacheCapacity(kEntries, /*totalObjectSizeLimit=*/0,
                        /*sizeControllerIntervalMs=*/0)
      .setMaxKeySizeBytes(kMaxKeySize)
      .setNumShards(kNumShards);
  auto cache = ObjectCache::create(config);

  auto& l1 = cache->getL1Cache();
  const auto poolIds = l1.getPoolIds();
  EXPECT_EQ(poolIds.size(), kNumShards);
  for (auto pid : poolIds) {
    EXPECT_EQ(l1.getPoolStats(pid).poolSize, info.perPoolSize);
  }
}

} // namespace facebook::cachelib::objcache2::test
