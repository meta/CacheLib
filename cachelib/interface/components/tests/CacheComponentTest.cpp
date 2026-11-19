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
#include <folly/coro/BlockingWait.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/testing/TestUtil.h>
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "cachelib/common/Time.h"
#include "cachelib/interface/components/tests/CacheComponentFactory.h"
#include "cachelib/interface/tests/Utils.h"

using namespace ::testing;
using namespace facebook::cachelib;
using namespace facebook::cachelib::util;
using namespace facebook::cachelib::interface;
using namespace facebook::cachelib::interface::test;

namespace {

template <typename FactoryType>
class CacheComponentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    factory_ = std::make_unique<FactoryType>();
    cache_ = factory_->create();
    ASSERT_NE(cache_, nullptr) << "Failed to create cache";
  }

  void TearDown() override { EXPECT_OK(cache_->shutdown()); }

  std::unique_ptr<CacheComponent> cache_;
  std::unique_ptr<FactoryType> factory_;
};

// Define the list of factory types to test
using FactoryTypes = ::testing::
    Types<RAMCacheFactory, FlashCacheFactory, ConsistentFlashCacheFactory>;
TYPED_TEST_SUITE(CacheComponentTest, FactoryTypes);

// ============================================================================
// getName() Tests
// ============================================================================

TYPED_TEST(CacheComponentTest, GetName) {
  const std::string& name = this->cache_->getName();
  EXPECT_EQ(name, "CacheComponentTest");
}

// ============================================================================
// allocate() Tests
// ============================================================================

CO_TYPED_TEST(CacheComponentTest, AllocateBasic) {
  const std::string key = "test_allocate";
  const uint32_t size = 100;
  const uint32_t creationTime = util::getCurrentTimeSec();
  const uint32_t ttlSecs = 3600;

  auto descriptor = CO_ASSERT_OK(
      co_await this->cache_->allocate(key, size, creationTime, ttlSecs));

  EXPECT_EQ(descriptor.capacity(), size);
  EXPECT_EQ(descriptor.key(), Key{key});
  EXPECT_EQ(descriptor.creationTime(), creationTime);
  EXPECT_EQ(descriptor.expiryTime(), creationTime + ttlSecs);
}

CO_TYPED_TEST(CacheComponentTest, AllocateVariousSizes) {
  const std::vector<uint32_t> sizes = {1, 10, 50, 100, 200, 500};
  const uint32_t creationTime = util::getCurrentTimeSec();
  const uint32_t ttlSecs = 3600;

  for (uint32_t size : sizes) {
    const std::string key = "size_test_" + std::to_string(size);
    auto descriptor = CO_ASSERT_OK(
        co_await this->cache_->allocate(key, size, creationTime, ttlSecs));
    EXPECT_EQ(descriptor.capacity(), size);
  }
}

CO_TYPED_TEST(CacheComponentTest, AllocateZeroTTL) {
  const std::string key = "zero_ttl";
  const uint32_t size = 100;
  const uint32_t creationTime = util::getCurrentTimeSec();
  const uint32_t ttlSecs = 0;

  auto descriptor = CO_ASSERT_OK(
      co_await this->cache_->allocate(key, size, creationTime, ttlSecs));
  // zero TTL = infinite TTL
  EXPECT_EQ(descriptor.expiryTime(), 0);
}

CO_TYPED_TEST(CacheComponentTest, AllocateEmptyKey) {
  EXPECT_ERROR(
      co_await this->cache_->allocate("", 100, util::getCurrentTimeSec(), 3600),
      Error::Code::INVALID_ARGUMENTS);
}

// ============================================================================
// insert() Tests
// ============================================================================

CO_TYPED_TEST(CacheComponentTest, InsertBasic) {
  const std::string key = "insert_basic";
  auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, 100, util::getCurrentTimeSec(), 3600));
  EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));
}

CO_TYPED_TEST(CacheComponentTest, InsertWithData) {
  const std::string key = "insert_data";
  const std::string data = "test_data_content";

  auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, data.size(), util::getCurrentTimeSec(), 3600));
  std::memcpy(handle.mutableData(), data.c_str(), data.size());
  EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(findResult.has_value());

  std::string retrievedData(static_cast<const char*>(findResult->data()),
                            findResult->size());
  EXPECT_EQ(retrievedData, data);
}

CO_TYPED_TEST(CacheComponentTest, InsertDuplicateKey) {
  const std::string key = "duplicate";

  auto handle1 = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, 100, util::getCurrentTimeSec(), 3600));
  EXPECT_OK(co_await this->cache_->insert(std::move(handle1).release()));

  auto handle2 = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, 100, util::getCurrentTimeSec(), 3600));
  EXPECT_ERROR(co_await this->cache_->insert(std::move(handle2).release()),
               Error::Code::ALREADY_INSERTED);
}

// ============================================================================
// insertOrReplace() Tests
// ============================================================================

CO_TYPED_TEST(CacheComponentTest, InsertOrReplaceNewItem) {
  const std::string key = "replace_new";
  auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, 100, util::getCurrentTimeSec(), 3600));
  auto result = CO_ASSERT_OK(
      co_await this->cache_->insertOrReplace(std::move(handle).release()));

  EXPECT_FALSE(result.has_value());
}

CO_TYPED_TEST(CacheComponentTest, InsertOrReplaceExistingItem) {
  const std::string key = "replace_existing";
  const std::string data1 = "original_data";
  const std::string data2 = "replaced_data";

  auto handle1 = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, data1.size(), util::getCurrentTimeSec(), 3600));
  std::memcpy(handle1.mutableData(), data1.c_str(), data1.size());
  auto result1 = CO_ASSERT_OK(
      co_await this->cache_->insertOrReplace(std::move(handle1).release()));
  EXPECT_FALSE(result1.has_value());

  auto handle2 = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, data2.size(), util::getCurrentTimeSec(), 3600));
  std::memcpy(handle2.mutableData(), data2.c_str(), data2.size());
  auto result2 = CO_ASSERT_OK(
      co_await this->cache_->insertOrReplace(std::move(handle2).release()));

  // Some implementations may not return the old data -- and that's ok!
  if (result2.has_value()) {
    auto& replacedHandle = result2.value();
    EXPECT_EQ(replacedHandle->getKey(), Key{key});

    std::string replacedData(replacedHandle->template getMemoryAs<const char>(),
                             data1.size());
    EXPECT_EQ(replacedData, data1);
  }

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(findResult.has_value());
  std::string currentData(static_cast<const char*>(findResult->data()),
                          findResult->size());
  EXPECT_EQ(currentData, data2);
}

CO_TYPED_TEST(CacheComponentTest, InsertOrReplaceMultipleTimes) {
  const std::string key = "replace_multiple";

  for (int i = 0; i < 5; ++i) {
    auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
        key, 100, util::getCurrentTimeSec(), 3600));
    *static_cast<uint32_t*>(handle.mutableData()) = i;
    auto result = CO_ASSERT_OK(
        co_await this->cache_->insertOrReplace(std::move(handle).release()));

    if (i == 0) {
      EXPECT_FALSE(result.has_value());
    } else if (result.has_value()) {
      EXPECT_EQ(*result.value()->template getMemoryAs<uint32_t>(), i - 1);
    }
  }
}

CO_TYPED_TEST(CacheComponentTest, InsertOrReplaceDifferentSizes) {
  const std::string key = "replace_sizes";
  const std::string smallData = "small";
  const std::string largeData = "this_is_a_much_larger_data_string";

  // Insert small item first
  auto handle1 = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, smallData.size(), util::getCurrentTimeSec(), 3600));
  std::memcpy(handle1.mutableData(), smallData.c_str(), smallData.size());
  auto result1 = CO_ASSERT_OK(
      co_await this->cache_->insertOrReplace(std::move(handle1).release()));
  EXPECT_FALSE(result1.has_value());

  // Replace with larger item
  auto handle2 = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, largeData.size(), util::getCurrentTimeSec(), 3600));
  std::memcpy(handle2.mutableData(), largeData.c_str(), largeData.size());
  auto result2 = CO_ASSERT_OK(
      co_await this->cache_->insertOrReplace(std::move(handle2).release()));

  if (result2.has_value()) {
    auto& replacedSmallHandle = result2.value();
    EXPECT_EQ(replacedSmallHandle->getKey(), Key{key});
    EXPECT_GE(replacedSmallHandle->getMemorySize(), smallData.size());
    std::string retrievedSmall(
        replacedSmallHandle->template getMemoryAs<const char>(),
        smallData.size());
    EXPECT_EQ(retrievedSmall, smallData);
  }

  // Verify large data is now in cache
  auto findResult1 = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(findResult1.has_value());
  EXPECT_GE(findResult1->size(), largeData.size());
  std::string retrievedLarge1(static_cast<const char*>(findResult1->data()),
                              largeData.size());
  EXPECT_EQ(retrievedLarge1, largeData);

  // Replace large item with smaller item
  auto handle3 = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, smallData.size(), util::getCurrentTimeSec(), 3600));
  std::memcpy(handle3.mutableData(), smallData.c_str(), smallData.size());
  auto result3 = CO_ASSERT_OK(
      co_await this->cache_->insertOrReplace(std::move(handle3).release()));

  if (result3.has_value()) {
    auto& replacedLargeHandle = result3.value();
    EXPECT_EQ(replacedLargeHandle->getKey(), Key{key});
    EXPECT_GE(replacedLargeHandle->getMemorySize(), largeData.size());
    std::string retrievedLarge2(
        replacedLargeHandle->template getMemoryAs<const char>(),
        largeData.size());
    EXPECT_EQ(retrievedLarge2, largeData);
  }

  // Verify small data is now in cache
  auto findResult2 = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(findResult2.has_value());
  EXPECT_GE(findResult2->size(), smallData.size());
  std::string retrievedSmall2(static_cast<const char*>(findResult2->data()),
                              smallData.size());
  EXPECT_EQ(retrievedSmall2, smallData);
}

// ============================================================================
// find() Tests
// ============================================================================

CO_TYPED_TEST(CacheComponentTest, FindExistingItem) {
  const std::string key = "find_existing";
  const std::string data = "find_test_data";

  auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, data.size(), util::getCurrentTimeSec(), 3600));
  std::memcpy(handle.mutableData(), data.c_str(), data.size());
  EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(findResult.has_value());

  auto& descriptor = findResult.value();
  EXPECT_EQ(descriptor.key(), Key{key});

  std::string retrievedData(static_cast<const char*>(descriptor.data()),
                            descriptor.size());
  EXPECT_EQ(retrievedData, data);
}

CO_TYPED_TEST(CacheComponentTest, FindNonExistentItem) {
  auto findResult = CO_ASSERT_OK(co_await this->cache_->find("nonexistent"));
  EXPECT_FALSE(findResult.has_value());
}

CO_TYPED_TEST(CacheComponentTest, FindMultipleItems) {
  const int numItems = 10;
  std::vector<std::string> keys;
  keys.reserve(numItems);

  for (int i = 0; i < numItems; ++i) {
    auto& key = keys.emplace_back("multi_" + std::to_string(i));
    auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
        key, 100, util::getCurrentTimeSec(), 3600));
    EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));
  }

  for (const auto& key : keys) {
    auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
    CO_ASSERT_TRUE(findResult.has_value());
    auto foundHandle = std::move(findResult.value()).release();
    EXPECT_EQ(foundHandle->getKey(), Key{key});
  }
}

CO_TYPED_TEST(CacheComponentTest, FindExpiredItem) {
  const std::string key = "expired";
  const uint32_t creationTime = util::getCurrentTimeSec() - 7200;
  const uint32_t ttlSecs = 3600;

  auto handle = CO_ASSERT_OK(
      co_await this->cache_->allocate(key, 100, creationTime, ttlSecs));
  EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  EXPECT_FALSE(findResult.has_value());
}

CO_TYPED_TEST(CacheComponentTest, FindPropertiesMatch) {
  const std::string key = "props";
  const uint32_t size = 200;
  const uint32_t creationTime = util::getCurrentTimeSec();
  const uint32_t ttlSecs = 7200;

  auto handle = CO_ASSERT_OK(
      co_await this->cache_->allocate(key, size, creationTime, ttlSecs));
  EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(findResult.has_value());

  auto foundHandle = std::move(findResult.value()).release();
  EXPECT_EQ(foundHandle->getKey(), Key{key});
  EXPECT_GE(foundHandle->getMemorySize(), size);
  EXPECT_EQ(foundHandle->getCreationTime(), creationTime);
  EXPECT_EQ(foundHandle->getExpiryTime(), creationTime + ttlSecs);
}

// ============================================================================
// findToWrite() Tests
// ============================================================================

CO_TYPED_TEST(CacheComponentTest, FindToWriteExistingItem) {
  const std::string key = "write_existing";
  const std::string originalData = "original";

  auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, 100, util::getCurrentTimeSec(), 3600));
  std::memcpy(handle.mutableData(), originalData.c_str(), originalData.size());
  EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));

  auto writeResult = CO_ASSERT_OK(co_await this->cache_->findToWrite(key));
  CO_ASSERT_TRUE(writeResult.has_value());

  auto& descriptor = writeResult.value();
  EXPECT_EQ(descriptor.key(), Key{key});
  CO_ASSERT_GE(descriptor.size(), originalData.size());
  EXPECT_EQ(
      std::memcmp(descriptor.data(), originalData.data(), originalData.size()),
      0);
}

CO_TYPED_TEST(CacheComponentTest, FindToWriteNonExistentItem) {
  auto writeResult =
      CO_ASSERT_OK(co_await this->cache_->findToWrite("nonexistent_write"));
  EXPECT_FALSE(writeResult.has_value());
}

CO_TYPED_TEST(CacheComponentTest, FindToWriteAndModify) {
  const std::string key = "modify";
  const std::string originalData = "original";
  const std::string modifiedData = "modified";

  auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, std::max(originalData.size(), modifiedData.size()),
      util::getCurrentTimeSec(), 3600));
  std::memcpy(handle.mutableData(), originalData.c_str(), originalData.size());
  EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));

  {
    auto writeResult = CO_ASSERT_OK(co_await this->cache_->findToWrite(key));
    CO_ASSERT_TRUE(writeResult.has_value());
    auto& descriptor = writeResult.value();
    CO_ASSERT_GE(descriptor.capacity(), modifiedData.size());
    std::memcpy(descriptor.mutableData(), modifiedData.c_str(),
                modifiedData.size());
  }

  auto readResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(readResult.has_value());
  std::string retrievedData(static_cast<const char*>(readResult->data()),
                            readResult->size());
  EXPECT_EQ(retrievedData, modifiedData);
}

CO_TYPED_TEST(CacheComponentTest, FindToWriteExpiredItem) {
  const std::string key = "expired_write";
  const uint32_t creationTime = util::getCurrentTimeSec() - 7200;
  const uint32_t ttlSecs = 3600;

  auto handle = CO_ASSERT_OK(
      co_await this->cache_->allocate(key, 100, creationTime, ttlSecs));
  EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));

  auto writeResult = CO_ASSERT_OK(co_await this->cache_->findToWrite(key));
  EXPECT_FALSE(writeResult.has_value());
}

// ============================================================================
// iterator() Tests
// ============================================================================

CO_TYPED_TEST(CacheComponentTest, IteratorEmptyCache) {
  auto iterator = this->cache_->iterator();
  while (auto item = co_await iterator.next()) {
    CO_FAIL() << "shouldn't return any items with empty cache";
  }
}

CO_TYPED_TEST(CacheComponentTest, IteratorBasic) {
  const std::vector<std::pair<std::string, std::string>> items = {
      {"iter_key1", "data_for_key1"},
      {"iter_key2", "data_for_key2"},
      {"iter_key3", "data_for_key3"},
  };

  for (const auto& item : items) {
    auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
        item.first, item.second.size(), util::getCurrentTimeSec(), 3600));
    std::memcpy(handle.mutableData(), item.second.c_str(), item.second.size());
    EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));
  }

  std::unordered_map<std::string, std::string> foundItems;
  auto iterator = this->cache_->iterator();
  while (auto item = co_await iterator.next()) {
    CO_ASSERT_TRUE(item.has_value());
    const auto& descriptor = item.value();
    const auto key = descriptor.key();
    foundItems[std::string(key.data(), key.size())] = std::string(
        static_cast<const char*>(descriptor.data()), descriptor.size());
  }

  EXPECT_EQ(foundItems.size(), items.size());
  for (const auto& item : items) {
    auto it = foundItems.find(item.first);
    CO_ASSERT_NE(it, foundItems.end())
        << "Key not found during iteration: " << item.first;
    // Data may be in a larger buffer, so check prefix matches
    auto substr = it->second.substr(0, item.second.size());
    EXPECT_EQ(substr, item.second)
        << "Data mismatch for key: " << item.first << ", found " << substr
        << " but expected " << item.second;
  }
}

CO_TYPED_TEST(CacheComponentTest, IteratorEarlyTermination) {
  const int numItems = 10;
  for (int i = 0; i < numItems; ++i) {
    auto key = "early_term_" + std::to_string(i);
    auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
        key, 100, util::getCurrentTimeSec(), 3600));
    EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));
  }

  auto iterator = this->cache_->iterator();
  int count = 0;
  while (auto item = co_await iterator.next()) {
    if (++count >= 3) {
      break;
    }
  }

  for (int i = 0; i < numItems; ++i) {
    auto key = "early_term_" + std::to_string(i);
    auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
    EXPECT_TRUE(findResult.has_value())
        << "Iterated item should still be reachable: " << key;
  }
}

CO_TYPED_TEST(CacheComponentTest, IteratorAfterRemovals) {
  const std::vector<std::string> allKeys = {"remain1", "remove1", "remain2",
                                            "remove2", "remain3"};
  for (const auto& key : allKeys) {
    auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
        key, 100, util::getCurrentTimeSec(), 3600));
    EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));
  }

  EXPECT_TRUE(CO_ASSERT_OK(co_await this->cache_->remove("remove1")));
  EXPECT_TRUE(CO_ASSERT_OK(co_await this->cache_->remove("remove2")));

  auto iterator = this->cache_->iterator();
  std::unordered_set<std::string> foundKeys;
  while (auto item = co_await iterator.next()) {
    const auto key = item.value().key();
    foundKeys.insert(std::string(key.data(), key.size()));
  }

  EXPECT_EQ(foundKeys.size(), 3);
  EXPECT_TRUE(foundKeys.count("remain1"));
  EXPECT_TRUE(foundKeys.count("remain2"));
  EXPECT_TRUE(foundKeys.count("remain3"));
  EXPECT_FALSE(foundKeys.count("remove1"));
  EXPECT_FALSE(foundKeys.count("remove2"));
}

CO_TYPED_TEST(CacheComponentTest, IteratorWithExpiredItems) {
  const uint32_t now = util::getCurrentTimeSec();

  auto handle1 = CO_ASSERT_OK(
      co_await this->cache_->allocate("valid_key", 100, now, 3600));
  EXPECT_OK(co_await this->cache_->insert(std::move(handle1).release()));

  // Created 2 hours ago with 1 hour TTL
  auto handle2 = CO_ASSERT_OK(
      co_await this->cache_->allocate("expired_key", 100, now - 7200, 3600));
  EXPECT_OK(co_await this->cache_->insert(std::move(handle2).release()));

  auto handle3 = CO_ASSERT_OK(
      co_await this->cache_->allocate("valid_key2", 100, now, 3600));
  EXPECT_OK(co_await this->cache_->insert(std::move(handle3).release()));

  auto iterator = this->cache_->iterator();
  std::unordered_set<std::string> foundKeys;
  while (auto item = co_await iterator.next()) {
    const auto key = item.value().key();
    foundKeys.insert(std::string(key.data(), key.size()));
  }

  EXPECT_TRUE(foundKeys.count("valid_key"));
  EXPECT_TRUE(foundKeys.count("valid_key2"));
  EXPECT_FALSE(foundKeys.count("expired_key"));
}

// ============================================================================
// remove(Key) Tests
// ============================================================================

CO_TYPED_TEST(CacheComponentTest, RemoveByKeyExistingItem) {
  const std::string key = "remove_key";

  auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, 100, util::getCurrentTimeSec(), 3600));
  EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));

  auto removeResult = CO_ASSERT_OK(co_await this->cache_->remove(key));
  EXPECT_TRUE(removeResult);

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  EXPECT_FALSE(findResult.has_value());
}

CO_TYPED_TEST(CacheComponentTest, RemoveByKeyNonExistentItem) {
  auto removeResult =
      CO_ASSERT_OK(co_await this->cache_->remove("nonexistent_remove"));
  EXPECT_FALSE(removeResult);
}

CO_TYPED_TEST(CacheComponentTest, RemoveByKeyTwice) {
  const std::string key = "remove_twice";

  auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, 100, util::getCurrentTimeSec(), 3600));
  EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));

  auto removeResult1 = CO_ASSERT_OK(co_await this->cache_->remove(key));
  EXPECT_TRUE(removeResult1);

  auto removeResult2 = CO_ASSERT_OK(co_await this->cache_->remove(key));
  EXPECT_FALSE(removeResult2);
}

CO_TYPED_TEST(CacheComponentTest, RemoveMultipleItemsByKey) {
  const int numItems = 10;
  std::vector<std::string> keys;
  keys.reserve(numItems);

  for (int i = 0; i < numItems; ++i) {
    auto& key = keys.emplace_back("remove_multi_" + std::to_string(i));
    auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
        key, 100, util::getCurrentTimeSec(), 3600));
    EXPECT_OK(co_await this->cache_->insert(std::move(handle).release()));
  }

  for (const auto& key : keys) {
    auto removeResult = CO_ASSERT_OK(co_await this->cache_->remove(key));
    EXPECT_TRUE(removeResult);
  }

  for (const auto& key : keys) {
    auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
    EXPECT_FALSE(findResult.has_value());
  }
}

// ============================================================================
// remove(ReadHandle) Tests
// ============================================================================

CO_TYPED_TEST(CacheComponentTest, RemoveByHandle) {
  const std::string key = "remove_handle";

  auto allocHandle = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, 100, util::getCurrentTimeSec(), 3600));
  EXPECT_OK(co_await this->cache_->insert(std::move(allocHandle).release()));

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(findResult.has_value());
  EXPECT_OK(
      co_await this->cache_->remove(std::move(findResult.value()).release()));

  auto findAgainResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  EXPECT_FALSE(findAgainResult.has_value());
}

// ============================================================================
// Multi-threaded Tests
// ============================================================================

TYPED_TEST(CacheComponentTest, MultiThreadedOperations) {
  constexpr int kNumThreads = 8;
  constexpr int kOperationsPerThread = 1000;
  constexpr int kNumSharedKeys = 20;

  std::vector<std::string> sharedKeys;
  sharedKeys.reserve(kNumSharedKeys);
  for (int i = 0; i < kNumSharedKeys; ++i) {
    sharedKeys.emplace_back("shared_key_" + std::to_string(i));
  }

  std::atomic<int> successfulInserts{0};
  std::atomic<int> successfulReplacements{0};
  std::atomic<int> successfulFinds{0};
  std::atomic<int> successfulRemoves{0};
  std::atomic<int> dataCorruptions{0};

  // Make sure we have enough space to modify values in findToWrite
  constexpr size_t maxValueSize = 32;

  auto workerTask = [&](int threadId) -> folly::coro::Task<void> {
    for (int i = 0; i < kOperationsPerThread; ++i) {
      const auto& key = sharedKeys[folly::Random::rand32() % sharedKeys.size()];
      const std::string value =
          "thread_" + std::to_string(threadId) + "_op_" + std::to_string(i);

      int operation = folly::Random::rand32() % 6;
      try {
        switch (operation) {
        case 0: { // allocate + insert
          auto allocResult = co_await this->cache_->allocate(
              key, maxValueSize, util::getCurrentTimeSec(), 3600);
          if (allocResult.hasValue()) {
            std::memcpy(allocResult.value().mutableData(), value.c_str(),
                        value.size());
            auto insertResult = co_await this->cache_->insert(
                std::move(allocResult.value()).release());
            if (insertResult.hasValue()) {
              successfulInserts.fetch_add(1, std::memory_order_relaxed);
            }
          }
          break;
        }

        case 1: { // allocate + insertOrReplace
          auto allocResult = co_await this->cache_->allocate(
              key, maxValueSize, util::getCurrentTimeSec(), 3600);
          if (allocResult.hasValue()) {
            std::memcpy(allocResult.value().mutableData(), value.c_str(),
                        value.size());
            auto replaceResult = co_await this->cache_->insertOrReplace(
                std::move(allocResult.value()).release());
            if (replaceResult.hasValue()) {
              successfulReplacements.fetch_add(1, std::memory_order_relaxed);
            }
          }
          break;
        }

        case 2: { // find
          auto findResult = co_await this->cache_->find(key);
          if (findResult.hasValue() && findResult.value().has_value()) {
            successfulFinds.fetch_add(1, std::memory_order_relaxed);

            const auto& descriptor = findResult.value().value();
            const char* data = static_cast<const char*>(descriptor.data());
            const size_t dataSize = descriptor.size();

            std::string_view retrievedData(data, dataSize);
            if (!retrievedData.starts_with("thread_") &&
                !retrievedData.starts_with("modified_")) {
              dataCorruptions.fetch_add(1, std::memory_order_relaxed);
            }
          }
          break;
        }

        case 3: { // findToWrite + modify
          auto writeResult = co_await this->cache_->findToWrite(key);
          if (writeResult.hasValue() && writeResult.value().has_value()) {
            auto& descriptor = writeResult.value().value();
            const std::string modifiedValue = "modified_" + value;
            CO_ASSERT_GE(descriptor.capacity(), modifiedValue.size());
            std::memcpy(descriptor.mutableData(), modifiedValue.c_str(),
                        modifiedValue.size());
          }
          break;
        }

        case 4: { // remove(key)
          auto removeResult = co_await this->cache_->remove(key);
          if (removeResult.hasValue() && removeResult.value()) {
            successfulRemoves.fetch_add(1, std::memory_order_relaxed);
          }
          break;
        }

        case 5: { // find + remove(handle)
          auto findResult = co_await this->cache_->find(key);
          if (findResult.hasValue() && findResult.value().has_value()) {
            auto removeResult = co_await this->cache_->remove(
                std::move(findResult.value().value()).release());
            if (removeResult.hasValue()) {
              successfulRemoves.fetch_add(1, std::memory_order_relaxed);
            }
          }
          break;
        }

        default:
          throw std::runtime_error("Invalid operation");
          break;
        }
      } catch (const std::exception& e) {
        XLOG(FATAL) << "Thread " << threadId
                    << " caught exception: " << e.what();
      }
    }

    co_return;
  };

  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);

  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&, threadId = i]() {
      folly::coro::blockingWait(workerTask(threadId));
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_GT(successfulInserts.load(), 0);
  EXPECT_GT(successfulReplacements.load(), 0);
  EXPECT_GT(successfulFinds.load(), 0);
  EXPECT_EQ(dataCorruptions.load(), 0);
}

// ============================================================================
// Persist and Recover Tests
// ============================================================================

CO_TYPED_TEST(CacheComponentTest, PersistAndRecover) {
  const std::string key = "persist_key";
  const std::string data = "persist_data";

  {
    auto cache = this->factory_->createPersistent();

    auto handle = CO_ASSERT_OK(co_await cache->allocate(
        key, data.size(), util::getCurrentTimeSec(), 3600));
    std::memcpy(handle.mutableData(), data.c_str(), data.size());
    EXPECT_OK(co_await cache->insert(std::move(handle).release()));

    EXPECT_OK(cache->shutdown());
    this->factory_->onShutdown(*cache);
  }

  auto result = this->factory_->recover();
  CO_ASSERT_TRUE(result.hasValue());
  auto recovered = std::move(result).value();

  auto findResult = CO_ASSERT_OK(co_await recovered->find(key));
  CO_ASSERT_TRUE(findResult.has_value());
  std::string retrievedData(static_cast<const char*>(findResult->data()),
                            findResult->size());
  EXPECT_EQ(retrievedData, data);
}

CO_TYPED_TEST(CacheComponentTest, PersistAndRecoverMultipleItems) {
  constexpr int kNumItems = 10;

  {
    auto cache = this->factory_->createPersistent();

    for (int i = 0; i < kNumItems; ++i) {
      auto key = "key_" + std::to_string(i);
      auto val = "data_" + std::to_string(i);
      auto handle = CO_ASSERT_OK(co_await cache->allocate(
          key, val.size(), util::getCurrentTimeSec(), 3600));
      std::memcpy(handle.mutableData(), val.c_str(), val.size());
      EXPECT_OK(co_await cache->insert(std::move(handle).release()));
    }

    EXPECT_OK(cache->shutdown());
    this->factory_->onShutdown(*cache);
  }

  auto result = this->factory_->recover();
  CO_ASSERT_TRUE(result.hasValue());
  auto recovered = std::move(result).value();

  for (int i = 0; i < kNumItems; ++i) {
    auto key = "key_" + std::to_string(i);
    auto expectedVal = "data_" + std::to_string(i);
    auto findResult = CO_ASSERT_OK(co_await recovered->find(key));
    CO_ASSERT_TRUE(findResult.has_value());
    std::string retrievedData(static_cast<const char*>(findResult->data()),
                              findResult->size());
    EXPECT_EQ(retrievedData, expectedVal);
  }
  EXPECT_OK(recovered->shutdown());
}

CO_TYPED_TEST(CacheComponentTest, RecoverWithoutPriorPersist) {
  // Cache components recover gracefully with an empty cache
  auto recovered = CO_ASSERT_OK(this->factory_->recover());
  auto findResult = CO_ASSERT_OK(co_await recovered->find("nonexistent_key"));
  EXPECT_FALSE(findResult.has_value());
}

CO_TYPED_TEST(CacheComponentTest, PersistShutdownWithPersistence) {
  auto cache = this->factory_->createPersistent();

  auto handle = CO_ASSERT_OK(
      co_await cache->allocate("key", 100, util::getCurrentTimeSec(), 3600));
  EXPECT_OK(co_await cache->insert(std::move(handle).release()));

  EXPECT_OK(cache->shutdown());
}

} // namespace
