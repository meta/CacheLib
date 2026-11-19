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

#include <concepts>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "cachelib/common/Time.h"
#include "cachelib/interface/Descriptor.h"
#include "cachelib/interface/DetachedItem.h"
#include "cachelib/interface/components/tests/CacheComponentFactory.h"
#include "cachelib/interface/tests/Utils.h"

using namespace ::testing;
using namespace facebook::cachelib;
using namespace facebook::cachelib::interface;
using namespace facebook::cachelib::interface::test;

namespace {

constexpr uint32_t kTtlSecs{3600};

// Preserve the ownership boundary even if the handle hierarchy changes:
// WriteDescriptor must accept only an owned WriteHandle, never an allocation.
static_assert(std::constructible_from<WriteDescriptor, WriteHandle&&>);
static_assert(!std::constructible_from<WriteDescriptor, AllocatedHandle&&>);
static_assert(!std::constructible_from<WriteDescriptor, WriteHandle&>);

template <typename FactoryType>
class DescriptorTest : public ::testing::Test {
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

using FactoryTypes = ::testing::
    Types<RAMCacheFactory, FlashCacheFactory, ConsistentFlashCacheFactory>;
TYPED_TEST_SUITE(DescriptorTest, FactoryTypes);

CO_TYPED_TEST(DescriptorTest, AllocatedDescriptorWriteReleaseInsert) {
  const std::string key = "write_release_insert";
  const std::string data = "written_through_descriptor";

  auto descriptor = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, data.size(), util::getCurrentTimeSec(), kTtlSecs));

  EXPECT_EQ(descriptor.capacity(), data.size());
  std::memcpy(descriptor.mutableData(), data.data(), data.size());

  EXPECT_OK(co_await this->cache_->insert(std::move(descriptor).release()));
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(static_cast<bool>(descriptor));

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(findResult.has_value());
  auto& readDescriptor = findResult.value();
  EXPECT_EQ(std::string(static_cast<const char*>(readDescriptor.data()),
                        readDescriptor.size()),
            data);
}

CO_TYPED_TEST(DescriptorTest, ReadDescriptorReleaseRemove) {
  const std::string key = "read_release_remove";
  const std::string data = "read_then_removed";

  auto allocated = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, data.size(), util::getCurrentTimeSec(), kTtlSecs));
  std::memcpy(allocated.mutableData(), data.data(), data.size());
  EXPECT_OK(co_await this->cache_->insert(std::move(allocated).release()));

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(findResult.has_value());
  auto descriptor = std::move(findResult.value());
  EXPECT_EQ(descriptor.size(), data.size());
  EXPECT_EQ(
      std::string(static_cast<const char*>(descriptor.data()), data.size()),
      data);

  EXPECT_OK(co_await this->cache_->remove(std::move(descriptor).release()));
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(static_cast<bool>(descriptor));

  auto afterRemove = CO_ASSERT_OK(co_await this->cache_->find(key));
  EXPECT_FALSE(afterRemove.has_value());
}

CO_TYPED_TEST(DescriptorTest, ReadDescriptorPreservesViewAcrossMoves) {
  const std::string key = "read_descriptor_move";
  const std::string data = "read_descriptor_value";
  const auto creationTime = util::getCurrentTimeSec();

  auto descriptor = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, data.size(), creationTime, kTtlSecs));
  std::memcpy(descriptor.mutableData(), data.data(), data.size());
  EXPECT_OK(co_await this->cache_->insert(std::move(descriptor).release()));

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(findResult.has_value());
  ReadDescriptor first(std::move(findResult.value()));
  ReadDescriptor second(std::move(first));
  ReadDescriptor moved(std::move(second));

  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(static_cast<bool>(first));
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(static_cast<bool>(second));
  CO_ASSERT_TRUE(static_cast<bool>(moved));
  EXPECT_TRUE(moved.isHandleBacked());
  EXPECT_EQ(moved.key(), Key{key});
  EXPECT_EQ(moved.creationTime(), creationTime);
  EXPECT_EQ(moved.expiryTime(), creationTime + kTtlSecs);
  EXPECT_EQ(moved.size(), data.size());
  EXPECT_EQ(std::string(static_cast<const char*>(moved.data()), data.size()),
            data);
}

CO_TYPED_TEST(DescriptorTest, OwnedReadDescriptorOutlivesSourceItem) {
  const std::string key = "owned_read_descriptor";
  const std::string data = "detached_value";
  const auto creationTime = util::getCurrentTimeSec();

  auto allocated = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, data.size(), creationTime, kTtlSecs));
  std::memcpy(allocated.mutableData(), data.data(), data.size());
  EXPECT_OK(co_await this->cache_->insert(std::move(allocated).release()));

  std::optional<DetachedItem> ownedItem;
  {
    auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
    CO_ASSERT_TRUE(findResult.has_value());
    auto foundHandle = std::move(findResult.value()).release();
    ownedItem.emplace(*foundHandle);
  }

  EXPECT_TRUE(CO_ASSERT_OK(co_await this->cache_->remove(key)));

  ReadDescriptor descriptor(std::move(*ownedItem));
  CO_ASSERT_TRUE(static_cast<bool>(descriptor));
  EXPECT_FALSE(descriptor.isHandleBacked());
  EXPECT_EQ(descriptor.key(), Key{key});
  EXPECT_EQ(descriptor.creationTime(), creationTime);
  EXPECT_EQ(descriptor.expiryTime(), creationTime + kTtlSecs);
  EXPECT_EQ(descriptor.size(), data.size());
  EXPECT_EQ(
      std::string(static_cast<const char*>(descriptor.data()), data.size()),
      data);

  ReadDescriptor moved(std::move(descriptor));
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(static_cast<bool>(descriptor));
  CO_ASSERT_TRUE(static_cast<bool>(moved));
  EXPECT_EQ(moved.key(), Key{key});
  EXPECT_EQ(std::string(static_cast<const char*>(moved.data()), moved.size()),
            data);
}

CO_TYPED_TEST(DescriptorTest, OwnedReadDescriptorSupportsEmptyValue) {
  const std::string key = "owned_empty_value";
  const auto creationTime = util::getCurrentTimeSec();

  auto allocated = CO_ASSERT_OK(
      co_await this->cache_->allocate(key, 0, creationTime, kTtlSecs));
  EXPECT_OK(co_await this->cache_->insert(std::move(allocated).release()));

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(findResult.has_value());
  auto foundHandle = std::move(findResult.value()).release();
  ReadDescriptor descriptor{DetachedItem(*foundHandle)};

  // A zero-size value must still read as present -- emptiness of the value and
  // emptiness of the descriptor are different states.
  CO_ASSERT_TRUE(static_cast<bool>(descriptor));
  EXPECT_FALSE(descriptor.isHandleBacked());
  EXPECT_EQ(descriptor.key(), Key{key});
  EXPECT_EQ(descriptor.size(), 0u);
}

// Dropping an AllocatedDescriptor must discard the allocation rather than
// publish it. This is also the runtime guard that mutableData() does not mark
// the handle dirty: if it did, ~WriteHandle() would write the item back and
// index it, and the find() below would hit.
CO_TYPED_TEST(DescriptorTest, AllocatedDescriptorDiscardsWithoutInsert) {
  const std::string key = "discarded_without_insert";
  const std::string data = "never_inserted";

  {
    auto descriptor = CO_ASSERT_OK(co_await this->cache_->allocate(
        key, data.size(), util::getCurrentTimeSec(), kTtlSecs));
    std::memcpy(descriptor.mutableData(), data.data(), data.size());
  }

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  EXPECT_FALSE(findResult.has_value());
}

CO_TYPED_TEST(DescriptorTest, WriteDescriptorMutableDataFlushes) {
  const std::string key = "write_descriptor_flushes";
  const std::string original = "original_value";
  const std::string updated = "UPDATED!_value";
  CO_ASSERT_EQ(original.size(), updated.size());

  auto allocated = CO_ASSERT_OK(co_await this->cache_->allocate(
      key, original.size(), util::getCurrentTimeSec(), kTtlSecs));
  std::memcpy(allocated.mutableData(), original.data(), original.size());
  EXPECT_OK(co_await this->cache_->insert(std::move(allocated).release()));

  {
    auto writeResult = CO_ASSERT_OK(co_await this->cache_->findToWrite(key));
    CO_ASSERT_TRUE(writeResult.has_value());
    WriteDescriptor movedFrom(std::move(writeResult.value()));
    WriteDescriptor descriptor(std::move(movedFrom));
    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_FALSE(static_cast<bool>(movedFrom));
    CO_ASSERT_TRUE(static_cast<bool>(descriptor));

    EXPECT_EQ(descriptor.size(), updated.size());
    EXPECT_EQ(descriptor.capacity(), descriptor.size());
    std::memcpy(descriptor.mutableData(), updated.data(), updated.size());
  }
  // Scope exit destroys the descriptor: ~WriteHandle() sees dirty_ and flushes.

  auto findResult = CO_ASSERT_OK(co_await this->cache_->find(key));
  CO_ASSERT_TRUE(findResult.has_value());
  auto& descriptor = findResult.value();
  EXPECT_EQ(std::string(static_cast<const char*>(descriptor.data()),
                        descriptor.size()),
            updated);
}

} // namespace
