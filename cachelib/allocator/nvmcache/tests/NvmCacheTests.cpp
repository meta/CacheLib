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

#include <fmt/format.h>
#include <folly/Random.h>
#include <gtest/gtest.h>

#include <array>
#include <climits>
#include <cstring>
#include <set>
#include <thread>

#include "cachelib/allocator/nvmcache/tests/NvmTestBase.h"

namespace facebook {
namespace cachelib {
namespace tests {

namespace {
std::string genRandomStr(size_t len) {
  std::string text;
  text.reserve(len);
  for (unsigned int i = 0; i < len; i++) {
    // avoid char 0 in string
    text += static_cast<char>(folly::Random::rand32() % UCHAR_MAX + 1);
  }
  return text;
}
} // namespace
TEST_F(NvmCacheTest, Config) {
  struct MockEncryptor : public navy::DeviceEncryptor {
   public:
    uint32_t encryptionBlockSize() const override { return 5555; }
    bool encrypt(folly::MutableByteRange, uint64_t) override { return true; }
    bool decrypt(folly::MutableByteRange, uint64_t) override { return true; }
  };

  auto config = *this->getConfig().nvmConfig;
  ASSERT_NO_THROW(config.validateAndSetDefaults());

  config.navyConfig.setBlockSize(5555);
  config.navyConfig.bigHash()
      .setSizePctAndMaxItemSize(50, 1024)
      .setBucketSize(5555)
      .setBucketBfSize(8);
  config.deviceEncryptor = std::make_shared<MockEncryptor>();
  ASSERT_NO_THROW(config.validateAndSetDefaults());

  config.navyConfig.setBlockSize(4444);
  ASSERT_THROW(config.validateAndSetDefaults(), std::invalid_argument);

  config.navyConfig.setBlockSize(5555);
  config.navyConfig.bigHash().setBucketSize(4444);
  ASSERT_THROW(config.validateAndSetDefaults(), std::invalid_argument);
}

namespace {
template <typename T>
struct MockNvmAdmissionPolicy : public NvmAdmissionPolicy<T> {
  MockNvmAdmissionPolicy() = default;
  using Item = typename T::Item;
  using ChainedItemIter = typename T::ChainedItemIter;

 protected:
  virtual bool acceptImpl(const Item&, folly::Range<ChainedItemIter>) override {
    return true;
  }
  virtual void getCountersImpl(const util::CounterVisitor& visitor) override {
    visitor("nvm_mock_policy", 1);
  }
};
} // namespace

TEST_F(NvmCacheTest, APConfig) {
  auto policy = std::make_shared<MockNvmAdmissionPolicy<AllocatorT>>();
  {
    auto& config = getConfig();
    config.enableRejectFirstAPForNvm(10, 10, 1, true);
    auto& nvm = makeCache();
    auto ctrs = nvm.getNvmCacheStatsMap().getCounts();
    EXPECT_NE(ctrs.find("ap.reject_first_keys_tracked"), ctrs.end());
  }

  {
    auto& config = getConfig();
    config.setNvmCacheAdmissionPolicy(policy);
    ASSERT_NO_THROW(config.validate());
    auto& nvm = makeCache();
    auto ctrs = nvm.getNvmCacheStatsMap().getCounts();
    EXPECT_NE(ctrs.find("nvm_mock_policy"), ctrs.end());
  }

  // setting both reject first and custom ap will give custom ap the higher
  // priority
  {
    auto& config = getConfig();
    config.enableRejectFirstAPForNvm(10, 10, 1, true);
    auto& nvm = makeCache();
    auto ctrs = nvm.getNvmCacheStatsMap().getCounts();
    EXPECT_NE(ctrs.find("nvm_mock_policy"), ctrs.end());
  }

  {
    auto& config = getConfig();
    EXPECT_THROW(config.setNvmCacheAdmissionPolicy(nullptr),
                 std::invalid_argument);
  }
}

TEST_F(NvmCacheTest, BasicGet) {
  auto& nvm = this->cache();
  auto pid = this->poolId();

  std::string key = "blah";

  ASSERT_FALSE(this->checkKeyExists(key, false /* ramOnly */));

  {
    auto it = nvm.allocate(pid, key, 100);
    nvm.insertOrReplace(it);
  }

  ASSERT_TRUE(this->checkKeyExists(key, true /* ramOnly */));
  ASSERT_TRUE(this->checkKeyExists(key, false /* ramOnly */));
}

TEST_F(NvmCacheTest, FindMany) {
  auto& cache = this->cache();
  for (const std::string key : {"dram", "nvm"}) {
    auto it = cache.allocate(this->poolId(), key, 100);
    ASSERT_NE(nullptr, it);
    std::memset(it->getMemory(), key[0], 100);
    cache.insertOrReplace(it);
  }
  ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting("nvm"));
  this->removeFromRamForTesting("nvm");
  ASSERT_FALSE(this->checkKeyExists("nvm", true /* ramOnly */));

  std::vector<LruAllocator::Key> keys{"dram", "nvm", "missing"};
  std::vector<ReadHandle> handles(keys.size());
  cache.findMany(folly::range(keys), folly::range(handles));
  for (const auto& handle : handles) {
    handle.wait();
  }
  ASSERT_NE(nullptr, handles[0]);
  EXPECT_EQ('d', *reinterpret_cast<const char*>(handles[0]->getMemory()));
  ASSERT_NE(nullptr, handles[1]);
  EXPECT_EQ(LruAllocator::Key{"nvm"}, handles[1]->getKey());
  EXPECT_EQ('n', *reinterpret_cast<const char*>(handles[1]->getMemory()));
  EXPECT_EQ(nullptr, handles[2]);
}

TEST_F(NvmCacheTest, CouldExistFast) {
  // Enable fast negative lookup
  this->makeCache();

  auto& nvm = this->cache();
  auto pid = this->poolId();

  std::string key = "blah";

  ASSERT_FALSE(this->cache().couldExistFast(key));

  {
    auto it = nvm.allocate(pid, key, 100);
    nvm.insertOrReplace(it);
  }

  ASSERT_TRUE(this->cache().couldExistFast(key));
  this->pushToNvmCacheFromRamForTesting(key);
  this->removeFromRamForTesting(key);
  ASSERT_TRUE(this->cache().couldExistFast(key));
}

TEST_F(NvmCacheTest, ExistFast) {
  // Enable fast negative lookup
  this->makeCache();

  auto& nvm = this->cache();
  auto pid = this->poolId();

  std::string key = "blah";

  ASSERT_EQ(StorageMedium::NONE, this->cache().existFast(key));

  {
    auto it = nvm.allocate(pid, key, 100);
    nvm.insertOrReplace(it);
  }

  ASSERT_EQ(StorageMedium::DRAM, this->cache().existFast(key));
  this->pushToNvmCacheFromRamForTesting(key);
  this->removeFromRamForTesting(key);
  ASSERT_EQ(StorageMedium::NVM, this->cache().existFast(key));
}

TEST_F(NvmCacheTest, EvictToNvmGet) {
  // Disable bighash since we're only testing large items here
  this->config_.bigHash().setSizePctAndMaxItemSize(0, 100);
  LruAllocator::NvmCacheConfig nvmConfig;
  nvmConfig.navyConfig = config_;
  this->allocConfig_.enableNvmCache(nvmConfig);
  this->makeCache();

  auto& nvm = this->cache();
  auto pid = this->poolId();

  const auto evictBefore = this->evictionCount();
  const int nKeys = 1024;

  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = fmt::format("key{}", i);
    auto it = nvm.allocate(pid, key, 15 * 1024);
    ASSERT_NE(nullptr, it);
    nvm.insertOrReplace(it);

    // Ensure nvm-cache is flushed every 100 items. The reason is to
    // make sure we don't have any race between a "remove" operation
    // queued by an item's initial insertion with this item's eventual
    // eviction. We only flush every 100 items to avoid pushing too
    // many regions to flash that only has one item per region. If we
    // flushed per insertion, we would fill up BlockCache prematurely
    // and trigger evictions which are not desirable in this test.
    if (i % 100 == 0) {
      nvm.flushNvmCache();
    }
  }
  nvm.flushNvmCache();

  const auto nEvictions = this->evictionCount() - evictBefore;
  ASSERT_LT(0, nEvictions);

  // Read from ram cache first so that we will not cause evictions
  // to navy for items that are still in ram-cache until we start
  // reading items from navy.
  for (unsigned int i = nKeys + 100; i-- > 0;) {
    unsigned int index = i;
    auto key = fmt::format("key{}", index);
    auto hdl = this->fetch(key, false /* ramOnly */);
    hdl.wait();
    if (index < nKeys) {
      ASSERT_NE(nullptr, hdl) << fmt::format("key: {}", key);
      // First nEvictions keys should have nvm clean bit set since
      // we load it from nvm
      const auto isClean = hdl->isNvmClean();
      if (index < nEvictions) {
        // A handle read from nvm-cache does not have any outstanding handle
        // associated with it on the local thread. It is adjusted at destruction
        // time to be net-zero for handle count (first an Inc, and then a Dec).
        EXPECT_EQ(0, nvm.getHandleCountForThread())
            << fmt::format("key: {} was read from Navy", key);
        ASSERT_TRUE(isClean);
        ASSERT_TRUE(hdl.wentToNvm());
      } else {
        // A handle read from ram-cache will have incremented the thread-local
        // handle count when we have acquired the handle.
        EXPECT_EQ(1, nvm.getHandleCountForThread())
            << fmt::format("key: {} was read from RAM cache", key);
        ASSERT_FALSE(isClean);
      }
    } else {
      ASSERT_EQ(nullptr, hdl);
    }
  }

  // Flush to ensure all async navy callbacks (including GetCtx destruction
  // which decrements handle counts) have fully completed.
  nvm.flushNvmCache();

  // Reads are done. We should be at "0" active handle count across all threads.
  ASSERT_EQ(0, nvm.getNumActiveHandles());
  ASSERT_EQ(0, nvm.getHandleCountForThread());
}

TEST_F(NvmCacheTest, EvictToNvmGetCheckCtime) {
  auto& nvm = this->cache();
  auto pid = this->poolId();

  const auto evictBefore = this->evictionCount();
  const int nKeys = 1024;

  std::unordered_map<std::string, uint32_t> keyToCtime;
  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    auto it = nvm.allocate(pid, key, 15 * 1024);
    ASSERT_NE(nullptr, it);
    cache_->insertOrReplace(it);
    keyToCtime.insert({key, it->getCreationTime()});
    // Avoid any nvm eviction being dropped due to the race with still
    // outstanding remove operation for insertion
    if (i % 100 == 0) {
      nvm.flushNvmCache();
    }
  }
  nvm.flushNvmCache();

  const auto nEvictions = this->evictionCount() - evictBefore;
  ASSERT_LT(0, nEvictions);

  /* sleep override */ std::this_thread::sleep_for(std::chrono::seconds(5));

  // read from reverse to no cause evictions to navy
  for (unsigned int i = nKeys - 1; i > 0; i--) {
    auto key = std::string("blah") + folly::to<std::string>(i - 1);
    auto hdl = this->fetch(key, false /* ramOnly */);
    hdl.wait();
    XDCHECK(hdl);
    ASSERT_EQ(hdl->getCreationTime(), keyToCtime[key]);
  }
}

TEST_F(NvmCacheTest, EvictToNvmExpired) {
  // Test with TTL, so the reaper should be disabled
  auto& config = this->getConfig();
  config.reaperInterval = std::chrono::milliseconds(0);
  auto& nvm = this->makeCache();
  auto pid = this->poolId();

  const uint32_t ttl = 5; // 5 second ttl
  std::string key = "blah";
  {
    auto it = nvm.allocate(pid, key, 15 * 1024, ttl);
    ASSERT_NE(nullptr, it);
    nvm.insertOrReplace(it);

    /* sleep override */ std::this_thread::sleep_for(
        std::chrono::seconds(ttl + 1));
    ASSERT_TRUE(it->isExpired());
  }

  this->pushToNvmCacheFromRamForTesting(key);
  this->removeFromRamForTesting(key);
  // should not be present in RAM since we pushed it to nvmcache
  ASSERT_FALSE(this->checkKeyExists(key, true /*ram Only*/));

  // should not have been in nvmcache since it expired.
  ASSERT_FALSE(this->checkKeyExists(key, false /* ram Only */));
}

TEST_F(NvmCacheTest, ReadFromNvmExpired) {
  // Test with TTL, so the reaper should be disabled
  auto& config = this->getConfig();
  config.reaperInterval = std::chrono::milliseconds(0);
  auto& nvm = this->makeCache();
  auto pid = this->poolId();

  const uint32_t ttl = 5; // 5 second ttl
  std::string key = "blah";
  {
    auto it = nvm.allocate(pid, key, 15 * 1024, ttl);
    ASSERT_NE(nullptr, it);
    nvm.insertOrReplace(it);
    ASSERT_FALSE(it->isExpired());
  }

  ASSERT_TRUE(this->checkKeyExists(key, true /*ram Only*/));

  this->pushToNvmCacheFromRamForTesting(key);
  this->removeFromRamForTesting(key);
  // should not be present in RAM since we pushed it to nvmcache
  ASSERT_FALSE(this->checkKeyExists(key, true /*ram Only*/));

  /* sleep override */ std::this_thread::sleep_for(
      std::chrono::seconds(ttl + 1));

  // reading an expired object from nvmcache must not insert it into cache.
  ASSERT_FALSE(this->checkKeyExists(key, false /* ram only */));
  {
    auto it = this->fetch(key, false /* ram Only */);
    ASSERT_EQ(nullptr, it);
    ASSERT_TRUE(it.wentToNvm());
    ASSERT_TRUE(it.wasExpired());
  }
}

TEST_F(NvmCacheTest, Delete) {
  auto& nvm = this->cache();
  auto pid = this->poolId();

  const int nKeys = 1024;

  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    auto it = nvm.allocate(pid, key, 15 * 1024);
    ASSERT_NE(nullptr, it);
    nvm.insertOrReplace(it);
    // Avoid any nvm eviction being dropped due to the race with still
    // outstanding remove operation for insertion
    if (i % 100 == 0) {
      nvm.flushNvmCache();
    }
  }
  nvm.flushNvmCache();

  // fetch all of them
  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    ASSERT_TRUE(this->checkKeyExists(key, false /* ramOnly */));
  }

  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    nvm.remove(key);
  }

  // fetch should fail for all
  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    ASSERT_FALSE(this->checkKeyExists(key, false /* ramOnly */));
  }
}

TEST_F(NvmCacheTest, InsertOrReplace) {
  auto& nvm = this->cache();
  auto pid = this->poolId();

  std::string key = "blah";

  {
    auto it = nvm.allocate(pid, key, 100);
    ASSERT_NE(nullptr, it);
    *(int*)it->getMemory() = 0xdeadbeef;
    nvm.insertOrReplace(it);
  }

  // verify contents after fetching

  {
    auto it = this->fetch(key, false);
    auto val = *(int*)it->getMemory();
    ASSERT_EQ(0xdeadbeef, val);
  }

  // replace with new content

  {
    auto it = nvm.allocate(pid, key, 100);
    ASSERT_NE(nullptr, it);
    *(int*)it->getMemory() = 0x5a5a5a5a;
    nvm.insertOrReplace(it);
  }

  // verify contents after fetching

  {
    auto it = this->fetch(key, false);
    auto val = *(int*)it->getMemory();
    ASSERT_EQ(0x5a5a5a5a, val);
  }
}

TEST_F(NvmCacheTest, ConcurrentFills) {
  auto& nvm = this->cache();
  auto pid = this->poolId();

  const int nKeys = 1024;

  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    auto it = nvm.allocate(pid, key, 15 * 1024);
    ASSERT_NE(nullptr, it);
    *((int*)it->getMemory()) = i;
    nvm.insertOrReplace(it);
  }

  auto doConcurrentFetch = [&](int id) {
    auto key = std::string("blah") + folly::to<std::string>(id);
    std::vector<std::thread> thr;
    std::atomic<bool> missed = false;
    for (unsigned int j = 0; j < 50; j++) {
      thr.emplace_back([&]() {
        auto hdl = nvm.find(key);
        hdl.wait();
        if (!hdl) {
          missed = true;
        } else {
          ASSERT_EQ(id, *hdl->getMemoryAs<int>());
        }
      });
    }
    for (unsigned int j = 0; j < 50; j++) {
      thr[j].join();
    }
    return missed.load(std::memory_order_relaxed);
  };
  size_t misses{0};
  for (unsigned int i = 0; i < nKeys; i++) {
    misses += doConcurrentFetch(i);
  }
  // The number of misses equals to the number of puts aborted in the process.
  // Aborts can happen if an item's NvmCache::remove issued by
  // CacheAllocator::insertOrReplace is still in flight when the item is evicted
  // from RAM.
  ASSERT_EQ(nvm.getGlobalCacheStats().numNvmAbortedPutOnTombstone, misses);
}

TEST_F(NvmCacheTest, NvmClean) {
  auto& nvm = this->cache();
  auto pid = this->poolId();

  auto evictBefore = this->evictionCount();
  auto putsBefore = this->getStats().numNvmPuts;
  const int nKeys = 1024;
  const uint32_t allocSize = 15 * 1024;

  // We determine how many keys fit into a Navy block-cache region.
  const uint32_t numKeysPerRegion =
      config_.blockCache().getRegionSize() / allocSize;

  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    auto it = nvm.allocate(pid, key, allocSize);
    ASSERT_NE(nullptr, it);
    cache_->insertOrReplace(it);

    if (i % numKeysPerRegion == 0) {
      // Flush nvm-cache. The reason we flush is to make sure remove jobs
      // enqueued when we call "insertOrReplace()" is finished to avoid
      // a scenario where we evict key "Foo" from cache while a previously
      // enqueued remove job for "Foo" is still pending, which would lead
      // to "Foo" not being inserted into the cache. And the reason we only
      // flush every "numKeysPerRegion" is to make sure we don't end up
      // trigger evictions from flash-cache by flushing too frequently.
      // If we flush after each insertion, then one region only fits a single
      // item.
      cache_->flushNvmCache();
    }
  }
  cache_->flushNvmCache();

  auto nEvictions = this->evictionCount() - evictBefore;
  auto nPuts = this->getStats().numNvmPuts - putsBefore;
  ASSERT_LT(0, nEvictions);
  ASSERT_EQ(nPuts, nEvictions);
  evictBefore = this->evictionCount();
  putsBefore = this->getStats().numNvmPuts;

  // read everything again. This should churn and cause the current ones to be
  // evicted to nvmcache.
  size_t numClean = 0;
  for (unsigned int i = nKeys; i > 0; i--) {
    auto key = std::string("blah") + folly::to<std::string>(i - 1);
    bool missInRam = !this->checkKeyExists(key, true /* ramOnly */);
    auto hdl = this->fetch(key, false /* ramOnly */);
    hdl.wait();
    ASSERT_TRUE(hdl);
    if (missInRam) {
      ++numClean;
      ASSERT_TRUE(hdl->isNvmClean());
    }
  }
  ASSERT_LT(0, numClean);

  // we must have done evictions from ram to navy
  nEvictions = this->evictionCount() - evictBefore;
  nPuts = this->getStats().numNvmPuts - putsBefore;
  ASSERT_LT(0, nEvictions);
  ASSERT_EQ(nKeys - numClean, nPuts);

  putsBefore = this->getStats().numNvmPuts;
  evictBefore = this->evictionCount();

  // read everything again. This should cause everything to be clean
  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    auto hdl = this->fetch(key, false /* ramOnly */);
    hdl.wait();
    ASSERT_TRUE(hdl);
    ASSERT_TRUE(hdl->isNvmClean());
  }
  ASSERT_EQ(0, this->getStats().numNvmEvictions);

  // we must have done evictions from ram to navy
  nEvictions = this->evictionCount() - evictBefore;
  nPuts = this->getStats().numNvmPuts - putsBefore;
  ASSERT_LT(0, nEvictions);
  ASSERT_EQ(0, nPuts);
}

// put nvmclean entries in cache and then mark them as nvmRewrite. this should
// write them to nvmcache.
TEST_F(NvmCacheTest, NvmEvicted) {
  auto& nvm = this->cache();
  auto pid = this->poolId();

  const int nKeys = 1024;
  const uint32_t allocSize = 15 * 1024;

  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    auto it = nvm.allocate(pid, key, allocSize);
    ASSERT_NE(nullptr, it);
    nvm.insertOrReplace(it);
    // Avoid any nvm eviction being dropped due to the race with still
    // outstanding remove operation for insertion
    if (i % 100 == 0) {
      nvm.flushNvmCache();
    }
  }
  nvm.flushNvmCache();

  // read everything again. This should churn and cause the current ones to be
  // evicted to nvmcache.
  for (unsigned int i = nKeys - 1; i > 0; i--) {
    auto key = std::string("blah") + folly::to<std::string>(i - 1);
    auto hdl = this->fetch(key, false /* ramOnly */);
    hdl.wait();
    ASSERT_TRUE(hdl);
  }

  // read everything again. This should cause everything to be clean
  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    auto hdl = this->fetch(key, false /* ramOnly */);
    hdl.wait();
    XDCHECK(hdl);
    ASSERT_TRUE(hdl->isNvmClean());
  }

  auto putsBefore = this->getStats().numNvmPuts;
  auto evictBefore = this->evictionCount();
  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    auto hdl = this->fetch(key, false /* ramOnly */);
    hdl.wait();
    XDCHECK(hdl);
    ASSERT_TRUE(hdl->isNvmClean());
  }

  // nothing should be put since everything is nvmclean
  ASSERT_EQ(this->getStats().numNvmPuts, putsBefore);
  ASSERT_EQ(this->evictionCount() - evictBefore, nKeys);

  putsBefore = this->getStats().numNvmPuts;
  evictBefore = this->evictionCount();

  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    auto hdl = this->fetch(key, false /* ramOnly */);
    hdl.wait();
    XDCHECK(hdl);
    ASSERT_TRUE(hdl->isNvmClean());
    hdl->markNvmEvicted();
  }

  for (unsigned int i = 0; i < nKeys; i++) {
    auto key = std::string("blah") + folly::to<std::string>(i);
    auto hdl = this->fetch(key, false /* ramOnly */);
    hdl.wait();
    XDCHECK(hdl);
    ASSERT_TRUE(hdl->isNvmClean());
  }

  // we must have done evictions from ram to navy
  ASSERT_EQ(this->evictionCount() - evictBefore, 2 * nKeys);
  ASSERT_EQ(nKeys, this->getStats().numNvmPuts - putsBefore);
  ASSERT_EQ(nKeys, this->getStats().numNvmPutFromClean);
}

TEST_F(NvmCacheTest, InspectCache) {
  auto& cache = this->cache();
  auto pid = this->poolId();

  std::string key = "blah";
  std::string val = "foobar";
  {
    auto it = cache.allocate(pid, key, val.length());
    ASSERT_NE(nullptr, it);
    cache.insertOrReplace(it);
    ::memcpy(it->getMemory(), val.data(), val.length());
  }

  // item is only in RAM
  {
    auto res = this->inspectCache(key);
    // must exist in RAM
    ASSERT_NE(nullptr, res.first);
    ASSERT_EQ(::memcmp(res.first->getMemory(), val.data(), val.length()), 0);

    // must not be in nvmcache
    ASSERT_EQ(nullptr, res.second);
  }

  this->pushToNvmCacheFromRamForTesting(key);
  this->removeFromRamForTesting(key);

  {
    auto res = this->inspectCache(key);
    // must not exist in RAM
    ASSERT_EQ(nullptr, res.first);

    // must be in nvmcache
    ASSERT_NE(nullptr, res.second);
    ASSERT_EQ(::memcmp(res.second->getMemory(), val.data(), val.length()), 0);

    // we should not have brought anything into RAM.
    ASSERT_EQ(nullptr, this->inspectCache(key).first);
  }

  // remove from NVM
  this->removeFromNvmForTesting(key);
  {
    auto res = this->inspectCache(key);
    // must not exist in RAM
    ASSERT_EQ(nullptr, res.first);
    // must not be in nvmcache
    ASSERT_EQ(nullptr, res.second);
  }
}

// same as above, but uses large items using chained items
TEST_F(NvmCacheTest, InspectCacheLarge) {
  auto& config = this->getConfig();
  config.configureChainedItems();
  auto& cache = this->makeCache();
  auto pid = this->poolId();

  std::string key = "blah";
  const size_t allocSize = 19 * 1024;
  int nChained = 100;
  std::string val = genRandomStr(allocSize);
  {
    auto it = cache.allocate(pid, key, val.length());
    ASSERT_NE(nullptr, it);
    cache.insertOrReplace(it);
    ::memcpy(it->getMemory(), val.data(), val.length());
    for (int i = 0; i < nChained; i++) {
      auto chainedIt = cache.allocateChainedItem(it, val.length());
      ASSERT_TRUE(chainedIt);
      ::memcpy(chainedIt->getMemory(), val.data(), val.length());
      cache.addChainedItem(it, std::move(chainedIt));
    }
  }

  // item is only in RAM
  {
    auto res = this->inspectCache(key);
    // must exist in RAM
    ASSERT_NE(nullptr, res.first);
    ASSERT_EQ(::memcmp(res.first->getMemory(), val.data(), val.length()), 0);

    // must not be in nvmcache
    ASSERT_EQ(nullptr, res.second);
  }

  this->pushToNvmCacheFromRamForTesting(key);
  this->removeFromRamForTesting(key);

  {
    auto res = this->inspectCache(key);
    // must not exist in RAM
    ASSERT_EQ(nullptr, res.first);

    // must be in nvmcache
    ASSERT_NE(nullptr, res.second);
    ASSERT_EQ(::memcmp(res.second->getMemory(), val.data(), val.length()), 0);

    {
      auto allocs = cache.viewAsChainedAllocs(res.second);
      for (const auto& curr : allocs.getChain()) {
        ASSERT_EQ(0, ::memcmp(curr.getMemory(), val.data(), val.length()));
      }
    }

    // we should not have brought anything into RAM.
    ASSERT_EQ(nullptr, this->inspectCache(key).first);
  }

  // remove from NVM
  this->removeFromNvmForTesting(key);
  {
    auto res = this->inspectCache(key);
    // must not exist in RAM
    ASSERT_EQ(nullptr, res.first);
    // must not be in nvmcache
    ASSERT_EQ(nullptr, res.second);
  }
}

TEST_F(NvmCacheTest, WarmRoll) {
  this->convertToShmCache();
  std::string key = "blah";
  {
    auto& nvm = this->cache();
    auto pid = this->poolId();

    {
      auto it = nvm.allocate(pid, key, 100);
      nvm.insertOrReplace(it);
    }

    ASSERT_TRUE(this->checkKeyExists(key, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key, false /* ramOnly */));

    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key));
    this->removeFromRamForTesting(key);
    ASSERT_FALSE(this->checkKeyExists(key, true /* ramOnly */));
  }

  this->warmRoll();
  {
    auto res = this->inspectCache(key);
    // key was removed from ram before we warm rolled
    ASSERT_FALSE(res.first);

    // key is present in nvmcache
    ASSERT_TRUE(res.second);

    // fetch from nvmcache on warm roll
    ASSERT_TRUE(this->checkKeyExists(key, false /* ramOnly */));
  }
}

TEST_F(NvmCacheTest, ColdRoll) {
  this->convertToShmCache();
  std::string key = "blah";
  {
    auto& nvm = this->cache();
    auto pid = this->poolId();

    {
      auto it = nvm.allocate(pid, key, 100);
      nvm.insertOrReplace(it);
    }

    ASSERT_TRUE(this->checkKeyExists(key, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key, false /* ramOnly */));

    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key));
    ASSERT_TRUE(this->checkKeyExists(key, true /* ramOnly */));
  }

  this->coldRoll();
  {
    // we cold rolled
    ASSERT_FALSE(this->checkKeyExists(key, true /* ramOnly */));

    // fetch from nvmcache should succeed.
    ASSERT_TRUE(this->checkKeyExists(key, false /* ramOnly */));
  }
}

TEST_F(NvmCacheTest, ColdRollDropNvmCache) {
  this->getConfig().setDropNvmCacheOnShmNew(true);
  this->convertToShmCache();
  std::string key = "blah";
  {
    auto& nvm = this->cache();
    auto pid = this->poolId();

    {
      auto it = nvm.allocate(pid, key, 100);
      nvm.insertOrReplace(it);
    }

    ASSERT_TRUE(this->checkKeyExists(key, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key, false /* ramOnly */));

    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key));
    ASSERT_TRUE(this->checkKeyExists(key, true /* ramOnly */));
  }

  this->coldRoll();
  {
    // we cold rolled
    ASSERT_FALSE(this->checkKeyExists(key, true /* ramOnly */));

    // fetch from nvmcache should also fail
    ASSERT_FALSE(this->checkKeyExists(key, false /* ramOnly */));
  }
}

TEST_F(NvmCacheTest, IceRoll) {
  this->convertToShmCache();
  std::string key1 = "blah1";
  std::string key2 = "blah2";
  auto pid = this->poolId();
  {
    auto& nvm = this->cache();
    {
      auto it1 = nvm.allocate(pid, key1, 100);
      nvm.insertOrReplace(it1);
      auto it2 = nvm.allocate(pid, key2, 100);
      nvm.insertOrReplace(it2);
    }

    ASSERT_TRUE(this->checkKeyExists(key1, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key2, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key1, false /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key2, false /* ramOnly */));

    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key1));
    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key2));

    this->removeFromRamForTesting(key1);

    ASSERT_FALSE(this->checkKeyExists(key1, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key2, true /* ramOnly */));
  }

  this->iceRoll();
  {
    // we preserved memory but key1 was removed from ram.
    ASSERT_FALSE(this->checkKeyExists(key1, true /* ramOnly */));

    // key2 was still in ram.
    ASSERT_TRUE(this->checkKeyExists(key2, true /* ramOnly */));

    this->removeFromRamForTesting(key2);

    // fetch from nvmcache should fail for both
    ASSERT_FALSE(this->checkKeyExists(key2, true /* ramOnly */));
    ASSERT_FALSE(this->checkKeyExists(key2, false /* ramOnly */));

    auto& nvm = this->cache();
    auto it1 = nvm.allocate(pid, key1, 100);
    nvm.insertOrReplace(it1);
    auto it2 = nvm.allocate(pid, key2, 100);
    nvm.insertOrReplace(it2);
    // push key 2 to nvmcache again. we will warm roll and check if it exists
    // after an ice roll.
    ASSERT_TRUE(this->checkKeyExists(key1, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key2, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key1, false /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key2, false /* ramOnly */));

    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key1));
    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key2));
  }

  this->warmRoll();
  {
    // nvm is preserved subsequently
    ASSERT_TRUE(this->checkKeyExists(key1, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key2, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key1, false /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key2, false /* ramOnly */));
  }
}

TEST_F(NvmCacheTest, IceColdRoll) {
  this->convertToShmCache();
  std::string key1 = "blah1";
  std::string key2 = "blah2";
  {
    auto& nvm = this->cache();
    auto pid = this->poolId();

    {
      auto it1 = nvm.allocate(pid, key1, 100);
      nvm.insertOrReplace(it1);
      auto it2 = nvm.allocate(pid, key2, 100);
      nvm.insertOrReplace(it2);
    }

    ASSERT_TRUE(this->checkKeyExists(key1, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key2, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key1, false /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key2, false /* ramOnly */));

    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key1));
    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key2));

    this->removeFromRamForTesting(key1);

    ASSERT_FALSE(this->checkKeyExists(key1, true /* ramOnly */));
    ASSERT_TRUE(this->checkKeyExists(key2, true /* ramOnly */));
  }

  this->iceColdRoll();

  {
    // we lost memory
    ASSERT_FALSE(this->checkKeyExists(key1, true /* ramOnly */));
    ASSERT_FALSE(this->checkKeyExists(key2, true /* ramOnly */));

    // fetch from nvmcache should fail as well since we did ice-cold
    ASSERT_FALSE(this->checkKeyExists(key2, true /* ramOnly */));
    ASSERT_FALSE(this->checkKeyExists(key1, true /* ramOnly */));
  }
}

// this test assumes that by default, the config we use does not move on slab
// release.
TEST_F(NvmCacheTest, EvictSlabRelease) {
  auto& cache = this->cache();
  auto pid = this->poolId();

  // insert allocations into cache. Release some slabs corresponding to the
  // keys  and make sure that the keys are evicted to nvmcache.
  std::vector<std::string> keys;
  int nKeys = 100;
  // we have only one alloc size that is 20K in the config.
  uint32_t size = 1024 * 19;
  for (int i = 0; i < nKeys; i++) {
    std::string key = "key" + std::to_string(i);
    std::string val = "val" + std::to_string(i);
    auto handle = cache.allocate(pid, key, size);
    cache.insertOrReplace(handle);
    if (handle) {
      std::memcpy(handle->getMemory(), val.data(), val.size());
      keys.push_back(std::move(key));
    }
  }

  // everything must be in ram and nothing in nvmcache
  for (const auto& key : keys) {
    auto res = cache.inspectCache(key);
    ASSERT_NE(res.first, nullptr);
    ASSERT_EQ(res.second, nullptr);
  }

  ASSERT_EQ(0, this->evictionCount());
  // pick a key and see if it exists in RAM
  for (const auto& key : keys) {
    auto handle = this->fetch(key, true /* ramOnly */);
    if (handle) {
      void* mem = handle.get();
      handle.reset();
      this->releaseSlabFor(mem);
      ASSERT_FALSE(this->checkKeyExists(key, true /* ramOnly */));
    }
  }

  // everything must be in nvmcache.
  for (const auto& key : keys) {
    auto res = cache.inspectCache(key);
    ASSERT_EQ(res.first, nullptr);
    ASSERT_NE(res.second, nullptr);
  }
}

// allocate an item that has extra bytes in the end that we use and make sure
// that we save and restore that
TEST_F(NvmCacheTest, TrailingAllocSize) {
  auto& cache = this->cache();
  auto pid = this->poolId();

  const uint32_t allocSize = 15 * 1024 - 5;
  std::string key = "foobar";
  std::string text;
  {
    auto it = cache.allocate(pid, key, allocSize);
    ASSERT_NE(nullptr, it);
    const size_t extraBytes = cache.getUsableSize(*it) - allocSize;
    // scribble some random stuff into the trailing space
    ASSERT_GT(extraBytes, text.size());
    text = genRandomStr(extraBytes);
    std::memcpy(reinterpret_cast<char*>(it->getMemory()) + allocSize,
                text.data(),
                text.size());

    cache.insertOrReplace(it);
  }

  this->pushToNvmCacheFromRamForTesting(key);
  this->removeFromRamForTesting(key);

  auto it = this->fetch(key, false /* ramOnly */);
  ASSERT_EQ(
      0,
      std::memcmp(reinterpret_cast<const char*>(it->getMemory()) + allocSize,
                  text.data(),
                  text.size()));
}

TEST_F(NvmCacheTest, ChainedItems) {
  auto& config = this->getConfig();
  config.configureChainedItems();
  auto& cache = this->makeCache();
  auto pid = this->poolId();

  const uint32_t allocSize = 15 * 1024 - 5;
  const uint32_t nChained = folly::Random::rand32(2, 20);
  // int nChained = 2;
  std::string key = "foobar";
  std::vector<std::string> vals;
  {
    auto it = cache.allocate(pid, key, allocSize);
    ASSERT_NE(nullptr, it);

    auto fillItem = [&](Item& item) {
      size_t fullSize = cache.getUsableSize(item);
      const auto text = genRandomStr(fullSize);
      vals.push_back(text);
      std::memcpy(reinterpret_cast<char*>(item.getMemory()), text.data(),
                  text.size());
    };

    fillItem(*it);

    for (unsigned int i = 0; i < nChained; i++) {
      auto chainedIt =
          cache.allocateChainedItem(it, folly::Random::rand32(100, allocSize));
      ASSERT_TRUE(chainedIt);
      fillItem(*chainedIt);
      cache.addChainedItem(it, std::move(chainedIt));
    }

    ASSERT_EQ(vals.size(), nChained + 1);
    cache.insertOrReplace(it);
  }

  auto verifyItem = [&](const Item& item, const std::string& text) {
    ASSERT_EQ(cache.getUsableSize(item), text.size()) << item.toString();
    ASSERT_EQ(0, std::memcmp(item.getMemory(), text.data(), text.size()))
        << item.toString();
  };

  auto verifyChainedAllcos = [&](const WriteHandle& hdl) {
    auto allocs = cache.viewAsChainedAllocs(hdl);
    verifyItem(allocs.getParentItem(), vals[0]);

    int index = 0;
    for (const auto& c : allocs.getChain()) {
      verifyItem(c, vals[nChained - index++]);
    }
  };

  {
    auto it = this->fetch(key, true /* ramOnly*/);
    verifyChainedAllcos(it);
  }

  this->pushToNvmCacheFromRamForTesting(key);
  this->removeFromRamForTesting(key);

  auto it = this->fetch(key, false /* ramOnly */);
  ASSERT_TRUE(it);
  verifyChainedAllcos(it);
}

TEST_F(NvmCacheTest, ChainedItemsModifyAccessible) {
  auto& config = this->getConfig();
  config.configureChainedItems();
  auto& cache = this->makeCache();
  auto pid = this->poolId();

  const uint32_t allocSize = 15 * 1024 - 5;
  std::string key = "foobar";
  std::vector<std::string> vals;
  {
    auto it = cache.allocate(pid, key, allocSize);
    ASSERT_NE(nullptr, it);

    auto fillItem = [&](Item& item) {
      size_t fullSize = cache.getUsableSize(item);
      const auto text = genRandomStr(fullSize);
      vals.push_back(text);
      std::memcpy(reinterpret_cast<char*>(item.getMemory()), text.data(),
                  text.size());
    };

    fillItem(*it);
    cache.insertOrReplace(it);
    {
      auto chainedIt =
          cache.allocateChainedItem(it, folly::Random::rand32(100, allocSize));
      ASSERT_TRUE(chainedIt);
      fillItem(*chainedIt);
      cache.addChainedItem(it, std::move(chainedIt));
    }
    {
      this->pushToNvmCacheFromRamForTesting(key);
      this->removeFromRamForTesting(key);
    }
    // Read everything again
    {
      auto hdl = this->fetch(key, false /* ramOnly*/);
      hdl.wait();
      ASSERT_TRUE(hdl->isNvmClean());
      {
        auto chainedIt = cache.allocateChainedItem(
            hdl, folly::Random::rand32(100, allocSize));
        ASSERT_TRUE(chainedIt);
        fillItem(*chainedIt);
        cache.addChainedItem(hdl, std::move(chainedIt));
      }
      ASSERT_EQ(vals.size(), 3);
    }
    auto verifyItem = [&](const Item& item, const std::string& text) {
      ASSERT_EQ(cache.getUsableSize(item), text.size()) << item.toString();
      ASSERT_EQ(0, std::memcmp(item.getMemory(), text.data(), text.size()))
          << item.toString();
    };

    auto verifyChainedAllcos = [&](const ReadHandle& hdl, uint32_t nChained) {
      auto allocs = cache.viewAsChainedAllocs(hdl);
      verifyItem(allocs.getParentItem(), vals[0]);

      int index = 0;
      for (const auto& c : allocs.getChain()) {
        verifyItem(c, vals[nChained - index++]);
      }
    };
    {
      auto res = this->inspectCache(key);
      EXPECT_NE(nullptr, res.first);
      verifyChainedAllcos(res.first, 2);
      if (nullptr != res.second) {
        verifyChainedAllcos(res.second, 2);
      }
    }

    // popChained Item test
    {
      this->pushToNvmCacheFromRamForTesting(key);
      this->removeFromRamForTesting(key);
    }
    // Read everything again
    {
      auto hdl = this->fetch(key, false /* ramOnly*/);
      hdl.wait();
      ASSERT_TRUE(hdl->isNvmClean());
      {
        auto chainedIt = cache.popChainedItem(hdl);
        ASSERT_TRUE(chainedIt);
        vals.pop_back();
      }
      ASSERT_EQ(vals.size(), 2);
    }

    {
      auto res = this->inspectCache(key);
      EXPECT_NE(nullptr, res.first);
      verifyChainedAllcos(res.first, 1);
      if (nullptr != res.second) {
        verifyChainedAllcos(res.second, 1);
      }
    }

    // replaceChained Item test
    {
      this->pushToNvmCacheFromRamForTesting(key);
      this->removeFromRamForTesting(key);
    }

    // Read everything again
    {
      auto hdl = this->fetchToWrite(key, false /* ramOnly*/);
      hdl.wait();
      ASSERT_FALSE(hdl->isNvmClean());

      vals.pop_back();
      auto newItemHandle =
          cache.allocateChainedItem(hdl, folly::Random::rand32(100, allocSize));
      ASSERT_TRUE(newItemHandle);
      fillItem(*newItemHandle);

      {
        auto* firstChainedItem =
            cache.viewAsWritableChainedAllocs(hdl).getNthInChain(0);
        Item& oldItem = *firstChainedItem;
        auto oldHandle =
            cache.replaceChainedItem(oldItem, std::move(newItemHandle), *hdl);
        ASSERT_TRUE(oldHandle);
      }
      ASSERT_EQ(vals.size(), 2);
    }
    {
      auto res = this->inspectCache(key);
      EXPECT_NE(nullptr, res.first);
      verifyChainedAllcos(res.first, 1);
      if (nullptr != res.second) {
        verifyChainedAllcos(res.second, 1);
      }
    }
  }
}

TEST_F(NvmCacheTest, EncodeDecode) {
  auto& config = this->getConfig();
  config.configureChainedItems();
  std::unordered_map<std::string, int> callbacks;
  std::string failKey = "failure";
  config.setNvmCacheEncodeCallback(
      [&](typename AllocatorT::NvmCacheT::EncodeDecodeContext ctx) mutable {
        auto& it = ctx.item;
        auto& cnt = callbacks[it.getKey().str()];
        ++cnt;

        if (it.getKey() == failKey) {
          return false;
        }

        for (const Item& item : ctx.chainedItemRange) {
          (void)item;
          ++cnt;
        }
        return true;
      });
  config.setNvmCacheDecodeCallback(
      [&](typename AllocatorT::NvmCacheT::EncodeDecodeContext ctx) mutable {
        auto& it = ctx.item;
        auto& cnt = callbacks[it.getKey().str()];
        --cnt;

        for (const Item& item : ctx.chainedItemRange) {
          (void)item;
          --cnt;
        }
      });

  auto& cache = this->makeCache();
  auto pid = this->poolId();

  const uint32_t allocSize = 15 * 1024 - 5;
  // chained item
  {
    const uint32_t nChained = folly::Random::rand32(2, 20);
    std::string key = "chained";
    {
      auto it = cache.allocate(pid, key, allocSize);
      ASSERT_NE(nullptr, it);

      for (unsigned int i = 0; i < nChained; i++) {
        auto chainedIt = cache.allocateChainedItem(
            it, folly::Random::rand32(100, allocSize));
        *reinterpret_cast<int*>(chainedIt->getMemory()) = i;
        ASSERT_TRUE(chainedIt);
        cache.addChainedItem(it, std::move(chainedIt));
      }

      cache.insertOrReplace(it);
    }

    this->pushToNvmCacheFromRamForTesting(key);
    ASSERT_EQ(nChained + 1, callbacks[key]);
    this->removeFromRamForTesting(key);

    {
      auto it = this->fetch(key, true /* ramOnly */);
      ASSERT_TRUE(!it);
      ASSERT_EQ(nChained + 1, callbacks[key]);
    }
    {
      auto it = this->fetch(key, false /* ramOnly */);
      ASSERT_TRUE(it);
      ASSERT_EQ(0, callbacks[key]);
      auto allocs = cache.viewAsChainedAllocs(it);

      // verify the order and content of chain
      int i = nChained;
      for (const auto& c : allocs.getChain()) {
        ASSERT_EQ(i - 1, *reinterpret_cast<const int*>(c.getMemory()));
        i--;
      }
    }
  }

  // regular non chained item
  {
    std::string key = "regular";
    {
      auto it = cache.allocate(pid, key, allocSize);
      ASSERT_NE(nullptr, it);
      cache.insertOrReplace(it);
    }

    this->pushToNvmCacheFromRamForTesting(key);
    ASSERT_EQ(1, callbacks[key]);
    this->removeFromRamForTesting(key);

    {
      auto it = this->fetch(key, true /* ramOnly */);
      ASSERT_TRUE(!it);
      ASSERT_EQ(1, callbacks[key]);
    }
    {
      auto it = this->fetch(key, false /* ramOnly */);
      ASSERT_TRUE(it);
      ASSERT_EQ(0, callbacks[key]);
    }
  }

  // failure to encode a regular item
  {
    {
      auto it = cache.allocate(pid, failKey, allocSize);
      ASSERT_NE(nullptr, it);
      cache.insertOrReplace(it);
    }

    this->pushToNvmCacheFromRamForTesting(failKey);
    ASSERT_EQ(1, callbacks[failKey]);
    this->removeFromRamForTesting(failKey);

    {
      auto it = this->fetch(failKey, true /* ramOnly */);
      ASSERT_TRUE(!it);
      ASSERT_EQ(1, callbacks[failKey]);
    }
    {
      auto it = this->fetch(failKey, false /* ramOnly */);
      ASSERT_TRUE(!it);
      ASSERT_EQ(1, callbacks[failKey]);
    }
  }

  // failure to encode a chained item
  {
    {
      auto it = cache.allocate(pid, failKey, allocSize);
      ASSERT_NE(nullptr, it);
      int nChained = 10;
      for (int i = 0; i < nChained; i++) {
        auto chainedIt = cache.allocateChainedItem(
            it, folly::Random::rand32(100, allocSize));
        ASSERT_TRUE(chainedIt);
        cache.addChainedItem(it, std::move(chainedIt));
      }
      cache.insertOrReplace(it);
    }

    this->pushToNvmCacheFromRamForTesting(failKey);
    ASSERT_EQ(2, callbacks[failKey]);
    this->removeFromRamForTesting(failKey);

    {
      auto it = this->fetch(failKey, true /* ramOnly */);
      ASSERT_TRUE(!it);
      ASSERT_EQ(2, callbacks[failKey]);
    }
    {
      auto it = this->fetch(failKey, false /* ramOnly */);
      ASSERT_TRUE(!it);
      ASSERT_EQ(2, callbacks[failKey]);
    }
  }
}

TEST_F(NvmCacheTest, NvmUptime) {
  unsigned int time = 6;
  this->convertToShmCache();
  {
    /* sleep override */ std::this_thread::sleep_for(
        std::chrono::seconds(time));
    ASSERT_GE(this->getStats().nvmUpTime, time);
  }

  this->warmRoll();
  {
    // uptime must  be preserved
    /* sleep override */ std::this_thread::sleep_for(
        std::chrono::seconds(time));
    ASSERT_GE(this->getStats().nvmUpTime, 2 * time);
  }

  this->coldRoll();
  {
    // uptime must  be preserved
    /* sleep override */ std::this_thread::sleep_for(
        std::chrono::seconds(time));
    ASSERT_GE(this->getStats().nvmUpTime, 3 * time);
  }

  this->iceColdRoll();
  {
    // uptime must  be reset
    ASSERT_LE(this->getStats().nvmUpTime, time);
    /* sleep override */ std::this_thread::sleep_for(
        std::chrono::seconds(time));
    ASSERT_GE(this->getStats().nvmUpTime, time);
    ASSERT_LE(this->getStats().nvmUpTime, 2 * time);
  }

  {
    auto& config = this->getConfig();
    // empty cache dir means no persistency
    config.enableCachePersistence("");
    this->makeCache();
    /* sleep override */ std::this_thread::sleep_for(
        std::chrono::seconds(time));
    ASSERT_GE(this->getStats().nvmUpTime, time);
  }
}

TEST_F(NvmCacheTest, FullAllocSize) {
  // Test truncated alloc sizes
  auto& config = this->getConfig();
  config.nvmConfig->truncateItemToOriginalAllocSizeInNvm = false;
  this->poolAllocsizes_ = {200};
  auto& cache = this->makeCache();
  auto pid = this->poolId();

  // Allocate a small item but use its extra bytes
  uint32_t totalSize = 0;
  {
    auto it = cache.allocate(pid, "test", 1);
    ASSERT_NE(nullptr, it);
    ASSERT_LT(it->getSize(), cache.getUsableSize(*it));

    totalSize = cache.getUsableSize(*it);
    for (uint32_t i = 0; i < totalSize; ++i) {
      it->template getMemoryAs<char>()[i] = static_cast<char>(i);
    }

    cache.insertOrReplace(it);
    this->pushToNvmCacheFromRamForTesting("test");
    this->removeFromRamForTesting("test");
  }
  {
    // Make sure we end up with a different item in free list
    auto it = cache.allocate(pid, "placeholder", 1);
    ASAN_UNPOISON_MEMORY_REGION(it->getMemory(), totalSize);
    for (uint32_t i = 0; i < totalSize; ++i) {
      it->template getMemoryAs<char>()[i] = 0;
    }
  }

  {
    auto it = cache.find("test");
    ASSERT_NE(nullptr, it);
    ASSERT_EQ(totalSize, cache.getUsableSize(*it));
    EXPECT_EQ(0, it->template getMemoryAs<char>()[0]);
    for (uint32_t i = 1; i < totalSize; ++i) {
      EXPECT_EQ(static_cast<char>(i), it->template getMemoryAs<char>()[i])
          << "i: " << i;
    }
  }
}

TEST_F(NvmCacheTest, TruncatedAllocSize) {
  // Test truncated alloc sizes
  auto& config = this->getConfig();
  config.nvmConfig->truncateItemToOriginalAllocSizeInNvm = true;
  this->poolAllocsizes_ = {200};
  auto& cache = this->makeCache();
  auto pid = this->poolId();

  // We use 101 bytes for value size because it's just bigger than
  // the small item threshold (100 bytes) we set up for NvmCache.
  // This ensures we won't evict anything prematurely in flash device.
  const uint32_t valSize = 101;

  // Allocate a small item but use its extra bytes
  uint32_t totalSize = 0;
  {
    auto it = cache.allocate(pid, "test", valSize);
    ASSERT_NE(nullptr, it);
    ASSERT_LT(it->getSize(), cache.getUsableSize(*it));

    totalSize = cache.getUsableSize(*it);
    for (uint32_t i = 0; i < totalSize; ++i) {
      it->template getMemoryAs<char>()[i] = static_cast<char>(i);
    }

    cache.insertOrReplace(it);
    this->pushToNvmCacheFromRamForTesting("test");
    this->removeFromRamForTesting("test");
  }
  {
    // Make sure we end up with a different item in free list
    auto it = cache.allocate(pid, "placeholder", valSize);
    ASAN_UNPOISON_MEMORY_REGION(it->getMemory(), totalSize);
    for (uint32_t i = 0; i < totalSize; ++i) {
      it->template getMemoryAs<char>()[i] = 0;
    }
  }

  {
    auto it = cache.find("test");
    ASSERT_NE(nullptr, it);
    ASSERT_EQ(totalSize, cache.getUsableSize(*it));
    EXPECT_EQ(0, it->template getMemoryAs<char>()[0]);
    for (uint32_t i = valSize; i < totalSize; ++i) {
      EXPECT_NE(static_cast<char>(i), it->template getMemoryAs<char>()[i])
          << "i: " << i;
    }
  }
}

TEST_F(NvmCacheTest, NavyStats) {
  // Ensure we export all the stats we expect
  // Everytime we add a new stat, make sure to update this test accordingly
  auto nvmStats = this->cache().getNvmCacheStatsMap().toMap();

  auto cs = [&nvmStats](const std::string& name) mutable {
    if (nvmStats.end() != nvmStats.find(name)) {
      nvmStats.erase(name);
      return true;
    }
    return false;
  };

  // navy::Driver
  EXPECT_TRUE(cs("navy_total_usable_size"));
  EXPECT_TRUE(cs("navy_inserts"));
  EXPECT_TRUE(cs("navy_succ_inserts"));
  EXPECT_TRUE(cs("navy_lookups"));
  EXPECT_TRUE(cs("navy_succ_lookups"));
  EXPECT_TRUE(cs("navy_removes"));
  EXPECT_TRUE(cs("navy_succ_removes"));
  EXPECT_TRUE(cs("navy_rejected"));
  EXPECT_TRUE(cs("navy_rejected_concurrent_inserts"));
  EXPECT_TRUE(cs("navy_rejected_parcel_memory"));
  EXPECT_TRUE(cs("navy_rejected_bytes"));
  EXPECT_TRUE(cs("navy_io_errors"));
  EXPECT_TRUE(cs("navy_parcel_memory"));
  EXPECT_TRUE(cs("navy_concurrent_inserts"));
  EXPECT_TRUE(cs("navy_accepted"));
  EXPECT_TRUE(cs("navy_accepted_bytes"));
  EXPECT_TRUE(cs("navy_metadata_size_bytes"));
  EXPECT_TRUE(cs("navy_metadata_estimated_bytes"));
  EXPECT_TRUE(cs("navy_recover_time_ms"));
  EXPECT_TRUE(cs("navy_metadata_recovered_bytes"));

  // CacheAllocator
  EXPECT_TRUE(cs("start_truncated"));
  EXPECT_TRUE(cs("start_truncated_no_cache_dir"));
  EXPECT_TRUE(cs("start_truncated_no_usable_state"));
  EXPECT_TRUE(cs("start_truncated_dram_cache_new"));
  EXPECT_TRUE(cs("navy_persist_time_ms"));

  // navy::OrderedThreadPoolJobScheduler
  EXPECT_TRUE(cs("navy_reader_pool_max_queue_len"));
  EXPECT_TRUE(cs("navy_reader_pool_reschedules"));
  EXPECT_TRUE(cs("navy_reader_pool_jobs_high_reschedule"));
  EXPECT_TRUE(cs("navy_reader_pool_jobs_done"));
  EXPECT_TRUE(cs("navy_max_reader_pool_pending_jobs"));
  EXPECT_TRUE(cs("navy_writer_pool_max_queue_len"));
  EXPECT_TRUE(cs("navy_writer_pool_reschedules"));
  EXPECT_TRUE(cs("navy_writer_pool_jobs_high_reschedule"));
  EXPECT_TRUE(cs("navy_writer_pool_jobs_done"));
  EXPECT_TRUE(cs("navy_max_writer_pool_pending_jobs"));
  EXPECT_TRUE(cs("navy_req_order_spooled"));
  EXPECT_TRUE(cs("navy_req_order_curr_spool_size"));

  // navy::BlockCache
  EXPECT_TRUE(cs("navy_bc_size"));
  EXPECT_TRUE(cs("navy_bc_item_removed_with_no_access"));
  EXPECT_TRUE(cs("navy_bc_item_hits_avg"));
  EXPECT_TRUE(cs("navy_bc_item_hits_min"));
  EXPECT_TRUE(cs("navy_bc_item_hits_p5"));
  EXPECT_TRUE(cs("navy_bc_item_hits_p10"));
  EXPECT_TRUE(cs("navy_bc_item_hits_p25"));
  EXPECT_TRUE(cs("navy_bc_item_hits_p50"));
  EXPECT_TRUE(cs("navy_bc_item_hits_p75"));
  EXPECT_TRUE(cs("navy_bc_item_hits_p90"));
  EXPECT_TRUE(cs("navy_bc_item_hits_p95"));
  EXPECT_TRUE(cs("navy_bc_item_hits_p99"));
  EXPECT_TRUE(cs("navy_bc_item_hits_p999"));
  EXPECT_TRUE(cs("navy_bc_item_hits_p9999"));
  EXPECT_TRUE(cs("navy_bc_item_hits_p99999"));
  EXPECT_TRUE(cs("navy_bc_item_hits_p999999"));
  EXPECT_TRUE(cs("navy_bc_item_hits_max"));
  EXPECT_TRUE(cs("navy_bc_items"));
  EXPECT_TRUE(cs("navy_bc_metadata_est_bytes"));
  EXPECT_TRUE(cs("navy_bc_inserts"));
  EXPECT_TRUE(cs("navy_bc_insert_hash_collisions"));
  EXPECT_TRUE(cs("navy_bc_succ_inserts"));
  EXPECT_TRUE(cs("navy_bc_lookups"));
  EXPECT_TRUE(cs("navy_bc_lookup_false_positives"));
  EXPECT_TRUE(cs("navy_bc_lookup_entry_header_checksum_errors"));
  EXPECT_TRUE(cs("navy_bc_lookup_value_checksum_errors"));
  EXPECT_TRUE(cs("navy_bc_succ_lookups"));
  EXPECT_TRUE(cs("navy_bc_retry_reads"));
  EXPECT_TRUE(cs("navy_bc_removes"));
  EXPECT_TRUE(cs("navy_bc_num_regions"));
  EXPECT_TRUE(cs("navy_bc_num_clean_regions"));
  EXPECT_TRUE(cs("navy_bc_num_clean_region_retries"));
  EXPECT_TRUE(cs("navy_bc_succ_removes"));
  EXPECT_TRUE(cs("navy_bc_eviction_lookup_misses"));
  EXPECT_TRUE(cs("navy_bc_alloc_errors"));
  EXPECT_TRUE(cs("navy_bc_alloc_retries"));
  EXPECT_TRUE(cs("navy_bc_alloc_retries_waits"));
  EXPECT_TRUE(cs("navy_bc_logical_written"));
  EXPECT_TRUE(cs("navy_bc_serialized_written"));
  EXPECT_TRUE(cs("navy_bc_alignment_padding_written"));
  EXPECT_TRUE(cs("navy_bc_hole_count"));
  EXPECT_TRUE(cs("navy_bc_hole_bytes"));
  EXPECT_TRUE(cs("navy_bc_used_size_bytes"));
  EXPECT_TRUE(cs("navy_bc_reinsertions"));
  EXPECT_TRUE(cs("navy_bc_reinsertion_bytes"));
  EXPECT_TRUE(cs("navy_bc_reinsertion_errors"));
  EXPECT_TRUE(cs("navy_bc_excessive_read_bytes"));
  EXPECT_TRUE(cs("navy_bc_lookup_for_item_destructor_errors"));
  EXPECT_TRUE(cs("navy_bc_reclaim_entry_header_checksum_errors"));
  EXPECT_TRUE(cs("navy_bc_reclaim_value_checksum_errors"));
  EXPECT_TRUE(cs("navy_bc_cleanup_entry_header_checksum_errors"));
  EXPECT_TRUE(cs("navy_bc_cleanup_value_checksum_errors"));
  EXPECT_TRUE(cs("navy_bc_remove_attempt_collisions"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_avg"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_min"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_max"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_p5"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_p10"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_p25"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_p50"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_p75"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_p90"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_p95"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_p99"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_p999"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_p9999"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_p99999"));
  EXPECT_TRUE(cs("navy_bc_item_lifetime_secs_p999999"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_avg"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_min"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_max"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_p5"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_p10"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_p25"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_p50"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_p75"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_p90"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_p95"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_p99"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_p999"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_p9999"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_p99999"));
  EXPECT_TRUE(cs("navy_bc_insert_latency_us_p999999"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_avg"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_min"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_max"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_p5"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_p10"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_p25"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_p50"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_p75"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_p90"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_p95"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_p99"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_p999"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_p9999"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_p99999"));
  EXPECT_TRUE(cs("navy_bc_lookup_latency_us_p999999"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_avg"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_min"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_max"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_p5"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_p10"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_p25"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_p50"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_p75"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_p90"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_p95"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_p99"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_p999"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_p9999"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_p99999"));
  EXPECT_TRUE(cs("navy_bc_remove_latency_us_p999999"));

  // navy::RegionManager
  EXPECT_TRUE(cs("navy_bc_reclaim"));
  EXPECT_TRUE(cs("navy_bc_reclaim_time"));
  EXPECT_TRUE(cs("navy_bc_region_reclaim_errors"));
  EXPECT_TRUE(cs("navy_bc_read_during_reclaim"));
  EXPECT_TRUE(cs("navy_bc_evictions"));
  EXPECT_TRUE(cs("navy_bc_evictions_expired"));
  EXPECT_TRUE(cs("navy_bc_physical_written"));
  EXPECT_TRUE(cs("navy_bc_external_fragmentation"));
  EXPECT_TRUE(cs("navy_bc_inmem_waiting_flush"));
  EXPECT_TRUE(cs("navy_bc_inmem_active"));
  EXPECT_TRUE(cs("navy_bc_inmem_flush_retries"));
  EXPECT_TRUE(cs("navy_bc_inmem_flush_failures"));

  // navy::LruPolicy
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_avg"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_min"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_p5"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_p10"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_p25"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_p50"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_p75"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_p90"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_p95"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_p99"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_p999"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_p9999"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_p99999"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_p999999"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_insertion_max"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_avg"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_min"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_p5"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_p10"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_p25"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_p50"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_p75"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_p90"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_p95"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_p99"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_p999"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_p9999"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_p99999"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_p999999"));
  EXPECT_TRUE(cs("navy_bc_lru_secs_since_access_max"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_avg"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_min"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_p5"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_p10"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_p25"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_p50"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_p75"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_p90"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_p95"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_p99"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_p999"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_p9999"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_p99999"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_p999999"));
  EXPECT_TRUE(cs("navy_bc_lru_region_hits_estimate_max"));

  // navy::BigHash
  EXPECT_TRUE(cs("navy_bh_size"));
  EXPECT_TRUE(cs("navy_bh_items"));
  EXPECT_TRUE(cs("navy_bh_inserts"));
  EXPECT_TRUE(cs("navy_bh_succ_inserts"));
  EXPECT_TRUE(cs("navy_bh_lookups"));
  EXPECT_TRUE(cs("navy_bh_succ_lookups"));
  EXPECT_TRUE(cs("navy_bh_removes"));
  EXPECT_TRUE(cs("navy_bh_succ_removes"));
  EXPECT_TRUE(cs("navy_bh_evictions"));
  EXPECT_TRUE(cs("navy_bh_evictions_expired"));
  EXPECT_TRUE(cs("navy_bh_logical_written"));
  EXPECT_TRUE(cs("navy_bh_physical_written"));
  EXPECT_TRUE(cs("navy_bh_io_errors"));
  EXPECT_TRUE(cs("navy_bh_bf_false_positive_pct"));
  EXPECT_TRUE(cs("navy_bh_bf_lookups"));
  EXPECT_TRUE(cs("navy_bh_bf_rebuilds"));
  EXPECT_TRUE(cs("navy_bh_checksum_errors"));
  EXPECT_TRUE(cs("navy_bh_used_size_bytes"));
  EXPECT_TRUE(cs("navy_bh_metadata_est_bytes"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_avg"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_min"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_max"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_p5"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_p10"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_p25"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_p50"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_p75"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_p90"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_p95"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_p99"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_p999"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_p9999"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_p99999"));
  EXPECT_TRUE(cs("navy_bh_expired_loop_x100_p999999"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_avg"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_min"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_max"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_p5"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_p10"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_p25"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_p50"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_p75"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_p90"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_p95"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_p99"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_p999"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_p9999"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_p99999"));
  EXPECT_TRUE(cs("navy_bh_item_lifetime_secs_p999999"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_avg"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_min"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_max"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_p5"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_p10"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_p25"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_p50"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_p75"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_p90"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_p95"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_p99"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_p999"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_p9999"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_p99999"));
  EXPECT_TRUE(cs("navy_bh_lookup_latency_us_p999999"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_avg"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_min"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_max"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_p5"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_p10"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_p25"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_p50"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_p75"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_p90"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_p95"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_p99"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_p999"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_p9999"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_p99999"));
  EXPECT_TRUE(cs("navy_bh_insert_latency_us_p999999"));
  EXPECT_TRUE(cs("navy_bh_disabled_bucket_lookup"));
  EXPECT_TRUE(cs("navy_bh_disabled_bucket_insert"));
  EXPECT_TRUE(cs("navy_bh_disabled_bucket_remove"));
  EXPECT_TRUE(cs("navy_bh_disabled_buckets"));

  // navy::Device
  EXPECT_TRUE(cs("navy_device_bytes_written"));
  EXPECT_TRUE(cs("navy_device_bytes_read"));
  EXPECT_TRUE(cs("navy_device_read_errors"));
  EXPECT_TRUE(cs("navy_device_write_errors"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_avg"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_min"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_max"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_p5"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_p10"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_p25"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_p50"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_p75"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_p90"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_p95"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_p99"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_p999"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_p9999"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_p99999"));
  EXPECT_TRUE(cs("navy_device_read_latency_us_p999999"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_avg"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_min"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_max"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_p5"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_p10"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_p25"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_p50"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_p75"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_p90"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_p95"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_p99"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_p999"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_p9999"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_p99999"));
  EXPECT_TRUE(cs("navy_device_write_latency_us_p999999"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_avg"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_min"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_max"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_p5"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_p10"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_p25"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_p50"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_p75"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_p90"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_p95"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_p99"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_p999"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_p9999"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_p99999"));
  EXPECT_TRUE(cs("navy_device_async_io_op_read_device_latency_us_p999999"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_avg"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_min"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_max"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_p5"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_p10"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_p25"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_p50"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_p75"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_p90"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_p95"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_p99"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_p999"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_p9999"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_p99999"));
  EXPECT_TRUE(cs("navy_device_async_io_op_write_device_latency_us_p999999"));

  EXPECT_TRUE(cs("navy_device_encryption_errors"));
  EXPECT_TRUE(cs("navy_device_decryption_errors"));

  // item destructor
  EXPECT_TRUE(cs("items_tracked_for_destructor"));

  // AccessTimeMap stats
  EXPECT_TRUE(cs("navy_atm_sets"));
  EXPECT_TRUE(cs("navy_atm_gets"));
  EXPECT_TRUE(cs("navy_atm_get_and_removes"));
  EXPECT_TRUE(cs("navy_atm_removes"));
  EXPECT_TRUE(cs("navy_atm_hits"));
  EXPECT_TRUE(cs("navy_atm_misses"));
  EXPECT_TRUE(cs("navy_atm_evictions"));
  EXPECT_TRUE(cs("navy_atm_size"));

  // there should be no additional stats
  if (nvmStats.size()) {
    for (auto kv : nvmStats) {
      XLOG(ERR) << kv.first << ", " << kv.second;
    }
  }
  EXPECT_EQ(0, nvmStats.size());
}

TEST_F(NvmCacheTest, Raid0Basic) {
  auto& config = getConfig();
  auto& navyConfig = config.nvmConfig->navyConfig;
  auto filePath = fmt::format("/tmp/nvmcache-navy-raid0/{}", ::getpid());
  util::makeDir(filePath);
  SCOPE_EXIT { util::removePath(filePath); };

  std::vector<std::string> vec = {filePath + "/CACHE0", filePath + "/CACHE1",
                                  filePath + "/CACHE2", filePath + "/CACHE3"};
  navyConfig.setSimpleFile("", 0);
  navyConfig.setRaidFiles(vec, 10 * 1024 * 1024);
  this->convertToShmCache();
  auto& nvm = this->cache();
  auto pid = this->poolId();
  std::string key = "blah";
  std::string val = "foobar";
  {
    auto it = nvm.allocate(pid, key, val.length());
    ASSERT_NE(nullptr, it);
    ::memcpy(it->getMemory(), val.data(), val.length());
    nvm.insertOrReplace(it);
  }

  // item is only in RAM
  {
    auto res = this->inspectCache(key);
    // must exist in RAM
    ASSERT_NE(nullptr, res.first);
    ASSERT_EQ(::memcmp(res.first->getMemory(), val.data(), val.length()), 0);

    // must not be in nvmcache
    ASSERT_EQ(nullptr, res.second);
  }

  this->pushToNvmCacheFromRamForTesting(key);
  this->removeFromRamForTesting(key);

  {
    auto res = this->inspectCache(key);
    // must not exist in RAM
    ASSERT_EQ(nullptr, res.first);

    // must be in nvmcache
    ASSERT_NE(nullptr, res.second);
    ASSERT_EQ(::memcmp(res.second->getMemory(), val.data(), val.length()), 0);

    // we should not have brought anything into RAM.
    ASSERT_EQ(nullptr, this->inspectCache(key).first);
  }

  // recovery should find the key/val
  this->warmRoll();
  {
    auto res = this->inspectCache(key);
    // must not exist in RAM
    ASSERT_EQ(nullptr, res.first);

    // must be in nvmcache
    ASSERT_NE(nullptr, res.second);
    ASSERT_EQ(::memcmp(res.second->getMemory(), val.data(), val.length()), 0);

    // we should not have brought anything into RAM.
    ASSERT_EQ(nullptr, this->inspectCache(key).first);
  }
}

TEST_F(NvmCacheTest, Raid0OrderChange) {
  auto& config = getConfig();
  auto& navyConfig = config.nvmConfig->navyConfig;
  auto filePath = fmt::format("/tmp/nvmcache-navy-raid0/{}", ::getpid());
  util::makeDir(filePath);
  SCOPE_EXIT { util::removePath(filePath); };

  navyConfig.setSimpleFile("", 0);
  std::vector<std::string> vec = {filePath + "/CACHE0", filePath + "/CACHE1",
                                  filePath + "/CACHE2", filePath + "/CACHE3"};
  navyConfig.setRaidFiles(vec, 10 * 1024 * 1024);

  // setup a cache with some content and change the raid0 order and verify
  // that everything is correct.
  std::string val = "foobar";
  int nKeys = 100;
  auto makeKey = [&](int i) { return fmt::format("blah-{}", i); };

  this->convertToShmCache();
  {
    auto& nvm = this->cache();
    auto pid = this->poolId();

    for (int i = 0; i < nKeys; i++) {
      auto it = nvm.allocate(pid, makeKey(i), val.length());
      ASSERT_NE(nullptr, it);
      ::memcpy(it->getMemory(), val.data(), val.length());
      nvm.insertOrReplace(it);
    }
    nvm.flushNvmCache();

    // item is only in RAM
    for (int i = 0; i < nKeys; i++) {
      auto res = this->inspectCache(makeKey(i));
      // must exist in RAM
      ASSERT_NE(nullptr, res.first);
      ASSERT_EQ(::memcmp(res.first->getMemory(), val.data(), val.length()), 0);

      // must not be in nvmcache
      ASSERT_EQ(nullptr, res.second);
      this->pushToNvmCacheFromRamForTesting(makeKey(i), false);
      this->removeFromRamForTesting(makeKey(i));
    }
    nvm.flushNvmCache();

    for (int i = 0; i < nKeys; i++) {
      auto res = this->inspectCache(makeKey(i));
      // must not exist in RAM
      ASSERT_EQ(nullptr, res.first);

      // must be in nvmcache
      ASSERT_NE(nullptr, res.second);
      ASSERT_EQ(::memcmp(res.second->getMemory(), val.data(), val.length()), 0);
    }
  }

  // change the order of files
  vec = {filePath + "/CACHE3", filePath + "/CACHE1", filePath + "/CACHE2",
         filePath + "/CACHE0"};
  navyConfig.setRaidFiles(vec, 10 * 1024 * 1024);

  this->warmRoll();
  // recovery should succeed and we must find those item in nvmcache.
  for (int i = 0; i < nKeys; i++) {
    auto res = this->inspectCache(makeKey(i));
    // must not exist in RAM
    ASSERT_EQ(nullptr, res.first);

    // must be in nvmcache
    ASSERT_NE(nullptr, res.second);
    ASSERT_EQ(::memcmp(res.second->getMemory(), val.data(), val.length()), 0);
  }
}

TEST_F(NvmCacheTest, Raid0NumFilesChange) {
  auto& config = getConfig();
  auto& navyConfig = config.nvmConfig->navyConfig;
  auto filePath = fmt::format("/tmp/nvmcache-navy-raid0/{}", ::getpid());
  util::makeDir(filePath);
  SCOPE_EXIT { util::removePath(filePath); };

  std::vector<std::string> vec = {filePath + "/CACHE0", filePath + "/CACHE1",
                                  filePath + "/CACHE2", filePath + "/CACHE3"};
  navyConfig.setSimpleFile("", 0);
  navyConfig.setRaidFiles(vec, 10 * 1024 * 1024);

  // setup a cache with some content and change the raid0 order and verify
  // that everything is correct.
  std::string val = "foobar";
  int nKeys = 100;
  auto makeKey = [&](int i) { return fmt::format("blah-{}", i); };

  this->convertToShmCache();
  {
    auto& nvm = this->cache();
    auto pid = this->poolId();

    for (int i = 0; i < nKeys; i++) {
      auto it = nvm.allocate(pid, makeKey(i), val.length());
      ASSERT_NE(nullptr, it);
      ::memcpy(it->getMemory(), val.data(), val.length());
      nvm.insertOrReplace(it);
    }
    nvm.flushNvmCache();

    // item is only in RAM
    for (int i = 0; i < nKeys; i++) {
      auto res = this->inspectCache(makeKey(i));
      // must exist in RAM
      ASSERT_NE(nullptr, res.first);
      ASSERT_EQ(::memcmp(res.first->getMemory(), val.data(), val.length()), 0);

      // must not be in nvmcache
      ASSERT_EQ(nullptr, res.second);
      this->pushToNvmCacheFromRamForTesting(makeKey(i), false);
      this->removeFromRamForTesting(makeKey(i));
    }
    nvm.flushNvmCache();

    for (int i = 0; i < nKeys; i++) {
      auto res = this->inspectCache(makeKey(i));
      // must not exist in RAM
      ASSERT_EQ(nullptr, res.first);

      // must be in nvmcache
      ASSERT_NE(nullptr, res.second);
      ASSERT_EQ(::memcmp(res.second->getMemory(), val.data(), val.length()), 0);
    }
  }

  vec = {filePath + "/CACHE0", filePath + "/CACHE2", filePath + "/CACHE3"};
  navyConfig.setRaidFiles(vec, 10 * 1024 * 1024);
  this->warmRoll();
  // recovery should fail and we should lose the previous content. nvmcache
  // should still be enabled
  //
  EXPECT_TRUE(this->cache().isNvmCacheEnabled());
  for (int i = 0; i < nKeys; i++) {
    auto res = this->inspectCache(makeKey(i));
    // must not exist in RAM
    ASSERT_EQ(nullptr, res.first);

    // must not be in nvmcache since it got dropped
    ASSERT_EQ(nullptr, res.second);
  }
}

TEST_F(NvmCacheTest, Raid0SizeChange) {
  auto& config = getConfig();
  auto& navyConfig = config.nvmConfig->navyConfig;
  auto filePath = fmt::format("/tmp/nvmcache-navy-raid0/{}", ::getpid());
  util::makeDir(filePath);
  SCOPE_EXIT { util::removePath(filePath); };
  std::vector<std::string> vec = {filePath + "/CACHE0", filePath + "/CACHE1",
                                  filePath + "/CACHE2", filePath + "/CACHE3"};
  navyConfig.setSimpleFile("", 0);
  navyConfig.setRaidFiles(vec, 10 * 1024 * 1024);

  // setup a cache with some content and change the raid0 order and verify
  // that everything is correct.
  std::string val = "foobar";
  int nKeys = 100;
  auto makeKey = [&](int i) { return fmt::format("blah-{}", i); };

  this->convertToShmCache();
  {
    auto& nvm = this->cache();
    auto pid = this->poolId();

    for (int i = 0; i < nKeys; i++) {
      auto it = nvm.allocate(pid, makeKey(i), val.length());
      ASSERT_NE(nullptr, it);
      ::memcpy(it->getMemory(), val.data(), val.length());
      nvm.insertOrReplace(it);
    }
    nvm.flushNvmCache();

    // item is only in RAM
    for (int i = 0; i < nKeys; i++) {
      auto res = this->inspectCache(makeKey(i));
      // must exist in RAM
      ASSERT_NE(nullptr, res.first);
      ASSERT_EQ(::memcmp(res.first->getMemory(), val.data(), val.length()), 0);

      // must not be in nvmcache
      ASSERT_EQ(nullptr, res.second);
      this->pushToNvmCacheFromRamForTesting(makeKey(i), false);
      this->removeFromRamForTesting(makeKey(i));
    }
    nvm.flushNvmCache();

    for (int i = 0; i < nKeys; i++) {
      auto res = this->inspectCache(makeKey(i));
      // must not exist in RAM
      ASSERT_EQ(nullptr, res.first);

      // must be in nvmcache
      ASSERT_NE(nullptr, res.second);
      ASSERT_EQ(::memcmp(res.second->getMemory(), val.data(), val.length()), 0);
    }
  }

  // increase the size of the raid-0 files
  navyConfig.setRaidFiles(vec, 32 * 1024 * 1024);
  this->warmRoll();
  // recovery should fail and we should lose the previous content. nvmcache
  // should still be enabled
  //
  EXPECT_TRUE(this->cache().isNvmCacheEnabled());
  for (int i = 0; i < nKeys; i++) {
    auto res = this->inspectCache(makeKey(i));
    // must not exist in RAM
    ASSERT_EQ(nullptr, res.first);

    // must not be in nvmcache since it got dropped
    ASSERT_EQ(nullptr, res.second);
  }
}

TEST_F(NvmCacheTest, ShardHashIsNotFillMapHash) {
  auto const shardHash = getNvmShardAndHashForKey("hello world");
  ASSERT_NE(shardHash.first, shardHash.second);
}

TEST_F(NvmCacheTest, testEvictCB) {
  bool destructorCalled = false;
  DestructorContext context;
  PoolId poolid;
  // this test only checks whether the destructor is triggered, but not checking
  // the DestructedData
  allocConfig_.setRemoveCallback({});
  allocConfig_.setItemDestructor([&](const DestructorData& data) {
    destructorCalled = true;
    context = data.context;
    poolid = data.pool;
  });
  auto& cache = makeCache();
  auto pid = poolId();

  // 1. Recycled event and item not in RAM, destructor should be triggered
  {
    destructorCalled = false;
    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(10);
    auto handle = cache.allocate(pid, key, 100);
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());
    auto buf = toIOBuf(makeNvmItem(*handle));
    evictCB(HashedKey{key.data()},
            navy::BufferView(buf.length(), buf.data()),
            navy::DestructorEvent::Recycled);
    ASSERT_TRUE(destructorCalled);
    ASSERT_EQ(DestructorContext::kEvictedFromNVM, context);
    ASSERT_EQ(poolid, pid);
  }
  // 2. Recycled event and item in RAM but unclean, destructor should be
  // triggered
  {
    destructorCalled = false;
    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(10);
    // a new pool
    auto newPool = cache_->addPool("test", poolSize_, poolAllocsizes_);

    auto handle = cache.allocate(newPool, key, 100);
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());
    cache.insertOrReplace(handle);
    auto buf = toIOBuf(makeNvmItem(*handle));
    evictCB(HashedKey{key.data()},
            navy::BufferView(buf.length(), buf.data()),
            navy::DestructorEvent::Recycled);
    ASSERT_TRUE(destructorCalled);
    ASSERT_FALSE(handle->isNvmEvicted());
    ASSERT_EQ(DestructorContext::kEvictedFromNVM, context);
    ASSERT_EQ(poolid, newPool);
  }
  // 3. Recycled event and item in RAM and clean, destructor should be skipped
  {
    destructorCalled = false;
    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(10);
    auto handle = cache.allocate(pid, key, 100);
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());
    cache.insertOrReplace(handle);
    handle->markNvmClean();
    auto buf = toIOBuf(makeNvmItem(*handle));
    evictCB(HashedKey{key.data()},
            navy::BufferView(buf.length(), buf.data()),
            navy::DestructorEvent::Recycled);
    // Recycled event, in RAM and clean
    ASSERT_FALSE(destructorCalled);
    ASSERT_TRUE(handle->isNvmEvicted());
  }
  // 4. Removed event and item not in RAM, destructor should be triggered
  {
    destructorCalled = false;
    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(10);
    auto handle = cache.allocate(pid, key, 100);
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());
    auto buf = toIOBuf(makeNvmItem(*handle));
    evictCB(HashedKey{key.data()},
            navy::BufferView(buf.length(), buf.data()),
            navy::DestructorEvent::Removed);
    // Removed event, not in RAM
    ASSERT_TRUE(destructorCalled);
    ASSERT_EQ(DestructorContext::kRemovedFromNVM, context);
    ASSERT_EQ(poolid, pid);
  }
  // 5. Removed event and item in RAM but unclean, destructor should be
  // triggered
  {
    destructorCalled = false;
    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(10);
    auto handle = cache.allocate(pid, key, 100);
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());
    cache.insertOrReplace(handle);
    auto buf = toIOBuf(makeNvmItem(*handle));
    evictCB(HashedKey{key.data()},
            navy::BufferView(buf.length(), buf.data()),
            navy::DestructorEvent::Removed);
    // Removed event, in RAM but unclean
    ASSERT_TRUE(destructorCalled);
    ASSERT_EQ(DestructorContext::kRemovedFromNVM, context);
  }
  // 6. Removed event and item in RAM and clean, destructor should be
  // skipped
  {
    destructorCalled = false;
    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(10);
    auto handle = cache.allocate(pid, key, 100);
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());
    cache.insertOrReplace(handle);
    handle->markNvmClean();
    auto buf = toIOBuf(makeNvmItem(*handle));
    evictCB(HashedKey{key.data()},
            navy::BufferView(buf.length(), buf.data()),
            navy::DestructorEvent::Removed);
    // Removed event, in RAM and clean
    ASSERT_FALSE(destructorCalled);
    ASSERT_TRUE(handle->isNvmEvicted());
  }
}

void verifyItem(const Item& item, const Item& iobufItem) {
  ASSERT_EQ(item.isChainedItem(), iobufItem.isChainedItem());
  ASSERT_EQ(item.hasChainedItem(), iobufItem.hasChainedItem());
  if (!item.isChainedItem()) {
    ASSERT_EQ(item.getCreationTime(), iobufItem.getCreationTime());
    ASSERT_EQ(item.getExpiryTime(), iobufItem.getExpiryTime());
  }
  ASSERT_EQ(item.getSize(), iobufItem.getSize());
  ASSERT_EQ(
      0, std::memcmp(item.getMemory(), iobufItem.getMemory(), item.getSize()));
  // iobuf item is prepared for ItemDestructor, accessible should be false
  ASSERT_FALSE(iobufItem.isAccessible());
}

void NvmCacheTest::verifyItemInIOBuf(const std::string& key,
                                     const ReadHandle& handle,
                                     folly::IOBuf* iobuf) {
  Item& item = *reinterpret_cast<Item*>(iobuf->writableData());
  ASSERT_LE(Item::getRequiredSize(key, handle->getSize()), iobuf->length());

  ASSERT_EQ(true, item.isNvmClean());
  ASSERT_EQ(true, item.isNvmEvicted());
  ASSERT_EQ(0, item.getRefCount());
  ASSERT_EQ(handle->getKey(), item.getKey());
  verifyItem(*handle, item);

  if (item.hasChainedItem()) {
    auto iobufRange = viewAsChainedAllocsRange(iobuf);
    auto handleRange = cache().viewAsChainedAllocsRange(*handle);

    auto iobufIter = iobufRange.begin();
    auto handleIter = handleRange.begin();

    while (iobufIter != iobufRange.end() || handleIter != handleRange.end()) {
      ASSERT_NE(iobufIter, iobufRange.end());
      ASSERT_NE(handleIter, handleRange.end());
      verifyItem(handleIter.dereference(), iobufIter.dereference());
      handleIter.increment();
      iobufIter.increment();
    }
  }
}

TEST_F(NvmCacheTest, testCreateItemAsIOBuf) {
  auto& cache = this->cache();
  auto pid = this->poolId();

  {
    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(10);
    auto handle = cache.allocate(pid, key, 100);
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());

    auto dipper = makeNvmItem(*handle);
    auto iobuf = createItemAsIOBuf(key, *dipper);

    verifyItemInIOBuf(key, handle, iobuf.get());
  }
  {
    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(100);
    auto handle = cache.allocate(pid, key, 1000);
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());

    auto dipper = makeNvmItem(*handle);
    auto iobuf = createItemAsIOBuf(key, *dipper);

    verifyItemInIOBuf(key, handle, iobuf.get());
  }
}

TEST_F(NvmCacheTest, testCreateItemAsIOBufChained) {
  auto& cache = this->cache();
  auto pid = this->poolId();

  {
    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(10);
    int nChained = 10;

    auto handle = cache.allocate(pid, key, 100);
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());
    cache.insertOrReplace(handle);

    for (int i = 0; i < nChained; i++) {
      std::string chainedVal = val + "_chained_" + std::to_string(i);
      auto chainedIt = cache.allocateChainedItem(handle, chainedVal.length());
      ASSERT_TRUE(chainedIt);
      ::memcpy(chainedIt->getMemory(), chainedVal.data(), chainedVal.length());
      cache.addChainedItem(handle, std::move(chainedIt));
    }

    auto dipper = makeNvmItem(*handle);
    auto iobuf = createItemAsIOBuf(key, *dipper);

    verifyItemInIOBuf(key, handle, iobuf.get());
  }
}

TEST_F(NvmCacheTest, testSampleItem) {
  auto& config = getConfig();
  auto& navyConfig = config.nvmConfig->navyConfig;
  navyConfig.setMemoryFile(config.getCacheSize());
  navyConfig.setDeviceMetadataSize(0);
  // Use only BlockCache for simplicity
  navyConfig.bigHash().setSizePctAndMaxItemSize(0, 0);
  navyConfig.blockCache().setRegionSize(32 * 1024);

  // This test is dependent on poolAllocsizes_
  ASSERT_EQ(20 * 1024, *poolAllocsizes_.begin());
  size_t numMax = config.getCacheSize() / *poolAllocsizes_.begin();

  std::mutex mtx;
  std::atomic<int> numEvicted = 0;
  std::unordered_set<std::string> cachedKeys;

  config.setRemoveCallback({});
  config.setItemDestructor([&](const DestructedData& data) {
    std::unique_lock<std::mutex> l(mtx);
    if (data.context == DestructorContext::kEvictedFromNVM ||
        data.context == DestructorContext::kEvictedFromRAM) {
      ++numEvicted;
    }
    cachedKeys.erase(data.item.getKey().toString());
  });

  auto& cache = makeCache();
  auto pid = this->poolId();

  size_t nKeys = 0;
  static constexpr unsigned kEvenKeyTTL = 5;
  // Insert items until either RAM or NVM cache is full
  for (; numEvicted == 0 && nKeys < numMax; nKeys++) {
    unsigned ttl = nKeys % 2 == 0 ? kEvenKeyTTL : 0;
    auto key = fmt::format("key{}", nKeys);
    // the pool's allocsize is
    auto it = cache.allocate(pid, key, 16 * 1024, ttl);
    ASSERT_NE(nullptr, it);
    cache.insertOrReplace(it);

    {
      std::unique_lock<std::mutex> l(mtx);
      cachedKeys.insert(key);
    }
    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key));
  }

  // wait for async remove finish
  cache.flushNvmCache();

  XLOGF(INFO,
        "Wait {}s until all items with even numbered keys are expired by TTL",
        kEvenKeyTTL + 1);
  /* sleep override */ std::this_thread::sleep_for(
      std::chrono::seconds(kEvenKeyTTL + 1));

  {
    std::unique_lock<std::mutex> l(mtx);
    nKeys = cachedKeys.size();
  }

  size_t numRam = 0;
  size_t numNvm = 0;
  for (size_t i = 0; i < nKeys * 10; i++) {
    auto sample = cache.getSampleItem();
    if (sample.isValid()) {
      auto keyStr = sample->getKey().toString();
      {
        std::unique_lock<std::mutex> l(mtx);
        ASSERT_TRUE(cachedKeys.contains(keyStr));
      }

      unsigned idx;
      ASSERT_GT(std::sscanf(keyStr.c_str(), "key%u", &idx), 0);
      ASSERT_TRUE(idx % 2 != 0) << fmt::format(
          "Error: expired item with key ({}) and nvm ({}) returned",
          keyStr,
          sample.isNvmItem());

      if (sample.isNvmItem()) {
        numNvm++;
      } else {
        numRam++;
      }
    }
  }

  // internal fragmentation for RAM and NVM are around
  // 20% (16K / 20K) and 37.5% (20K / 32K), respectively
  // 15% is arbitrary and pessimistic target
  size_t targetCnt = (size_t)(static_cast<double>(nKeys) * 10.0 * 0.5 * 0.15);
  ASSERT_GE(numNvm, targetCnt);
  ASSERT_GE(numRam, targetCnt);
  // Should reset the cache since the destructor callback
  // could use local variables
  cache_.reset();
}

TEST_F(NvmCacheTest, testItemDestructor) {
  uint32_t destructorCount = 0;
  std::unordered_set<std::pair<std::string, std::string>> destructedItems;
  getConfig().setRemoveCallback({});
  getConfig().setItemDestructor([&](const DestructedData& data) {
    ++destructorCount;
    std::string val =
        std::string(reinterpret_cast<const char*>(data.item.getMemory()),
                    data.item.getSize());
    for (auto& chained : data.chainedAllocs) {
      val += std::string(reinterpret_cast<const char*>(chained.getMemory()),
                         chained.getSize());
    }
    destructedItems.insert(std::make_pair(data.item.getKey().toString(), val));
  });
  auto& cache = makeCache();
  auto pid = this->poolId();

  // 1. remove the item that is in NVM only
  {
    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(10);
    int nChained = 10;
    std::string combinedVal = val;

    auto handle = cache.allocate(pid, key, val.size());
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());

    for (int i = 0; i < nChained; i++) {
      std::string chainedVal = val + "_chained_" + std::to_string(i);
      auto chainedIt = cache.allocateChainedItem(handle, chainedVal.length());
      ASSERT_TRUE(chainedIt);
      ::memcpy(chainedIt->getMemory(), chainedVal.data(), chainedVal.length());
      cache.addChainedItem(handle, std::move(chainedIt));
    }

    cache.insertOrReplace(handle);
    pushToNvmCacheFromRamForTesting(key);
    removeFromRamForTesting(key);
    handle.reset();
    // manual remove will trigger destructor
    destructorCount = 0;
    destructedItems.clear();

    {
      auto res = this->inspectCache(key);
      // must not exist in RAM
      ASSERT_EQ(nullptr, res.first);

      // must be in nvmcache
      ASSERT_NE(nullptr, res.second);
    }

    cache.remove(key);
    // wait for async remove finish
    cache.flushNvmCache();

    ASSERT_EQ(1, destructorCount);
    auto it = destructedItems.begin();
    ASSERT_EQ(key, it->first);

    // chained items are in reversed order
    for (int i = nChained - 1; i >= 0; --i) {
      combinedVal += val + "_chained_" + std::to_string(i);
    }
    ASSERT_EQ(combinedVal, it->second);
  }

  // 2. remove the item that is in both RAM and NVM
  {
    destructorCount = 0;
    destructedItems.clear();

    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(200);

    auto handle = cache.allocate(pid, key, val.size());
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());

    cache.insertOrReplace(handle);
    pushToNvmCacheFromRamForTesting(key);

    {
      auto res = this->inspectCache(key);
      // in both RAM and nvmcache
      ASSERT_NE(nullptr, res.first);
      ASSERT_NE(nullptr, res.second);
    }

    cache.invalidateNvm(*handle);
    // wait for async remove finish
    cache.flushNvmCache();

    ASSERT_EQ(0, destructorCount);
    ASSERT_TRUE(destructedItems.empty());
    ASSERT_FALSE(handle->isNvmEvicted());
  }

  // 3. remove the item that is in both RAM and NVM,
  // but the RAM handle is hold by user
  // destruct should be triggered until handle is released
  {
    destructorCount = 0;
    destructedItems.clear();

    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(20);

    auto handle = cache.allocate(pid, key, val.size());
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());

    cache.insertOrReplace(handle);
    pushToNvmCacheFromRamForTesting(key);

    {
      auto res = this->inspectCache(key);
      // in both RAM and nvmcache
      ASSERT_NE(nullptr, res.first);
      ASSERT_NE(nullptr, res.second);
    }

    cache.remove(key);
    // wait for async remove finish
    cache.flushNvmCache();

    // handle is still being hold
    ASSERT_EQ(0, destructorCount);
    ASSERT_TRUE(handle->isNvmClean());

    handle.reset();
    ASSERT_EQ(1, destructorCount);
  }

  // 4. remove the item that is in both RAM and NVM,
  // but RAM copy is replaced by a new item.
  {
    destructorCount = 0;
    destructedItems.clear();

    std::string key = "key" + genRandomStr(10);
    std::string val = "val" + genRandomStr(20);

    auto handle = cache.allocate(pid, key, val.size());
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());

    cache.insertOrReplace(handle);
    pushToNvmCacheFromRamForTesting(key);

    // replace with new val
    auto val2 = "val" + genRandomStr(25);
    handle = cache.allocate(pid, key, val2.size());
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val2.data(), val2.size());
    cache.insertOrReplace(handle);

    // wait for async remove finish
    cache.flushNvmCache();

    ASSERT_EQ(1, destructorCount);
    ASSERT_FALSE(handle->isNvmEvicted());
    ASSERT_FALSE(handle->isNvmClean());

    auto it = destructedItems.begin();
    ASSERT_EQ(key, it->first);
    ASSERT_EQ(val, it->second);
  }
}

TEST_F(NvmCacheTest, testItemDestructorMovePreservesNvmEvicted) {
  uint32_t destructorCount = 0;
  getConfig().setRemoveCallback({});
  getConfig().setItemDestructor(
      [&](const DestructedData&) { ++destructorCount; });
  getConfig().enableMovingOnSlabRelease([](Item& oldItem, Item& newItem,
                                           Item* /* parentPtr */) {
    std::memcpy(newItem.getMemory(), oldItem.getMemory(), oldItem.getSize());
  });
  auto& cache = makeCache();
  auto pid = this->poolId();

  const std::string key = "key" + genRandomStr(10);
  const std::string val = "val" + genRandomStr(10);
  {
    auto handle = cache.allocate(pid, key, val.size());
    ASSERT_NE(nullptr, handle.get());
    std::memcpy(handle->getMemory(), val.data(), val.size());
    cache.insertOrReplace(handle);
  }
  ASSERT_TRUE(pushToNvmCacheFromRamForTesting(key));

  // Drop only the NVM copy so the RAM copy becomes NvmClean + NvmEvicted.
  removeFromNvmForTesting(key);
  cache.flushNvmCache();
  void* oldMemory = nullptr;
  {
    auto handle = fetch(key, true /* ramOnly */);
    ASSERT_NE(nullptr, handle.get());
    ASSERT_TRUE(handle->isNvmClean());
    ASSERT_TRUE(handle->isNvmEvicted());
    oldMemory = handle.get();
  }

  releaseSlabFor(oldMemory);
  {
    auto handle = fetch(key, true /* ramOnly */);
    ASSERT_NE(nullptr, handle.get());
    ASSERT_NE(oldMemory, handle.get());
    EXPECT_TRUE(handle->isNvmClean());
    EXPECT_TRUE(handle->isNvmEvicted());
  }

  cache.remove(key);
  cache.flushNvmCache();
  EXPECT_EQ(1, destructorCount);
  EXPECT_EQ(0,
            cache.getNvmCacheStatsMap().getCounts().at(
                "items_tracked_for_destructor"));
}

TEST_F(NvmCacheTest, testItemDestructorPutFail) {
  auto& config = getConfig();
  auto& navyConfig = config.nvmConfig->navyConfig;
  navyConfig.blockCache().setRegionSize(1024);

  int destructorCount = 0;
  std::string destructoredKey;
  config.setRemoveCallback({});
  config.setItemDestructor([&](const DestructedData& data) {
    ++destructorCount;
    destructoredKey = data.item.getKey();
  });

  auto& cache = makeCache();
  auto pid = poolId();

  std::string key = "key" + genRandomStr(10);
  {
    // val size larger than BlockCacheRegionSize
    auto handle = cache.allocate(pid, key, 10000);
    ASSERT_NE(nullptr, handle.get());
    // NVM::put will fail
    pushToNvmCacheFromRamForTesting(handle);
    ASSERT_TRUE(handle->isNvmClean());
    ASSERT_FALSE(handle->isNvmEvicted());
  }
  // wait for async insert finish
  cache.flushNvmCache();
  // we manually push item to nvm without inserted into dram cache,
  // PutFail will trigger destructor
  ASSERT_EQ(1, destructorCount);
  ASSERT_EQ(key, destructoredKey);
}

TEST_F(NvmCacheTest, testFindToWriteNvmInvalidation) {
  uint32_t destructorCount = 0;
  std::unordered_set<std::pair<std::string, std::string>> destructedItems;
  getConfig().setRemoveCallback({});
  getConfig().setItemDestructor([&](const DestructedData& data) {
    ++destructorCount;
    std::string val =
        std::string(reinterpret_cast<const char*>(data.item.getMemory()),
                    data.item.getSize());
    for (auto& chained : data.chainedAllocs) {
      val += std::string(reinterpret_cast<const char*>(chained.getMemory()),
                         chained.getSize());
    }
    destructedItems.insert(std::make_pair(data.item.getKey().toString(), val));
  });
  auto& cache = makeCache();
  auto pid = this->poolId();
  std::string key = "key" + genRandomStr(10);
  std::string val = "val" + genRandomStr(200);

  auto handle = cache.allocate(pid, key, val.size());
  ASSERT_NE(nullptr, handle.get());
  std::memcpy(handle->getMemory(), val.data(), val.size());

  cache.insertOrReplace(handle);
  pushToNvmCacheFromRamForTesting(key);

  {
    auto res = this->inspectCache(key);
    // in both RAM and nvmcache
    ASSERT_NE(nullptr, res.first);
    ASSERT_NE(nullptr, res.second);
  }

  handle = cache.findToWrite(key);
  // wait for async remove finish
  cache.flushNvmCache();

  ASSERT_NE(nullptr, handle.get());
  ASSERT_EQ(0, destructorCount);
  ASSERT_TRUE(destructedItems.empty());
  ASSERT_FALSE(handle->isNvmEvicted());
  ASSERT_FALSE(handle->isNvmClean());
}

TEST_F(NvmCacheTest, IsNewCacheInstanceStat) {
  // A new instane of cache should have this stat set to true
  // A cache that is recovered successfully should set it to false

  auto stats = getStats();
  EXPECT_TRUE(stats.isNewRamCache);
  EXPECT_TRUE(stats.isNewNvmCache);
  // The sleep calls in this test is to make sure the time
  // has moved forward by a second or two so the cache uptime
  // checks we rely on for determining new/warm cache is valid.
  std::this_thread::sleep_for(std::chrono::seconds{2});

  // Use SHM. This is also a new cache instance
  this->convertToShmCache();
  stats = getStats();
  EXPECT_TRUE(stats.isNewRamCache);
  EXPECT_TRUE(stats.isNewNvmCache);
  std::this_thread::sleep_for(std::chrono::seconds{2});

  warmRoll();
  stats = getStats();
  EXPECT_FALSE(stats.isNewRamCache);
  EXPECT_FALSE(stats.isNewNvmCache);
  std::this_thread::sleep_for(std::chrono::seconds{2});

  coldRoll();
  stats = getStats();
  EXPECT_TRUE(stats.isNewRamCache);
  EXPECT_FALSE(stats.isNewNvmCache);
  std::this_thread::sleep_for(std::chrono::seconds{2});

  warmRoll();
  stats = getStats();
  EXPECT_FALSE(stats.isNewRamCache);
  EXPECT_FALSE(stats.isNewNvmCache);
  std::this_thread::sleep_for(std::chrono::seconds{2});

  iceRoll();
  stats = getStats();
  EXPECT_FALSE(stats.isNewRamCache);
  EXPECT_TRUE(stats.isNewNvmCache);
  std::this_thread::sleep_for(std::chrono::seconds{2});

  warmRoll();
  stats = getStats();
  EXPECT_FALSE(stats.isNewRamCache);
  EXPECT_FALSE(stats.isNewNvmCache);
  std::this_thread::sleep_for(std::chrono::seconds{2});

  iceColdRoll();
  stats = getStats();
  EXPECT_TRUE(stats.isNewRamCache);
  EXPECT_TRUE(stats.isNewNvmCache);
  std::this_thread::sleep_for(std::chrono::seconds{2});
}

TEST_F(NvmCacheTest, BadDeviceIoFailure) {
  // Device errors along the deletion path will disable NvmCache
  {
    const auto key = "test_key_foo";
    auto& config = getConfig();
    // Disable remove callback to set item dtor instead
    config.setRemoveCallback({});
    config.setItemDestructor([](const DestructedData&) {
      // noop. Need to set item dtor to activate lookup on remove in BC
    });
    config.nvmConfig->navyConfig.setBadDeviceForTesting(
        navy::BadDeviceStatus::IoReqFailure);
    auto& cache = makeCache();
    {
      auto it = cache.allocate(this->poolId(), key, 200);
      std::memcpy(it->getMemory(), "foobar", sizeof("foobar"));
      cache.insertOrReplace(it);
    }
    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key));
    cache.flushNvmCache();
    this->removeFromRamForTesting(key);
    EXPECT_TRUE(cache.isNvmCacheEnabled());

    // Device error on remove disables nvm-cache
    cache.remove(key);
    cache.flushNvmCache();
    EXPECT_FALSE(cache.isNvmCacheEnabled());
  }

  // NvmCache remains usable after device errors. However, the items that are
  // caught in errors will never be returned to the user again.
  {
    const auto key = "test_key_foo";
    auto& config = getConfig();
    // Disable remove callback to set item dtor instead
    config.setRemoveCallback({});
    config.setItemDestructor([](const DestructedData&) {
      // noop. Need to set item dtor to activate lookup on remove in BC
    });
    config.nvmConfig->navyConfig.setBadDeviceForTesting(
        navy::BadDeviceStatus::IoReqFailure);
    // Do not disable NvmCache
    config.nvmConfig->disableNvmCacheOnBadState = false;
    auto& cache = makeCache();
    {
      auto it = cache.allocate(this->poolId(), key, 200);
      std::memcpy(it->getMemory(), "foobar", sizeof("foobar"));
      cache.insertOrReplace(it);
    }
    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key));
    cache.flushNvmCache();
    this->removeFromRamForTesting(key);
    EXPECT_TRUE(cache.isNvmCacheEnabled());

    // Device error on remove does not disable nvm-cache
    cache.remove(key);
    cache.flushNvmCache();
    EXPECT_TRUE(cache.isNvmCacheEnabled());

    auto it = cache.find(key);
    EXPECT_EQ(nullptr, it);
  }
}

TEST_F(NvmCacheTest, DataCorruption) {
  // Checksum error with data corruption will not disable NvmCache
  // Only the item with checksum error will be removed from NvmCache
  {
    const auto key = "test_key_foo";
    auto& config = getConfig();
    // Disable remove callback to set item dtor instead
    config.setRemoveCallback({});
    config.setItemDestructor([](const DestructedData&) {
      // noop. Need to set item dtor to activate lookup on remove in BC
    });
    config.nvmConfig->navyConfig.setBadDeviceForTesting(
        navy::BadDeviceStatus::DataCorruption);
    auto& cache = makeCache();
    {
      auto it = cache.allocate(this->poolId(), key, 200);
      std::memcpy(it->getMemory(), "foobar", sizeof("foobar"));
      cache.insertOrReplace(it);
    }
    ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(key));
    cache.flushNvmCache();
    this->removeFromRamForTesting(key);
    EXPECT_TRUE(cache.isNvmCacheEnabled());

    // Checksum error on remove does not disable nvm-cache
    cache.remove(key);
    cache.flushNvmCache();
    EXPECT_TRUE(cache.isNvmCacheEnabled());

    // The item with checksum error was removed from NvmCache
    auto it = cache.find(key);
    EXPECT_EQ(nullptr, it);
  }
}

TEST_F(NvmCacheTest, NvmHitTTATracking) {
  // Disable BigHash — it doesn't store lastAccessTimeSecs (returns 0).
  // BlockCache tracks it, so TTA can be computed.
  this->config_.bigHash().setSizePctAndMaxItemSize(0, 100);
  LruAllocator::NvmCacheConfig nvmConfig;
  nvmConfig.navyConfig = config_;
  this->allocConfig_.enableNvmCache(nvmConfig);
  this->makeCache();

  auto& cache = this->cache();
  auto pid = this->poolId();

  std::string key = "tta_test_key";
  std::string val = "tta_test_value";

  // 1. Insert item into DRAM
  {
    auto it = cache.allocate(pid, key, val.length());
    ASSERT_NE(nullptr, it);
    ::memcpy(it->getMemory(), val.data(), val.length());
    cache.insertOrReplace(it);
  }

  // 2. Push to NVM and remove from RAM
  this->pushToNvmCacheFromRamForTesting(key);
  this->removeFromRamForTesting(key);

  // 3. Wait so TTA (currentTime - lastAccessTimeSecs) > 0
  /* sleep override */ std::this_thread::sleep_for(std::chrono::seconds(1));

  // 4. Fetch from NVM (triggers onGetComplete -> TTA tracking)
  auto it = this->fetch(key, false /* ramOnly */);
  ASSERT_NE(nullptr, it);

  // 5. Verify NVM hit occurred
  auto stats = this->getStats();
  EXPECT_GT(stats.numNvmGets, 0);
  EXPECT_GT(stats.numNvmGets - stats.numNvmGetMiss, 0);

  // 6. Verify TTA was tracked (>= 1 second)
  EXPECT_GE(stats.nvmHitTTASecs.p50, 1);
}
TEST_F(NvmCacheTest, NvmLargeItemFlagOnPromotion) {
  // Verify that the NvmLargeItem flag is set correctly on promotion:
  // - Items routed to BlockCache (large) should have the flag set.
  // - Items routed to BigHash (small) should NOT have the flag set.
  this->config_.setSimpleFile(cacheDir_ + "/navy", 200 * 1024ULL * 1024ULL,
                              true /* truncateFile */);
  LruAllocator::NvmCacheConfig nvmConfig;
  nvmConfig.navyConfig = this->config_;
  nvmConfig.truncateItemToOriginalAllocSizeInNvm = true;
  auto& config = this->getConfig();
  config.enableNvmCache(nvmConfig);
  this->poolAllocsizes_ = {20 * 1024};
  auto& cache = this->makeCache();
  auto pid = this->poolId();

  auto testNvmLargeItemFlag = [&](const std::string& key, uint32_t valSize,
                                  char fillChar) -> bool {
    {
      auto it = cache.allocate(pid, key, valSize);
      EXPECT_NE(nullptr, it);
      ::memset(it->getMemory(), fillChar, it->getSize());
      cache.insertOrReplace(it);
    }
    EXPECT_TRUE(this->pushToNvmCacheFromRamForTesting(key));
    cache.flushNvmCache();
    this->removeFromRamForTesting(key);
    auto hdl = this->fetch(key, false /* ramOnly */);
    EXPECT_NE(nullptr, hdl);
    EXPECT_TRUE(hdl->isNvmClean());
    return hdl->isNvmLargeItem();
  };

  EXPECT_TRUE(testNvmLargeItemFlag("bc", 15 * 1024, 'L'));
  EXPECT_FALSE(testNvmLargeItemFlag("bh", 50, 'S'));
}

TEST_F(NvmCacheTest, AccessTimeMapPopulatedOnDramEviction) {
  const int nKeys = 5;
  const uint32_t allocSize = 15 * 1024;

  insertPromoteAndEvictNvmCleanItems("atm_multi", allocSize, nKeys, allocSize);

  // ATM should now have entries for the NvmClean items that were evicted
  // from DRAM (BlockCache items only).
  auto* atm = this->getAccessTimeMap();
  auto now = util::getCurrentTimeSec();
  int populated = 0;
  for (int i = 0; i < nKeys; i++) {
    auto key = fmt::format("atm_multi_{}", i);
    HashedKey hk{key};
    auto ts = atm->get(hk.keyHash());
    if (ts != std::nullopt) {
      EXPECT_GT(*ts, 0);
      EXPECT_LE(*ts, now);
      ++populated;
    }
  }
  EXPECT_GT(populated, 0);
}

TEST_F(NvmCacheTest, AccessTimeMapNotUpdatedForBigHashItems) {
  // Enable truncation so small items route to BigHash instead of BlockCache.
  this->config_.setSimpleFile(cacheDir_ + "/navy", 200 * 1024ULL * 1024ULL,
                              true /* truncateFile */);
  LruAllocator::NvmCacheConfig nvmConfig;
  nvmConfig.navyConfig = this->config_;
  nvmConfig.truncateItemToOriginalAllocSizeInNvm = true;
  auto& config = this->getConfig();
  config.enableNvmCache(nvmConfig);
  this->poolAllocsizes_ = {20 * 1024};
  this->makeCache();

  const int nKeys = 5;
  const uint32_t allocSize = 50;
  const uint32_t fillerSize = 15 * 1024;

  insertPromoteAndEvictNvmCleanItems("bh", allocSize, nKeys, fillerSize);

  // ATM should NOT have entries for the BigHash items — the isNvmLargeItem()
  // bit is not set for BigHash items, preventing updateAccessTime().
  auto* atm = this->getAccessTimeMap();
  for (int i = 0; i < nKeys; i++) {
    auto key = fmt::format("bh_{}", i);
    HashedKey hk{key};
    auto ts = atm->get(hk.keyHash());
    EXPECT_EQ(std::nullopt, ts)
        << "BigHash item " << key << " should not be in ATM";
  }
}

TEST_F(NvmCacheTest, AccessTimeMapNotUpdatedOnRegularEviction) {
  auto& nvm = this->cache();
  auto pid = this->poolId();
  const uint32_t allocSize = 15 * 1024;
  const uint32_t numKeysPerRegion =
      config_.blockCache().getRegionSize() / allocSize;

  auto* atm = this->getAccessTimeMap();
  ASSERT_NE(nullptr, atm);
  EXPECT_EQ(0, atm->size());

  // Insert many fresh items (never been to NVM, so NOT NvmClean).
  // Evictions of these items go through the NVM put path, not
  // the updateAccessTime path.
  for (int i = 0; i < 1024; i++) {
    auto key = fmt::format("regular_{}", i);
    auto it = nvm.allocate(pid, key, allocSize);
    ASSERT_NE(nullptr, it);
    cache_->insertOrReplace(it);
    if (i % numKeysPerRegion == 0) {
      nvm.flushNvmCache();
    }
  }
  nvm.flushNvmCache();
  ASSERT_GT(this->evictionCount(), 0);

  // Non-NvmClean evictions should NOT populate the AccessTimeMap.
  EXPECT_EQ(0, atm->size());
}

TEST_F(NvmCacheTest, AccessTimeMapCleanupTest) {
  // Verify ATM entries are cleaned up when items leave NVM via:
  //   Group 0: remove() while only in NVM (Removed)
  //   Group 1: promote to DRAM, then remove() (Removed)
  //   Group 2: promote to DRAM, then insertOrReplace() (Removed)
  //   Group 3: NVM eviction via region reclaim (Recycled)
  auto& config = getConfig();
  config.setRemoveCallback({});
  config.setItemDestructor([](const DestructedData&) {});
  this->makeCache();

  const int nKeys = 16;
  const uint32_t allocSize = 15 * 1024;

  insertPromoteAndEvictNvmCleanItems("atm_cl", allocSize, nKeys, allocSize);

  auto& nvm = this->cache();
  auto pid = this->poolId();
  auto* atm = this->getAccessTimeMap();
  ASSERT_NE(nullptr, atm);
  EXPECT_EQ(std::nullopt, atm->get(HashedKey{"atm_cl"}.keyHash()));

  auto expectAtmCleared = [&](const std::vector<std::string>& keys,
                              const char* desc) {
    for (const auto& key : keys) {
      HashedKey hk{key};
      EXPECT_EQ(std::nullopt, atm->get(hk.keyHash())) << desc << " " << key;
    }
  };

  // Collect keys that have ATM entries after DRAM eviction.
  // Split into four groups by index % 4.
  constexpr int kNumGroups = 4;
  std::array<std::vector<std::string>, kNumGroups> groups;
  for (int i = 0; i < nKeys; i++) {
    auto key = fmt::format("atm_cl_{}", i);
    HashedKey hk{key};
    if (atm->get(hk.keyHash()) != std::nullopt) {
      groups[i % kNumGroups].push_back(key);
    }
  }
  size_t totalKeys = 0;
  for (const auto& g : groups) {
    totalKeys += g.size();
  }
  ASSERT_GT(totalKeys, 0);

  // Group 0: remove items while they're only in NVM.
  for (const auto& key : groups[0]) {
    nvm.remove(key);
  }
  nvm.flushNvmCache();
  expectAtmCleared(groups[0], "NVM-only removed item");

  // Group 1: promote back to DRAM, then remove.
  for (const auto& key : groups[1]) {
    auto hdl = this->fetch(key, false /* ramOnly */);
    ASSERT_NE(nullptr, hdl);
    nvm.remove(key);
  }
  nvm.flushNvmCache();
  expectAtmCleared(groups[1], "DRAM removed item");

  // Group 2: promote back to DRAM, then replace via insertOrReplace.
  for (const auto& key : groups[2]) {
    auto hdl = this->fetch(key, false /* ramOnly */);
    ASSERT_NE(nullptr, hdl);
    auto it = nvm.allocate(pid, key, allocSize);
    ASSERT_NE(nullptr, it);
    this->insertOrReplace(it);
  }
  expectAtmCleared(groups[2], "Replaced item");

  // Group 3: trigger NVM eviction by filling NVM with new items until
  // BlockCache reclaims regions containing the group-3 items.
  const uint32_t numKeysPerRegion =
      config_.blockCache().getRegionSize() / allocSize;
  for (int i = 0; i < 2048; i++) {
    auto key = fmt::format("nvm_evictor_{}", i);
    auto it = nvm.allocate(pid, key, allocSize);
    ASSERT_NE(nullptr, it);
    cache_->insertOrReplace(it);
    if (i % numKeysPerRegion == 0) {
      nvm.flushNvmCache();
    }
  }
  nvm.flushNvmCache();
  expectAtmCleared(groups[3], "NVM-evicted item");
}

TEST_F(NvmCacheTest, AccessTimeMapSurvivesWarmRoll) {
  // Verify that ATM entries persist across a warm restart cycle:
  //   shutDown() persists ATM → SharedMemAttach recovers ATM
  this->convertToShmCache();
  auto& nvm = this->cache();
  auto pid = this->poolId();
  const uint32_t allocSize = 15 * 1024;
  const uint32_t numKeysPerRegion =
      config_.blockCache().getRegionSize() / allocSize;

  // Insert a target item and push it to NVM.
  std::string targetKey = "atm_warm_roll_target";
  {
    auto it = nvm.allocate(pid, targetKey, allocSize);
    ASSERT_NE(nullptr, it);
    cache_->insertOrReplace(it);
  }
  ASSERT_TRUE(this->pushToNvmCacheFromRamForTesting(targetKey));
  nvm.flushNvmCache();
  this->removeFromRamForTesting(targetKey);

  // Promote target from NVM (making it NvmClean in DRAM).
  {
    auto hdl = this->fetch(targetKey, false /* ramOnly */);
    ASSERT_NE(nullptr, hdl);
    ASSERT_TRUE(hdl->isNvmClean());
  }

  HashedKey targetHk{targetKey};
  auto* atm = this->getAccessTimeMap();
  ASSERT_NE(nullptr, atm);

  // ATM should be empty after promotion (entry is created on eviction).
  EXPECT_EQ(std::nullopt, atm->get(targetHk.keyHash()));

  // Insert filler items to trigger DRAM eviction of the NvmClean target.
  auto timeBefore = util::getCurrentTimeSec();
  auto evictBefore = this->evictionCount();
  for (int i = 0; i < 1024; i++) {
    auto key = fmt::format("atm_warm_roll_filler_{}", i);
    auto it = nvm.allocate(pid, key, allocSize);
    ASSERT_NE(nullptr, it);
    cache_->insertOrReplace(it);
    if (i % numKeysPerRegion == 0) {
      nvm.flushNvmCache();
    }
  }
  nvm.flushNvmCache();
  ASSERT_LT(evictBefore, this->evictionCount());

  // The target was NvmClean when evicted, so ATM should have its entry.
  auto accessTime = atm->get(targetHk.keyHash());
  ASSERT_NE(std::nullopt, accessTime);
  EXPECT_GE(*accessTime, timeBefore);
  auto savedTime = *accessTime;

  // Warm restart: shutDown() persists ATM, SharedMemAttach recovers it.
  this->warmRoll();

  // Re-acquire the ATM pointer (NvmCache instance is recreated).
  auto* atmAfter = this->getAccessTimeMap();
  ASSERT_NE(nullptr, atmAfter);

  // Verify the entry survived with the same timestamp.
  auto recoveredTime = atmAfter->get(targetHk.keyHash());
  ASSERT_NE(std::nullopt, recoveredTime)
      << "ATM entry missing after warm restart";
  EXPECT_EQ(savedTime, *recoveredTime);
}

} // namespace tests
} // namespace cachelib
} // namespace facebook
