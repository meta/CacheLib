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

// find() per key vs findMany() on a warmed DRAM cache holding all items:
// Zipfian keys in windows of --batch distinct keys, every hit reads its value.
// Reports CPU time per key for each pass.
//
// Example:
//   FindManyBench --mode=find --num_keys=40000000 --size_gb=36 --threads=16
//   FindManyBench --mode=findMany --num_keys=40000000 --size_gb=36 --threads=16

#include <fmt/core.h>
#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <latch>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#include "cachelib/allocator/CacheAllocator.h"

using namespace facebook::cachelib;

DEFINE_string(mode, "findMany", "find or findMany");
DEFINE_uint64(num_keys, 1 << 22, "Number of items in the cache");
DEFINE_uint64(size_gb, 4, "Cache size in GB; must hold every item");
DEFINE_uint32(value_size, 500, "Value size in bytes");
DEFINE_uint32(threads, 4, "Lookup threads");
DEFINE_uint32(batch, 32, "Distinct keys per lookup window");
DEFINE_double(zipf_theta, 0.99, "Zipfian skew, in [0, 1); 0 is uniform");
DEFINE_uint64(keys_per_pass, 1 << 21, "Keys looked up per thread per pass");
DEFINE_uint32(passes, 4, "Measured passes after one warmup pass");
DEFINE_uint64(seed, 1, "Random seed");

namespace {

using Cache = LruAllocator;
using KeyBytes = std::array<char, 16>;

KeyBytes makeKey(uint64_t id) {
  uint64_t hash = id + 0x9e3779b97f4a7c15ULL;
  hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
  hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebULL;
  hash ^= hash >> 31;
  KeyBytes key;
  std::memcpy(key.data(), &hash, 8);
  std::memcpy(key.data() + 8, &id, 8);
  return key;
}

Cache::Key asKey(const KeyBytes& key) { return {key.data(), key.size()}; }

// YCSB ZipfianGenerator (Gray et al.); rank 0 is the most popular item.
class Zipf {
 public:
  Zipf(uint64_t items, double theta) : items_(items) {
    double zetan = 0;
    for (uint64_t i = 1; i <= items; ++i) {
      zetan += 1.0 / std::pow(double(i), theta);
    }
    const double zeta2 = 1.0 + 1.0 / std::pow(2.0, theta);
    zetan_ = zetan;
    alpha_ = 1.0 / (1.0 - theta);
    eta_ = (1.0 - std::pow(2.0 / items, 1.0 - theta)) / (1.0 - zeta2 / zetan);
    half_ = 1.0 + std::pow(0.5, theta);
  }

  template <class R>
  uint64_t operator()(R& random) const {
    const double u = std::uniform_real_distribution<double>(0.0, 1.0)(random);
    const double uz = u * zetan_;
    if (uz < 1.0) {
      return 0;
    }
    if (uz < half_) {
      return 1;
    }
    return std::min<uint64_t>(
        items_ - 1, uint64_t(items_ * std::pow(eta_ * u - eta_ + 1.0, alpha_)));
  }

 private:
  uint64_t items_;
  double zetan_ = 0, alpha_ = 0, eta_ = 0, half_ = 0;
};

uint64_t threadCpuNs() {
  timespec value{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value);
  return value.tv_sec * 1'000'000'000ULL + value.tv_nsec;
}

template <class F>
void parallelFor(uint32_t threads, uint64_t n, F&& f) {
  std::vector<std::thread> pool;
  for (uint32_t t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      for (uint64_t i = n * t / threads; i < n * (t + 1) / threads; ++i) {
        f(i);
      }
    });
  }
  for (auto& thread : pool) {
    thread.join();
  }
}

struct alignas(64) Worker {
  std::vector<uint64_t> cpuNs;
  uint64_t hits = 0;
  uint64_t checksum = 0;
};

void consume(const Cache::ReadHandle& handle, Worker& worker) {
  if (!handle) {
    return;
  }
  ++worker.hits;
  const auto* data = static_cast<const unsigned char*>(handle->getMemory());
  for (size_t i = 0; i < handle->getSize(); i += 64) {
    worker.checksum += data[i];
  }
}

} // namespace

int main(int argc, char** argv) {
  const folly::Init init(&argc, &argv);
  const bool many = FLAGS_mode == "findMany";
  XCHECK(many || FLAGS_mode == "find") << "--mode must be find or findMany";
  XCHECK(FLAGS_batch > 0 && FLAGS_keys_per_pass % FLAGS_batch == 0);
  XCHECK(FLAGS_zipf_theta >= 0 && FLAGS_zipf_theta < 1);
  XCHECK_LT(FLAGS_num_keys, 1ULL << 32);
  XCHECK_LE(FLAGS_batch, FLAGS_num_keys);

  Cache::Config config;
  config.setCacheSize(FLAGS_size_gb << 30);
  config.setAccessConfig(FLAGS_num_keys);
  config.reaperInterval = std::chrono::seconds(0);
  config.poolRebalanceInterval = std::chrono::seconds(0);
  Cache cache(config);
  Cache::MMConfig mmConfig;
  mmConfig.defaultLruRefreshTime = 900;
  mmConfig.lruRefreshTime = 900;
  const auto pool = cache.addPool(
      "default", cache.getCacheMemoryStats().ramCacheSize, {}, mmConfig);

  XLOG(INFO) << "populating " << FLAGS_num_keys << " items";
  std::atomic<uint64_t> failures{0};
  parallelFor(FLAGS_threads, FLAGS_num_keys, [&](uint64_t id) {
    auto handle = cache.allocate(pool, asKey(makeKey(id)), FLAGS_value_size);
    if (!handle) {
      failures.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    std::memset(handle->getMemory(), 'x', FLAGS_value_size);
    cache.insertOrReplace(handle);
  });
  XCHECK_EQ(0u, failures.load()) << "cache too small, increase --size_gb";
  // MMLru promotes an item on its first read regardless of the refresh time.
  parallelFor(FLAGS_threads, FLAGS_num_keys, [&](uint64_t id) {
    XCHECK(cache.find(asKey(makeKey(id))))
        << "item evicted, increase --size_gb";
  });

  std::vector<uint32_t> rankToItem(FLAGS_num_keys);
  std::iota(rankToItem.begin(), rankToItem.end(), uint32_t(0));
  std::shuffle(rankToItem.begin(), rankToItem.end(),
               std::mt19937_64(FLAGS_seed ^ 0x5eed));
  const Zipf zipf(FLAGS_num_keys, FLAGS_zipf_theta);

  const uint32_t passes = FLAGS_passes + 1;
  std::vector<Worker> workers(FLAGS_threads);
  std::vector<std::thread> threads;
  std::latch ready(FLAGS_threads), start(1);
  for (uint32_t t = 0; t < FLAGS_threads; ++t) {
    threads.emplace_back([&, t] {
      auto& worker = workers[t];
      std::mt19937_64 random(FLAGS_seed * 1000003 + t);
      std::vector<KeyBytes> sequence(FLAGS_keys_per_pass * passes);
      std::vector<uint32_t> window(FLAGS_batch);
      for (uint64_t base = 0; base < sequence.size(); base += FLAGS_batch) {
        for (uint32_t i = 0; i < FLAGS_batch; ++i) {
          uint32_t item;
          do {
            item = rankToItem[zipf(random)];
          } while (std::find(window.begin(), window.begin() + i, item) !=
                   window.begin() + i);
          window[i] = item;
          sequence[base + i] = makeKey(item);
        }
      }
      std::vector<Cache::Key> keys;
      keys.reserve(sequence.size());
      for (const auto& key : sequence) {
        keys.push_back(asKey(key));
      }
      std::vector<Cache::ReadHandle> handles(FLAGS_batch);
      worker.cpuNs.resize(passes);
      ready.count_down();
      start.wait();

      const Cache::Key* next = keys.data();
      for (uint32_t pass = 0; pass < passes; ++pass) {
        const auto started = threadCpuNs();
        for (uint64_t done = 0; done < FLAGS_keys_per_pass;
             done += FLAGS_batch) {
          if (many) {
            cache.findMany(folly::Range<const Cache::Key*>(next, FLAGS_batch),
                           folly::range(handles));
            for (auto& handle : handles) {
              consume(handle, worker);
              handle.reset();
            }
          } else {
            for (uint32_t i = 0; i < FLAGS_batch; ++i) {
              consume(cache.find(next[i]), worker);
            }
          }
          next += FLAGS_batch;
        }
        worker.cpuNs[pass] = threadCpuNs() - started;
      }
    });
  }
  ready.wait();
  XLOG(INFO) << "running " << FLAGS_mode;
  start.count_down();
  for (auto& thread : threads) {
    thread.join();
  }

  const uint64_t expected = FLAGS_keys_per_pass * passes;
  const uint64_t perHit = ((FLAGS_value_size + 63) / 64) * uint64_t('x');
  for (const auto& worker : workers) {
    XCHECK_EQ(expected, worker.hits);
    XCHECK_EQ(expected * perHit, worker.checksum);
  }
  for (uint32_t pass = 1; pass < passes; ++pass) {
    uint64_t cpuNs = 0;
    for (const auto& worker : workers) {
      cpuNs += worker.cpuNs[pass];
    }
    const double keys = double(FLAGS_keys_per_pass) * FLAGS_threads;
    XLOG(INFO) << fmt::format(
        "{} pass {}: {:.1f} ns CPU per key, {:.2f}M keys/s per core",
        FLAGS_mode, pass, cpuNs / keys, keys / cpuNs * 1e3);
  }
  return 0;
}
