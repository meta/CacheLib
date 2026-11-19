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

#include <folly/coro/GtestHelpers.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <optional>
#include <vector>

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/common/Time.h"
#include "cachelib/interface/DetachedItem.h"
#include "cachelib/interface/components/tests/CacheComponentFactory.h"
#include "cachelib/interface/tests/Utils.h"

using namespace facebook::cachelib::interface;
using namespace facebook::cachelib::interface::test;

namespace {

constexpr uint32_t kEvictionAllocSize{8 * 1024};
constexpr uint32_t kEvictionValueSize{7 * 1024};

Result<RAMCacheComponent> createSmallRAMCache(
    EvictionCallback evictionCallback,
    facebook::cachelib::LruAllocator::ItemDestructor itemDestructor = {},
    facebook::cachelib::LruAllocator::RemoveCb removeCallback = {}) {
  facebook::cachelib::LruAllocatorConfig config;
  config.setCacheName("RAMCacheComponentEvictionTest");
  config.setCacheSize(2 * facebook::cachelib::Slab::kSize);
  config.defaultPoolRebalanceStrategy = nullptr;
  if (itemDestructor) {
    config.setItemDestructor(std::move(itemDestructor));
  }
  if (removeCallback) {
    config.setRemoveCallback(std::move(removeCallback));
  }

  return RAMCacheComponent::create(
      std::move(config),
      RAMCacheComponent::PoolConfig{
          .name_ = "eviction_pool",
          .size_ = facebook::cachelib::Slab::kSize,
          .allocSizes_ = std::set<uint32_t>{kEvictionAllocSize},
          .mmConfig_ = {},
          .ensureProvisionable_ = true,
      },
      RAMCacheComponent::PersistenceConfig::noPersistenceOrRecovery(),
      RAMCacheComponent::LatencySamplingConfig{},
      std::move(evictionCallback));
}

class RAMCacheComponentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    factory_ = std::make_unique<RAMCacheFactory>();
    cache_ = factory_->create();
    ASSERT_NE(cache_, nullptr) << "Failed to create cache";
  }

  void TearDown() override { EXPECT_OK(cache_->shutdown()); }

  RAMCacheComponent& ramCache() {
    return static_cast<RAMCacheComponent&>(*cache_);
  }

  void checkNoOutstandingRefs(const std::vector<std::string>& keys) {
    auto& allocator = ramCache().get();
    for (const auto& key : keys) {
      auto implHandle = allocator.find(key);
      ASSERT_TRUE(implHandle) << "Item should still be in cache: " << key;
      EXPECT_EQ(implHandle->getRefCount(), 1u)
          << "Leaked refcount for key: " << key;
    }
  }

  std::unique_ptr<CacheComponent> cache_;

 private:
  std::unique_ptr<RAMCacheFactory> factory_;
};

// ============================================================================
// insertOrReplace() Tests
// ============================================================================

CO_TEST_F(RAMCacheComponentTest, InsertOrReplaceAlwaysReturnsReplacedItem) {
  const std::string key = "replace_key";
  const std::string data1 = "original_data";
  const std::string data2 = "replaced_data";
  const uint32_t now = facebook::cachelib::util::getCurrentTimeSec();

  auto handle1 =
      CO_ASSERT_OK(co_await cache_->allocate(key, data1.size(), now, 3600));
  std::memcpy(handle1.mutableData(), data1.c_str(), data1.size());
  auto result1 = CO_ASSERT_OK(
      co_await cache_->insertOrReplace(std::move(handle1).release()));
  EXPECT_FALSE(result1.has_value());

  // RAM cache must always return the replaced item
  auto handle2 =
      CO_ASSERT_OK(co_await cache_->allocate(key, data2.size(), now, 3600));
  std::memcpy(handle2.mutableData(), data2.c_str(), data2.size());
  auto result2 = CO_ASSERT_OK(
      co_await cache_->insertOrReplace(std::move(handle2).release()));
  CO_ASSERT_TRUE(result2.has_value());
  EXPECT_EQ(result2.value()->getKey(), Key{key});
  std::string replacedData(result2.value()->getMemoryAs<const char>(),
                           data1.size());
  EXPECT_EQ(replacedData, data1);
}

CO_TEST_F(RAMCacheComponentTest, FindDescriptorReturnsValue) {
  const std::string key = "descriptor_key";
  const std::string data = "descriptor_value";
  const uint32_t now = facebook::cachelib::util::getCurrentTimeSec();

  auto handle =
      CO_ASSERT_OK(co_await cache_->allocate(key, data.size(), now, 3600));
  std::memcpy(handle.mutableData(), data.data(), data.size());
  EXPECT_OK(co_await cache_->insert(std::move(handle).release()));

  {
    auto descriptor = CO_ASSERT_OK(co_await ramCache().find(key));
    CO_ASSERT_TRUE(descriptor.has_value());
    EXPECT_EQ(descriptor->size(), data.size());
    EXPECT_EQ(std::string(static_cast<const char*>(descriptor->data()),
                          descriptor->size()),
              data);
  }

  checkNoOutstandingRefs({key});
}

// ============================================================================
// iterator() Tests
// ============================================================================

CO_TEST_F(RAMCacheComponentTest, IteratorReleasesRefcounts) {
  const std::vector<std::string> keys = {"iter_ref_1", "iter_ref_2",
                                         "iter_ref_3"};
  const uint32_t now = facebook::cachelib::util::getCurrentTimeSec();

  for (const auto& key : keys) {
    auto handle = CO_ASSERT_OK(co_await cache_->allocate(key, 100, now, 3600));
    EXPECT_OK(co_await cache_->insert(std::move(handle).release()));
  }

  // Iterate and consume all descriptors
  {
    auto gen = cache_->iterator();
    while (auto item = co_await gen.next()) {
      // consume and discard
    }
  }

  checkNoOutstandingRefs(keys);
}

CO_TEST_F(RAMCacheComponentTest, ActiveHandleAccounting) {
  const uint32_t now = facebook::cachelib::util::getCurrentTimeSec();
  auto& allocator = ramCache().get();

  constexpr int kNumItems = 3;
  for (int i = 0; i < kNumItems; ++i) {
    auto key = "handle_count_" + std::to_string(i);
    auto handle = CO_ASSERT_OK(co_await cache_->allocate(key, 100, now, 3600));
    EXPECT_OK(co_await cache_->insert(std::move(handle).release()));
  }

  CO_ASSERT_EQ(allocator.getHandleCountForThread(), 0);

  {
    std::vector<ReadDescriptor> descriptors;
    for (int i = 0; i < kNumItems; ++i) {
      auto key = "handle_count_" + std::to_string(i);
      auto result = CO_ASSERT_OK(co_await cache_->find(key));
      CO_ASSERT_TRUE(result.has_value());
      descriptors.push_back(std::move(result).value());
    }
    EXPECT_EQ(allocator.getHandleCountForThread(), kNumItems);

    for (int i = kNumItems; i > 0; --i) {
      descriptors.pop_back();
      EXPECT_EQ(allocator.getHandleCountForThread(), i - 1);
    }
  }
}

CO_TEST_F(RAMCacheComponentTest, IteratorEarlyTerminationReleasesRefcounts) {
  constexpr int kNumItems = 10;
  std::vector<std::string> keys;
  const uint32_t now = facebook::cachelib::util::getCurrentTimeSec();

  for (int i = 0; i < kNumItems; ++i) {
    auto& key = keys.emplace_back("early_ref_" + std::to_string(i));
    auto handle = CO_ASSERT_OK(co_await cache_->allocate(key, 100, now, 3600));
    EXPECT_OK(co_await cache_->insert(std::move(handle).release()));
  }

  // Iterate but break early after 3 items
  {
    auto gen = cache_->iterator();
    int count = 0;
    while (auto item = co_await gen.next()) {
      if (++count >= 3) {
        break;
      }
    }
  }

  checkNoOutstandingRefs(keys);
}

CO_TEST(RAMCacheComponentEvictionTest, ReportsCapacityEviction) {
  // Capacity eviction is synchronous in this configuration, so the callback
  // and allocation loop run on the test thread.
  std::optional<DetachedItem> evicted;
  auto cache =
      std::make_unique<RAMCacheComponent>(ASSERT_OK(createSmallRAMCache(
          [&](const CacheItem& item) { evicted.emplace(item); })));

  const auto creationTime = facebook::cachelib::util::getCurrentTimeSec();
  constexpr uint32_t kTtlSecs{3600};
  const std::string value(kEvictionValueSize, 'x');
  const auto maxItems =
      facebook::cachelib::Slab::kSize / kEvictionAllocSize + 2;
  for (size_t i = 0; !evicted.has_value() && i < maxItems; ++i) {
    auto key = "eviction_key_" + std::to_string(i);
    auto allocated = CO_ASSERT_OK(
        co_await cache->allocate(key, value.size(), creationTime, kTtlSecs));
    std::memcpy(allocated.mutableData(), value.data(), value.size());
    CO_ASSERT_OK(co_await cache->insert(std::move(allocated).release()));
  }

  CO_ASSERT_TRUE(evicted.has_value());
  EXPECT_EQ(evicted->getCreationTime(), creationTime);
  EXPECT_EQ(evicted->getExpiryTime(), creationTime + kTtlSecs);
  EXPECT_EQ(evicted->getMemorySize(), value.size());
  EXPECT_EQ(std::string(static_cast<const char*>(evicted->getMemory()),
                        evicted->getMemorySize()),
            value);
  EXPECT_OK(cache->shutdown());
}

CO_TEST(RAMCacheComponentEvictionTest, IgnoresExpiredCapacityEviction) {
  std::atomic<bool> expiredItemEvicted{false};
  std::atomic<bool> expiredItemReported{false};
  auto cache =
      std::make_unique<RAMCacheComponent>(ASSERT_OK(createSmallRAMCache(
          [&expiredItemReported](const CacheItem& item) {
            if (item.getKey() == Key{"expired"}) {
              expiredItemReported = true;
            }
          },
          [&expiredItemEvicted](
              const facebook::cachelib::LruAllocator::DestructorData& data) {
            if (data.context ==
                    facebook::cachelib::DestructorContext::kEvictedFromRAM &&
                data.item.getKey() == Key{"expired"}) {
              expiredItemEvicted = true;
            }
          })));

  const auto now = facebook::cachelib::util::getCurrentTimeSec();
  const std::string value(kEvictionValueSize, 'x');
  auto expired = CO_ASSERT_OK(
      co_await cache->allocate("expired", value.size(), now - 2, 1));
  CO_ASSERT_OK(co_await cache->insert(std::move(expired).release()));

  const auto maxItems =
      facebook::cachelib::Slab::kSize / kEvictionAllocSize + 2;
  for (size_t i = 0; !expiredItemEvicted.load() && i < maxItems; ++i) {
    auto allocated = CO_ASSERT_OK(co_await cache->allocate(
        "live_" + std::to_string(i), value.size(), now, 3600));
    CO_ASSERT_OK(co_await cache->insert(std::move(allocated).release()));
  }

  CO_ASSERT_TRUE(expiredItemEvicted.load());
  EXPECT_FALSE(expiredItemReported.load());
  EXPECT_OK(cache->shutdown());
}

CO_TEST(RAMCacheComponentEvictionTest, IgnoresExplicitRemoval) {
  std::atomic<size_t> evictionCount{0};
  std::atomic<size_t> destructorCount{0};
  auto cache =
      std::make_unique<RAMCacheComponent>(ASSERT_OK(createSmallRAMCache(
          [&evictionCount](const CacheItem&) { ++evictionCount; },
          [&destructorCount](const auto&) { ++destructorCount; })));

  auto allocated = CO_ASSERT_OK(co_await cache->allocate(
      "removed", 16, facebook::cachelib::util::getCurrentTimeSec(), 0));
  CO_ASSERT_OK(co_await cache->insert(std::move(allocated).release()));
  CO_ASSERT_TRUE(CO_ASSERT_OK(co_await cache->remove("removed")));

  EXPECT_EQ(evictionCount.load(), 0);
  EXPECT_EQ(destructorCount.load(), 1);
  EXPECT_OK(cache->shutdown());
}

TEST(RAMCacheComponentEvictionTest, RejectsAllocatorRemoveCallback) {
  EXPECT_ERROR(
      createSmallRAMCache(
          [](const CacheItem&) {}, {},
          [](const facebook::cachelib::LruAllocator::RemoveCbData&) {}),
      Error::Code::INVALID_CONFIG);
}

} // namespace
