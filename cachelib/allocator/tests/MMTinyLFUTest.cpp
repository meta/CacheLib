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

#include "cachelib/allocator/MMTinyLFU.h"
#include "cachelib/allocator/tests/MMTypeTest.h"

namespace facebook {
namespace cachelib {

using MMTinyLFUTest = MMTypeTest<MMTinyLFU>;

TEST_F(MMTinyLFUTest, AddBasic) { testAddBasic(MMTinyLFU::Config{}); }

TEST_F(MMTinyLFUTest, RemoveBasic) { testRemoveBasic(MMTinyLFU::Config{}); }

TEST_F(MMTinyLFUTest, RecordAccessBasic) {
  MMTinyLFU::Config c;
  // Change lruRefreshTime to make sure only the first recordAccess bumps
  // the node and subsequent recordAccess invocations do not.
  c.lruRefreshTime = 100;
  testRecordAccessBasic(std::move(c));
}

TEST_F(MMTinyLFUTest, RecordAccessWrites) {
  using Nodes = std::vector<std::unique_ptr<Node>>;
  // access the nodes in the container randomly with the given access mode and
  // ensure that nodes are updated in lru with access mode write (read) only
  // when updateOnWrite (updateOnRead) is enabled.

  auto testWithAccessMode = [this](Container& c_, const Nodes& nodes_,
                                   AccessMode mode, bool updateOnWrites,
                                   bool updateOnReads) {
    // accessing must at least update the update time. to do so, first set the
    // updateTime of the node to be in the past.
    const uint32_t timeInPastStart = 100;
    std::vector<uint32_t> prevNodeTime;
    int i = 0;
    for (auto& node : nodes_) {
      auto time = timeInPastStart + i;
      node->setUpdateTime(time);
      ASSERT_EQ(node->getUpdateTime(), time);
      prevNodeTime.push_back(time);
      i++;
    }

    std::vector<int> nodeOrderPrev;
    for (auto itr = c_.getEvictionIterator(); itr; ++itr) {
      nodeOrderPrev.push_back(itr->getId());
    }
    verifyIterationVariants(c_);

    int nAccess = 1000;
    std::set<int> accessedNodes;
    while (nAccess-- || accessedNodes.size() < nodes_.size()) {
      auto& node = *(nodes_.begin() + folly::Random::rand32() % nodes_.size());
      accessedNodes.insert(node->getId());
      c_.recordAccess(*node, mode);
    }

    i = 0;
    const auto now = util::getCurrentTimeSec();
    for (const auto& node : nodes_) {
      if ((mode == AccessMode::kWrite && updateOnWrites) ||
          (mode == AccessMode::kRead && updateOnReads)) {
        ASSERT_GT(node->getUpdateTime(), prevNodeTime[i++]);
        ASSERT_LE(node->getUpdateTime(), now);
      } else {
        ASSERT_EQ(node->getUpdateTime(), prevNodeTime[i++]);
      }
    }

    // after a random set of recordAccess, test the order of the nodes in the
    // lru.
    std::vector<int> nodeOrderCurr;
    for (auto itr = c_.getEvictionIterator(); itr; ++itr) {
      nodeOrderCurr.push_back(itr->getId());
    }
    verifyIterationVariants(c_);

    if ((mode == AccessMode::kWrite && updateOnWrites) ||
        (mode == AccessMode::kRead && updateOnReads)) {
      ASSERT_NE(nodeOrderCurr, nodeOrderPrev);
    } else {
      ASSERT_EQ(nodeOrderCurr, nodeOrderPrev);
    }
  };

  auto createNodes = [](Container& c, Nodes& nodes) {
    // put some nodes in the container and ensure that the recordAccess does not
    // change the fact that the node is still in container.
    const int numNodes = 10;
    for (int i = 0; i < numNodes; i++) {
      nodes.emplace_back(new Node{i});
      auto& node = nodes.back();
      ASSERT_TRUE(c.add(*node));
    }
  };

  MMTinyLFU::Config config1{/* lruRefreshTime */ 0,
                            /* updateOnWrite */ false,
                            /* updateOnRead */ false};
  Container c1{config1, {}};

  MMTinyLFU::Config config2{/* lruRefreshTime */ 0,
                            /* updateOnWrite */ false,
                            /* updateOnRead */ true};
  Container c2{config2, {}};

  MMTinyLFU::Config config3{/* lruRefreshTime */ 0,
                            /* updateOnWrite */ true,
                            /* updateOnRead */ false};
  Container c3{config3, {}};

  MMTinyLFU::Config config4{/* lruRefreshTime */ 0,
                            /* updateOnWrite */ true,
                            /* updateOnRead */ true};
  Container c4{config4, {}};

  Nodes nodes1, nodes2, nodes3, nodes4;
  createNodes(c1, nodes1);
  createNodes(c2, nodes2);
  createNodes(c3, nodes3);
  createNodes(c4, nodes4);

  testWithAccessMode(c1, nodes1, AccessMode::kWrite, config1.updateOnWrite,
                     config1.updateOnRead);
  testWithAccessMode(c1, nodes1, AccessMode::kRead, config1.updateOnWrite,
                     config1.updateOnRead);
  testWithAccessMode(c2, nodes2, AccessMode::kWrite, config2.updateOnWrite,
                     config2.updateOnRead);
  testWithAccessMode(c2, nodes2, AccessMode::kRead, config2.updateOnWrite,
                     config2.updateOnRead);
  testWithAccessMode(c3, nodes3, AccessMode::kWrite, config3.updateOnWrite,
                     config3.updateOnRead);
  testWithAccessMode(c3, nodes3, AccessMode::kRead, config3.updateOnWrite,
                     config3.updateOnRead);
  testWithAccessMode(c4, nodes4, AccessMode::kWrite, config4.updateOnWrite,
                     config4.updateOnRead);
  testWithAccessMode(c4, nodes4, AccessMode::kRead, config4.updateOnWrite,
                     config4.updateOnRead);
}

TEST_F(MMTinyLFUTest, TinyLFUBasic) {
  MMTinyLFU::Config config;
  config.lruRefreshTime = 0;
  config.updateOnWrite = false;
  config.tinySizePercent = 20;
  config.windowToCacheSizeRatio = 8;
  Container c{config, {}};

  constexpr auto nNodes = 10;
  using Nodes = std::vector<std::unique_ptr<Node>>;
  Nodes nodes;
  for (int i = 0; i < nNodes; i++) {
    nodes.emplace_back(new Node{i, folly::to<std::string>(i)});
  }

  auto checkTlfuConfig = [&](Container& container, std::string expected,
                             std::string context) {
    verifyIterationVariants(container);
    auto it = container.getEvictionIterator();
    std::string actual;
    while (it) {
      actual += fmt::format("{}:{}, ", it->getKey().str().c_str(),
                            (container.isTiny(*it) ? "T" : "M"));
      ++it;
    }
    ASSERT_EQ(expected, actual) << context;
  };

  // Insert all nodes
  for (size_t i = 0; i < nodes.size(); i++) {
    c.add(*nodes[i]);
  }

  // verify configuration.
  checkTlfuConfig(c, "2:M, 3:M, 4:M, 0:M, 5:M, 6:M, 7:M, 8:M, 1:T, 9:T, ",
                  "Check initial state");

  // Access an object in tiny and main cache.
  c.recordAccess(*nodes[2], AccessMode::kRead);
  c.recordAccess(*nodes[1], AccessMode::kRead);

  // The accessed items move to the head.
  checkTlfuConfig(c, "3:M, 4:M, 0:M, 5:M, 6:M, 7:M, 8:M, 9:T, 2:M, 1:T, ",
                  "Check after access");

  // Access another tiny item to push the main cache item to tail
  c.recordAccess(*nodes[9], AccessMode::kRead);

  // The tiny item should be last on eviction list.
  checkTlfuConfig(c, "3:M, 4:M, 0:M, 5:M, 6:M, 7:M, 8:M, 2:M, 1:T, 9:T, ",
                  "Check after promotion");

  // Access main cache tail items more than tiny cache tail item. Check that
  // there's no promotion.
  std::vector<int> mainItems = {3, 4, 0, 5, 6, 7, 8, 2};
  for (int j = 0; j < 2; j++) {
    for (size_t i = 0; i < mainItems.size(); i++) {
      ASSERT_EQ(false, c.isTiny(*nodes[mainItems[i]]));
      c.recordAccess(*nodes[mainItems[i]], AccessMode::kRead);
    }
  }

  // The tiny items are now candidates for eviction (at the tail for eviction).
  checkTlfuConfig(c, "1:T, 9:T, 3:M, 4:M, 0:M, 5:M, 6:M, 7:M, 8:M, 2:M, ",
                  "Check after no promotion");

  // remove all nodes
  for (size_t i = 0; i < nodes.size(); i++) {
    c.remove(*nodes[i]);
  }

  for (int i = 0; i < 5; i++) {
    c.add(*nodes[i]);
  }
  // Main cache nodes have higher frequency due to historical accesses.
  checkTlfuConfig(c, "1:M, 0:T, 2:M, 3:M, 4:M, ", "Check after adds");

  c.remove(*nodes[1]);
  checkTlfuConfig(c, "0:T, 2:M, 3:M, 4:M, ", "Check after remove from main");

  c.add(*nodes[1]);
  checkTlfuConfig(c, "1:T, 2:M, 3:M, 4:M, 0:M, ", "Check after one add");

  c.replace(*nodes[1], *nodes[6]);
  checkTlfuConfig(c, "6:T, 2:M, 3:M, 4:M, 0:M, ", "Check after replace");

  c.add(*nodes[1]);
  checkTlfuConfig(c, "2:M, 3:M, 4:M, 0:M, 6:M, 1:T, ",
                  "Check after replace and re-add");

  c.remove(*nodes[1]);
  checkTlfuConfig(c, "2:M, 3:M, 4:M, 0:M, 6:M, ",
                  "Check after remove from tiny");

  for (int i = 0; i < 7; i++) {
    c.remove(*nodes[i]);
  }

  checkTlfuConfig(c, "", "Check after removing all");

  // add all nodes
  for (size_t i = 2; i < nodes.size(); i++) {
    c.add(*nodes[i]);
  }

  checkTlfuConfig(c, "9:T, 3:M, 4:M, 5:M, 2:M, 6:M, 7:M, 8:M, ",
                  "Check state before save/restore");

  ////////////////// save restore ////////////////
  {
    // save state and restore
    const auto sizeBefore = c.getStats().size;
    auto serializedData = c.saveState();

    // newC should behave the same as c
    Container newC(serializedData, {});
    ASSERT_EQ(sizeBefore, newC.getStats().size);
    // We've lost the frequency counts after save restore.
    checkTlfuConfig(newC, "3:M, 4:M, 5:M, 2:M, 6:M, 7:M, 8:M, 9:T, ",
                    "Check state after save/restore");

    // try adding at the head of tiny cache
    newC.add(*nodes[1]);
    checkTlfuConfig(newC, "3:M, 4:M, 5:M, 2:M, 6:M, 7:M, 8:M, 9:M, 1:T, ",
                    "Check state after restore and add");

    // few more operations
    newC.remove(*nodes[5]);
    checkTlfuConfig(newC, "3:M, 4:M, 2:M, 6:M, 7:M, 8:M, 9:M, 1:T, ",
                    "Check state after restore and remove");

    // clear nodes
    for (size_t i = 0; i < nodes.size(); i++) {
      newC.remove(*nodes[i]);
    }
    checkTlfuConfig(newC, "",
                    "Check after removing all from restored container");
  }
}

TEST_F(MMTinyLFUTest, SegmentStress) {
  auto doStressTest = [&](size_t windowToCacheSizeRatio,
                          size_t tinySizePercent) {
    MMTinyLFU::Config config;
    config.lruRefreshTime = 0;
    config.windowToCacheSizeRatio = windowToCacheSizeRatio;
    config.tinySizePercent = tinySizePercent;
    Container c{config, {}};

    constexpr auto nNodes = 500;
    using Nodes = std::vector<std::unique_ptr<Node>>;
    Nodes nodes;
    for (int i = 0; i < nNodes; i++) {
      nodes.emplace_back(new Node{i, folly::to<std::string>(i)});
    }

    // list of nodes currently in lru
    std::unordered_set<int> inLru;

    // add a random node into the lru that is not present.
    // the lru must not be full at this point.
    auto addRandomNode = [&]() {
      if (inLru.size() >= nNodes) {
        return;
      }
      auto n = folly::Random::rand32() % nNodes;
      while (inLru.count(n) != 0) {
        n = folly::Random::rand32() % nNodes;
      }
      c.add(*nodes[n]);
      assert(inLru.count(n) == 0);
      inLru.insert(n);
    };

    // removes a random node that is present in the lru.
    auto removeRandomNode = [&]() {
      if (inLru.empty()) {
        return;
      }

      auto n = folly::Random::rand32() % nNodes;
      while (inLru.count(n) == 0) {
        n = folly::Random::rand32() % nNodes;
      }
      c.remove(*nodes[n]);
      assert(inLru.count(n) != 0);
      inLru.erase(n);
    };

    // on a non-empty lru, bump up a random node
    auto recordRandomNode = [&]() {
      if (inLru.empty()) {
        return;
      }

      auto n = folly::Random::rand32() % nNodes;
      while (inLru.count(n) == 0) {
        n = folly::Random::rand32() % nNodes;
      }
      c.recordAccess(*nodes[n], AccessMode::kRead);
    };

    int opsToComplete = 100000;
    folly::ThreadLocalPRNG prng = folly::ThreadLocalPRNG();
    std::mt19937 gen(folly::Random::rand32(prng));
    std::uniform_real_distribution<> opDis(0, 1);

    // probabilities for different operation.
    const double addPct = 0.4;
    const double recordAccesPct = 0.9;
    const double removePct = 0.2;
    int completedOps = 0;

    while (++completedOps < opsToComplete) {
      auto op = opDis(gen);
      if (inLru.size() < nNodes && op < addPct) {
        addRandomNode();
      }

      if (op < removePct && !inLru.empty()) {
        removeRandomNode();
      }

      if (op < recordAccesPct && !inLru.empty()) {
        recordRandomNode();
      }

      if (!inLru.empty()) {
        const auto errorMargin = 1;
        int actualTinySize = c.lru_.getList(MMTinyLFU::LruType::Tiny).size();
        int expectedTinySize = inLru.size() * tinySizePercent / 100;
        EXPECT_TRUE(actualTinySize >= expectedTinySize - errorMargin &&
                    actualTinySize <= expectedTinySize + errorMargin)
            << "Actual: " << actualTinySize
            << ", Expected: " << expectedTinySize << " total: " << inLru.size();
      }
    }
  };

  doStressTest(8, 1);
  doStressTest(16, 5);
  doStressTest(32, 10);
  doStressTest(64, 20);
  doStressTest(128, 40);
}

TEST_F(MMTinyLFUTest, Serialization) {
  testSerializationBasic(MMTinyLFU::Config{});
}

TEST_F(MMTinyLFUTest, Reconfigure) {
  Container container(MMTinyLFU::Config{}, {});
  auto config = container.getConfig();
  config.defaultLruRefreshTime = 1;
  config.lruRefreshTime = 1;
  config.lruRefreshRatio = 0.8;
  config.mmReconfigureIntervalSecs = std::chrono::seconds(2);
  container.setConfig(config);
  std::vector<std::unique_ptr<Node>> nodes;
  nodes.emplace_back(new Node{0});
  container.add(*nodes[0]);
  sleep(2);
  nodes.emplace_back(new Node{1});
  container.add(*nodes[1]);
  sleep(2);

  // node 0 (age 3) gets promoted
  // upon access, refresh time changed from 1 to 3 (4 * 0.8)
  EXPECT_TRUE(container.recordAccess(*nodes[0], AccessMode::kRead));

  sleep(2);
  nodes.emplace_back(new Node{2});
  container.add(*nodes[2]);

  // refresh time 3, node 0 (age 2) does not get promoted
  EXPECT_FALSE(container.recordAccess(*nodes[0], AccessMode::kRead));
}
} // namespace cachelib
} // namespace facebook
