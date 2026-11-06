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

#include "cachelib/common/Time.h"
#include "cachelib/interface/components/tests/CacheComponentFactory.h"
#include "cachelib/interface/tests/Utils.h"

using namespace facebook::cachelib::interface;
using namespace facebook::cachelib::interface::test;

namespace {

class ChecksummedFlashCacheFactory : public FlashCacheFactory {
 public:
  explicit ChecksummedFlashCacheFactory(
      facebook::cachelib::navy::ChecksumType checksumType)
      : checksumType_(checksumType) {}

 protected:
  Result<std::unique_ptr<CacheComponent>> createWithPersistence(
      std::unique_ptr<facebook::cachelib::navy::Device> device,
      FlashCacheComponent::PersistenceConfig persistenceConfig) override {
    auto config = makeConfig();
    config.checksum = true;
    config.checksumType = checksumType_;
    facebook::cachelib::interface::utils::CoroFiberAdapter::Config
        executorConfig{
            .threadName = "checksum_test",
            .numThreads = 2,
            .fibersPerThread = 2,
            .stackSize = 32 * 1024,
        };
    auto result = FlashCacheComponent::create("ChecksumTest", std::move(config),
                                              std::move(device), executorConfig,
                                              std::move(persistenceConfig));
    if (result.hasError()) {
      return folly::makeUnexpected(std::move(result).error());
    }
    return std::make_unique<FlashCacheComponent>(std::move(result).value());
  }

 private:
  const facebook::cachelib::navy::ChecksumType checksumType_;
};

CO_TEST(FlashCacheComponent, ChecksumFormatsPersistAndRecover) {
  using facebook::cachelib::navy::ChecksumType;
  for (auto checksumType : {ChecksumType::Crc32, ChecksumType::Crc32c}) {
    ChecksummedFlashCacheFactory factory(checksumType);
    auto cache = factory.createPersistent();
    CO_ASSERT_NE(nullptr, cache);
    const std::string key = "checksum-key";
    const std::string original = "original";
    const std::string replacement = "modified";
    auto handle = CO_ASSERT_OK(co_await cache->allocate(
        key, original.size(), facebook::cachelib::util::getCurrentTimeSec(),
        0));
    std::memcpy(handle.mutableData(), original.data(), original.size());
    EXPECT_OK(co_await cache->insert(std::move(handle).release()));
    {
      auto writable = CO_ASSERT_OK(co_await cache->findToWrite(key));
      CO_ASSERT_TRUE(writable.has_value());
      std::memcpy(writable->mutableData(), replacement.data(),
                  replacement.size());
    }
    EXPECT_OK(cache->shutdown());
    factory.onShutdown(*cache);
    cache.reset();

    auto recovered = CO_ASSERT_OK(factory.recover());
    {
      auto found = CO_ASSERT_OK(co_await recovered->find(key));
      CO_ASSERT_TRUE(found.has_value());
      EXPECT_EQ(
          replacement,
          std::string(static_cast<const char*>(found->data()), found->size()));
    }
    EXPECT_OK(recovered->shutdown());
  }
}

template <typename FactoryType>
class FlashCacheComponentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    factory_ = std::make_unique<FactoryType>();
    cache_ = factory_->create();
    ASSERT_NE(cache_, nullptr) << "Failed to create cache";
  }

  std::unique_ptr<CacheComponent> cache_;

 private:
  std::unique_ptr<FactoryType> factory_;
};

using FlashFactoryTypes =
    ::testing::Types<FlashCacheFactory, ConsistentFlashCacheFactory>;
TYPED_TEST_SUITE(FlashCacheComponentTest, FlashFactoryTypes);

CO_TYPED_TEST(FlashCacheComponentTest, ExpirationCallback) {
  const uint32_t currentTime = facebook::cachelib::util::getCurrentTimeSec();
  constexpr uint32_t kValueSize = 100;
  constexpr uint32_t kTTL = 3600;

  auto valid = CO_ASSERT_OK(co_await this->cache_->allocate(
      "valid_0", kValueSize, currentTime, kTTL));
  EXPECT_OK(co_await this->cache_->insert(std::move(valid).release()));

  auto valid2 = CO_ASSERT_OK(co_await this->cache_->allocate(
      "valid_1", kValueSize, currentTime - kTTL / 2, kTTL));
  EXPECT_OK(co_await this->cache_->insert(std::move(valid2).release()));

  auto expired = CO_ASSERT_OK(co_await this->cache_->allocate(
      "expired_0", kValueSize, currentTime - 2 * kTTL, kTTL));
  EXPECT_OK(co_await this->cache_->insert(std::move(expired).release()));

  auto expired2 = CO_ASSERT_OK(co_await this->cache_->allocate(
      "expired_1", kValueSize, currentTime - 10 * kTTL, kTTL));
  EXPECT_OK(co_await this->cache_->insert(std::move(expired2).release()));

  // Fill the cache to force region reclamation, which exercises the
  // checkExpired callback configured in FlashCacheComponent::initConfig().
  for (int i = 0; i < 100; ++i) {
    auto res = co_await this->cache_->allocate("fill_" + std::to_string(i),
                                               3000, currentTime, kTTL);
    if (res.hasError()) {
      break;
    }
    EXPECT_OK(co_await this->cache_->insertOrReplace(
        std::move(res.value()).release()));
  }

  // Expired items should not have been reinserted during reclamation.
  auto rates = this->cache_->getStats().extraStats_.getRates();
  auto statIt = rates.find("navy_bc_evictions_expired");
  CO_ASSERT_NE(statIt, rates.end());
  EXPECT_EQ(statIt->second, 2);

  // Valid items should have been reinserted during reclamation.  Note that
  // reclamation *must* have iterated over these since they were inserted before
  // the expired items.
  for (int i = 0; i < 2; ++i) {
    auto key = "valid_" + std::to_string(i);
    auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
    CO_ASSERT_TRUE(findResult.has_value());
    auto foundHandle = std::move(findResult.value()).release();
    EXPECT_EQ(foundHandle->getKey(), key);
    EXPECT_EQ(foundHandle->getExpiryTime(),
              foundHandle->getCreationTime() + kTTL);
  }
}

} // namespace
