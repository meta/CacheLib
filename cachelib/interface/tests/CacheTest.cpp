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

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "cachelib/common/Time.h"
#include "cachelib/interface/Cache.h"
#include "cachelib/interface/components/tests/CacheComponentFactory.h"
#include "cachelib/interface/tests/Utils.h"

using namespace ::testing;
using namespace facebook::cachelib;
using namespace facebook::cachelib::interface;
using namespace facebook::cachelib::interface::test;

namespace {

constexpr uint32_t kTtlSecs{3600};

template <typename FactoryType>
class CacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    factory_ = std::make_unique<FactoryType>();
    cache_ = std::make_unique<Cache>(factory_->create());
  }

  void TearDown() override { EXPECT_OK(cache_->shutdown()); }

  folly::coro::Task<void> insert(std::string key, std::string value) {
    auto handle = CO_ASSERT_OK(co_await cache_->allocate(
        key, value.size(), util::getCurrentTimeSec(), kTtlSecs));
    std::memcpy(handle->getMemory(), value.data(), value.size());
    EXPECT_OK(co_await cache_->insert(std::move(handle)));
  }

  std::unique_ptr<FactoryType> factory_;
  std::unique_ptr<Cache> cache_;
};

using FactoryTypes = ::testing::
    Types<RAMCacheFactory, FlashCacheFactory, ConsistentFlashCacheFactory>;
TYPED_TEST_SUITE(CacheTest, FactoryTypes);

CO_TYPED_TEST(CacheTest, AllocateInsertFindAndExists) {
  const std::string key = "key";
  const std::string value = "value";

  EXPECT_FALSE(CO_ASSERT_OK(co_await this->cache_->exists(key)));
  co_await this->insert(key, value);
  EXPECT_TRUE(CO_ASSERT_OK(co_await this->cache_->exists(key)));

  auto result = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(result.has_value());
  auto handle = std::move(result).value();
  EXPECT_EQ(handle->getKey(), Key{key});
  EXPECT_EQ(std::string(static_cast<const char*>(handle->getMemory()),
                        handle->getMemorySize()),
            value);
}

CO_TYPED_TEST(CacheTest, InsertOrReplaceOverwritesExistingValue) {
  const std::string key = "key";
  const std::string replacement = "replacement";
  co_await this->insert(key, "original");

  auto handle = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, replacement.size(), util::getCurrentTimeSec(), kTtlSecs));
  std::memcpy(handle->getMemory(), replacement.data(), replacement.size());
  // Only assert the replacement is visible: returning a handle to the
  // displaced item is optional, and flash components never do.
  EXPECT_OK(co_await this->cache_->insertOrReplace(std::move(handle)));

  auto result = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(result.has_value());
  auto found = std::move(result).value();
  EXPECT_EQ(std::string(static_cast<const char*>(found->getMemory()),
                        found->getMemorySize()),
            replacement);
}

CO_TYPED_TEST(CacheTest, FindToWriteReturnsWritableHandle) {
  const std::string key = "key";
  const std::string original = "original";
  const std::string updated = "modified";
  CO_ASSERT_EQ(original.size(), updated.size());
  co_await this->insert(key, original);

  {
    auto result = CO_ASSERT_OK(co_await this->cache_->findToWrite(key));
    CO_ASSERT_TRUE(result.has_value());
    auto handle = std::move(result).value();
    std::memcpy(handle->getMemory(), updated.data(), updated.size());
    handle.markDirty();
  }

  auto result = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(result.has_value());
  auto handle = std::move(result).value();
  EXPECT_EQ(std::string(static_cast<const char*>(handle->getMemory()),
                        handle->getMemorySize()),
            updated);
}

CO_TYPED_TEST(CacheTest, IteratorReturnsReadHandles) {
  const std::unordered_map<std::string, std::string> expected{
      {"first", "one"}, {"second", "two"}, {"third", "three"}};
  for (const auto& [key, value] : expected) {
    co_await this->insert(key, value);
  }

  std::unordered_map<std::string, std::string> actual;
  auto iterator = this->cache_->iterator();
  while (auto item = co_await iterator.next()) {
    auto handle = std::move(item).value();
    actual.emplace(handle->getKey(),
                   std::string(static_cast<const char*>(handle->getMemory()),
                               handle->getMemorySize()));
  }

  EXPECT_EQ(actual, expected);
}

CO_TYPED_TEST(CacheTest, RemoveByKeyAndHandle) {
  co_await this->insert("by_key", "one");
  co_await this->insert("by_handle", "two");

  EXPECT_TRUE(CO_ASSERT_OK(co_await this->cache_->remove("by_key")));
  EXPECT_FALSE(CO_ASSERT_OK(co_await this->cache_->exists("by_key")));

  auto result = CO_ASSERT_OK(co_await this->cache_->find("by_handle"));
  CO_ASSERT_TRUE(result.has_value());
  EXPECT_OK(co_await this->cache_->remove(std::move(result).value()));
  EXPECT_FALSE(CO_ASSERT_OK(co_await this->cache_->exists("by_handle")));
}

} // namespace
