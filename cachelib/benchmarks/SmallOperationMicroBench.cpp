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

/*
clang-format off
Microbenchmarks for variou small operations
Results are at the bottom of this file

Various latency numbers circa 2012
----------------------------------
L1 cache reference                           0.5 ns
Branch mispredict                            5   ns
L2 cache reference                           7   ns                      14x L1 cache
Mutex lock/unlock                           25   ns
Main memory reference                      100   ns                      20x L2 cache, 200x L1 cache
Compress 1K bytes with Zippy             3,000   ns        3 us
Send 1K bytes over 1 Gbps network       10,000   ns       10 us
Read 4K randomly from SSD*             150,000   ns      150 us          ~1GB/sec SSD
Read 1 MB sequentially from memory     250,000   ns      250 us
Round trip within same datacenter      500,000   ns      500 us
Read 1 MB sequentially from SSD*     1,000,000   ns    1,000 us    1 ms  ~1GB/sec SSD, 4X memory
Disk seek                           10,000,000   ns   10,000 us   10 ms  20x datacenter roundtrip
Read 1 MB sequentially from disk    20,000,000   ns   20,000 us   20 ms  80x memory, 20X SSD
Send packet CA->Netherlands->CA    150,000,000   ns  150,000 us  150 ms
clang-format on
*/

#include <fmt/core.h>
#include <fmt/format.h>
#include <folly/Benchmark.h>
#include <folly/BenchmarkUtil.h>
#include <folly/init/Init.h>
#include <folly/memory/MallctlHelper.h>
#include <folly/memory/Malloc.h>

#include <iostream>
#include <random>

#include "cachelib/benchmarks/BenchmarkUtils.h"
#include "cachelib/common/AtomicCounter.h"
#include "cachelib/common/BytesEqual.h"
#include "cachelib/common/PercentileStats.h"
#include "cachelib/navy/testing/SeqPoints.h"

namespace facebook {
namespace cachelib {
void randomGenCost() {
  constexpr uint64_t kOps = 10'000'000;
  std::mt19937 gen;
  std::uniform_int_distribution<uint64_t> dist(0, 10000);

  {
    Timer t{"Random Number", kOps};
    for (uint64_t i = 0; i < kOps; i++) {
      auto num = dist(gen);
      folly::doNotOptimizeAway(num);
    }
  }
}

void takeTimeStamp() {
  constexpr uint64_t kOps = 10'000'000;
  {
    Timer t{"Timestamp", kOps};
    for (uint64_t i = 0; i < kOps; i++) {
      auto now = std::chrono::system_clock::now();
      folly::doNotOptimizeAway(now);
    }
  }
}

void takeTimeStampSecGranularity() {
  constexpr uint64_t kOps = 10'000'000;
  {
    Timer t{"Timestamp In Seconds", kOps};
    for (uint64_t i = 0; i < kOps; i++) {
      auto now = std::time(nullptr);
      folly::doNotOptimizeAway(now);
    }
  }
}

void takeCpuCycles() {
  constexpr uint64_t kOps = 10'000'000;
  {
    Timer t{"CPU Cycles", kOps};
    for (uint64_t i = 0; i < kOps; i++) {
      uint64_t tsc = Timer::cycles();
      folly::doNotOptimizeAway(tsc);
    }
  }
}

void percentileStatsAdd() {
  constexpr uint64_t kOps = 10'000'000;
  util::PercentileStats stats;
  {
    Timer t{"PercentileStats Add", kOps};
    for (uint64_t i = 0; i < kOps; i++) {
      double d = 1.2345;
      folly::doNotOptimizeAway(d);
      stats.trackValue(d);
    }
  }
}

void atomicCounterAdd() {
  constexpr uint64_t kThreads = 32;
  constexpr uint64_t kOps = 10'000'000;
  AtomicCounter atomicCounter;
  navy::SeqPoints sp;
  auto addStats = [&]() {
    sp.wait(0);
    for (uint64_t j = 0; j < kOps; j++) {
      atomicCounter.inc();
    }
  };

  std::vector<std::thread> ts;
  for (uint64_t i = 0; i < kThreads; i++) {
    ts.emplace_back(addStats);
  }

  {
    Timer timer{"AtomicCounter Add", kOps};
    sp.reached(0);
    for (auto& t : ts) {
      t.join();
    }
  }
}

void tlCounterAdd() {
  constexpr uint64_t kThreads = 32;
  constexpr uint64_t kOps = 10'000'000;
  TLCounter tlCounter;
  navy::SeqPoints sp;
  auto addStats = [&]() {
    sp.wait(0);
    for (uint64_t j = 0; j < kOps; j++) {
      tlCounter.inc();
    }
  };

  std::vector<std::thread> ts;
  for (uint64_t i = 0; i < kThreads; i++) {
    ts.emplace_back(addStats);
  }

  {
    Timer timer{"TLCounter Add", kOps};
    sp.reached(0);
    for (auto& t : ts) {
      t.join();
    }
  }
}

void createSmallString() {
  // Just a simple test to figure out cost of creating a small string
  constexpr uint64_t kOps = 10'000'000;
  {
    Timer t{"Create Small String", kOps};
    for (uint64_t i = 0; i < kOps; i++) {
      // Read from the vector of strings (first memory load)
      auto key = fmt::format("{: <10}", i);
      folly::doNotOptimizeAway(key);
    }
  }
}

void compareString(int len) {
  // Just a simple test to figure out cost of reading a string
  constexpr uint64_t kOps = 10'000'000;
  constexpr uint64_t kObjects = 100'000;
  std::vector<std::string> keys;
  const std::string keyCopy(len, 'a');
  for (uint64_t i = 0; i < kObjects; i++) {
    keys.push_back(keyCopy);
  }

  std::mt19937 gen;
  std::uniform_int_distribution<int> dist(0, kObjects - 1);
  {
    Timer t{fmt::format("Read String - {: <3}", len), kOps};
    for (uint64_t i = 0; i < kOps; i++) {
      // Read from the vector of strings (first memory load)
      auto& k1 = keys[dist(gen)];
      auto& k2 = keys[dist(gen)];
      auto equal = bytesEqual(k1.data(), k2.data(), k1.size());
      folly::doNotOptimizeAway(equal);
    }
  }
}

void callMallctl() {
  constexpr uint64_t kOps = 10'000'000;
  {
    Timer t{"Mallctl", kOps};
    for (uint64_t i = 0; i < kOps; i++) {
      uint64_t mallocedBytes;
      folly::mallctlRead("thread.allocated", &mallocedBytes);
      folly::doNotOptimizeAway(mallocedBytes);
    }
  }
}
} // namespace cachelib
} // namespace facebook

using namespace facebook::cachelib;

int main(int argc, char** argv) {
  const folly::Init init(&argc, &argv);

  printMsg("Benchmark various small operations");
  randomGenCost();
  takeTimeStamp();
  takeTimeStampSecGranularity();
  takeCpuCycles();
  percentileStatsAdd();
  atomicCounterAdd();
  tlCounterAdd();
  createSmallString();
  compareString(1);
  compareString(10);
  compareString(100);
  callMallctl();
  printMsg("Benchmarks have completed");
}

/*
clang-format off
Hardware Spec: T1 Skylake

Architecture:        x86_64
CPU op-mode(s):      32-bit, 64-bit
Byte Order:          Little Endian
CPU(s):              36
On-line CPU(s) list: 0-35
Thread(s) per core:  2
Core(s) per socket:  18
Socket(s):           1
NUMA node(s):        1
Vendor ID:           GenuineIntel
CPU family:          6
Model:               85
Model name:          Intel(R) Xeon(R) D-2191A CPU @ 1.60GHz
Stepping:            4
CPU MHz:             1855.037
CPU max MHz:         1601.0000
CPU min MHz:         800.0000
BogoMIPS:            3200.00
Virtualization:      VT-x
L1d cache:           32K
L1i cache:           32K
L2 cache:            1024K
L3 cache:            25344K
NUMA node0 CPU(s):   0-35

-------- Benchmark various small operations --------------------------------------------------------
[Random Number                                               ] Per-Op: 18    ns, 29    cycles
[Timestamp                                                   ] Per-Op: 24    ns, 38    cycles
[Timestamp In Seconds                                        ] Per-Op: 2     ns, 4     cycles
[CPU Cycles                                                  ] Per-Op: 10    ns, 16    cycles
[PercentileStats Add                                         ] Per-Op: 87    ns, 140   cycles
[AtomicCounter Add                                           ] Per-Op: 833   ns, 1329  cycles
[TLCounter Add                                               ] Per-Op: 4     ns, 7     cycles
[Create Small String                                         ] Per-Op: 83    ns, 134   cycles
[Read String - 1                                             ] Per-Op: 46    ns, 74    cycles
[Read String - 10                                            ] Per-Op: 50    ns, 80    cycles
[Read String - 100                                           ] Per-Op: 108   ns, 173   cycles
[Mallctl                                                     ] Per-Op: 63    ns, 151   cycles
-------- Benchmarks have completed -----------------------------------------------------------------
clang-format on
*/
