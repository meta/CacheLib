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

#include "cachelib/allocator/MM2Q.h"
#include "cachelib/allocator/tests/MMTypeTest.h"

namespace facebook {
namespace cachelib {
using MM2QTest = MMTypeTest<MM2Q>;

TEST_F(MM2QTest, AddBasic) { testAddBasic(MM2Q::Config{}); }

TEST_F(MM2QTest, RemoveBasic) { testRemoveBasic(MM2Q::Config{}); }

TEST_F(MM2QTest, RemoveWithSmallQueues) {
  MM2Q::Config config{};
  config.coldSizePercent = 10;
  config.hotSizePercent = 35;
  config.lruRefreshTime = 0;
  Container c(config, {});
  std::vector<std::unique_ptr<Node>> nodes;
  createSimpleContainer(c, nodes);

  // access to move items from cold to warm
  for (auto& node : nodes) {
    ASSERT_TRUE(c.recordAccess(*node, AccessMode::kRead));
  }

  // trying to remove through iterator should work as expected.
  // no need of iter++ since remove will do that.
  verifyIterationVariants(c);
  for (auto iter = c.getEvictionIterator(); iter;) {
    auto& node = *iter;
    ASSERT_TRUE(node.isInMMContainer());

    // this will move the iter.
    c.remove(iter);
    ASSERT_FALSE(node.isInMMContainer());
    if (iter) {
      ASSERT_NE((*iter).getId(), node.getId());
    }
  }
  verifyIterationVariants(c);

  ASSERT_EQ(c.getStats().size, 0);
  for (const auto& node : nodes) {
    ASSERT_FALSE(node->isInMMContainer());
  }
}

TEST_F(MM2QTest, RecordAccessBasic) {
  MM2Q::Config c;
  // Change lruRefreshTime to make sure only the first recordAccess bumps
  // the node and subsequent recordAccess invocations do not.
  c.lruRefreshTime = 100;
  testRecordAccessBasic(std::move(c));
}

TEST_F(MM2QTest, RecordAccessWrites) {
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

  MM2Q::Config config1{/* lruRefreshTime */ 0,
                       /* lruRefreshRatio */ 0.,
                       /* updateOnWrite */ false,
                       /* updateOnRead */ false,
                       /* tryLockUpdate */ false,
                       /* updateOnRecordAccess */ true,
                       /* hotSizePercent */ 30,
                       /* coldSizePercent */ 30};
  Container c1{config1, {}};

  MM2Q::Config config2{/* lruRefreshTime */ 0,
                       /* lruRefreshRatio */ 0.,
                       /* updateOnWrite */ false,
                       /* updateOnRead */ true,
                       /* tryLockUpdate */ false,
                       /* updateOnRecordAccess */ true,
                       /* hotSizePercent */ 30,
                       /* coldSizePercent */ 30};
  Container c2{config2, {}};

  MM2Q::Config config3{/* lruRefreshTime */ 0,
                       /* lruRefreshRatio */ 0.,
                       /* updateOnWrite */ true,
                       /* updateOnRead */ false,
                       /* tryLockUpdate */ false,
                       /* updateOnRecordAccess */ true,
                       /* hotSizePercent */ 30,
                       /* coldSizePercent */ 30};
  Container c3{config3, {}};

  MM2Q::Config config4{/* lruRefreshTime */ 0,
                       /* lruRefreshRatio */ 0.,
                       /* updateOnWrite */ true,
                       /* updateOnRead */ true,
                       /* tryLockUpdate */ false,
                       /* updateOnRecordAccess */ true,
                       /* hotSizePercent */ 30,
                       /* coldSizePercent */ 30};
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

template <typename MMType>
void MMTypeTest<MMType>::testIterate(std::vector<std::unique_ptr<Node>>& nodes,
                                     Container& c) {
  verifyIterationVariants(c);
  auto it2q = c.getEvictionIterator();
  auto it = nodes.begin();
  while (it2q) {
    ASSERT_EQ(it2q->getId(), (*it)->getId());
    ++it2q;
    ++it;
  }
}

template <typename MMType>
void MMTypeTest<MMType>::testMatch(std::string expected,
                                   MMTypeTest<MMType>::Container& c) {
  std::string actual;
  verifyIterationVariants(c);
  auto it2q = c.getEvictionIterator();
  while (it2q) {
    actual +=
        fmt::format("{}:{}, ", it2q->getId(),
                    (c.isHot(*it2q) ? "H" : (c.isCold(*it2q) ? "C" : "W")));
    ++it2q;
  }
  ASSERT_EQ(expected, actual);
}

TEST_F(MM2QTest, DetailedTest) {
  MM2Q::Config config;
  config.lruRefreshTime = 0;
  config.updateOnWrite = false;
  config.hotSizePercent = 35;
  config.coldSizePercent = 30;
  // For 6 element, 35% gives us 2
  // elements in hot and warm cache

  Container c{config, {}};
  using Nodes = std::vector<std::unique_ptr<Node>>;
  Nodes nodes;

  int numNodes = 6;
  for (int i = 0; i < numNodes; i++) {
    nodes.emplace_back(new Node{i});
    auto& node = nodes.back();
    c.add(*node);
  }

  testIterate(nodes, c);

  testMatch("0:C, 1:C, 2:C, 3:C, 4:H, 5:H, ", c);
  // Move 3 to top of the hot cache
  c.recordAccess(*(nodes[4]), AccessMode::kRead);
  testMatch("0:C, 1:C, 2:C, 3:C, 5:H, 4:H, ", c);

  // Move 1 (cold) to warm cache
  c.recordAccess(*(nodes[1]), AccessMode::kRead);
  testMatch("0:C, 2:C, 3:C, 5:H, 4:H, 1:W, ", c);
  // Move 2 (cold) to warm cache
  c.recordAccess(*(nodes[2]), AccessMode::kRead);
  testMatch("0:C, 3:C, 5:H, 4:H, 1:W, 2:W, ", c);
  // Move 1 (warm) to top of warm cache
  c.recordAccess(*(nodes[1]), AccessMode::kRead);
  testMatch("0:C, 3:C, 5:H, 4:H, 2:W, 1:W, ", c);
  // Move 3 (cold) to top of warm cache
  c.recordAccess(*(nodes[3]), AccessMode::kRead);
  testMatch("0:C, 2:C, 5:H, 4:H, 1:W, 3:W, ", c);

  // Add 6 to hot cache.
  nodes.emplace_back(new Node{6});
  c.add(*(nodes[6]));
  testMatch("0:C, 2:C, 5:C, 4:H, 6:H, 1:W, 3:W, ", c);

  // Test arbitrary node deletion.
  // Delete a hot node
  c.remove(*(nodes[4]));
  nodes.erase(nodes.begin() + 4);
  testMatch("0:C, 2:C, 5:C, 6:H, 1:W, 3:W, ", c);

  // Delete a cold node
  c.remove(*(nodes[2]));
  nodes.erase(nodes.begin() + 2);
  testMatch("0:C, 5:C, 1:C, 6:H, 3:W, ", c);

  // Delete a warm node
  c.remove(*(nodes[2]));
  nodes.erase(nodes.begin() + 2);
  testMatch("0:C, 5:C, 1:C, 6:H, ", c);

  int listSize = nodes.size();
  for (auto it = nodes.begin(); it != nodes.end(); ++it) {
    ASSERT_EQ(c.remove(*(it->get())), true);
    ASSERT_EQ(c.size(), --listSize);
  }

  ASSERT_EQ(c.size(), 0);
}

TEST_F(MM2QTest, SegmentStress) {
  // given hot/cold segment sizes, run a finite number of random operations
  // on it and ensure that the tail sizes match what we expect it to be.
  auto doStressTest = [&](unsigned int hotSizePercent,
                          unsigned int coldSizePercent) {
    MM2Q::Config config;
    config.lruRefreshTime = 0;
    config.hotSizePercent = hotSizePercent;
    config.coldSizePercent = coldSizePercent;
    Container c{config, {}};

    constexpr auto nNodes = 500;
    using Nodes = std::vector<std::unique_ptr<Node>>;
    Nodes nodes;
    for (int i = 0; i < nNodes; i++) {
      nodes.emplace_back(new Node{i});
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

      if (!inLru.empty() && completedOps > 1000) {
        // to trigger a rebalance.
        const auto errorMargin = 2;

        const unsigned int actualColdPercent =
            (c.getEvictionAgeStat(0).coldQueueStat.size) * 100 / inLru.size();
        EXPECT_TRUE(actualColdPercent >= coldSizePercent - errorMargin &&
                    actualColdPercent <= coldSizePercent + errorMargin)
            << "Actual: " << actualColdPercent
            << ", Expected: " << coldSizePercent;
        const unsigned int actualHotPercent =
            c.getEvictionAgeStat(0).hotQueueStat.size * 100 / inLru.size();
        EXPECT_TRUE(actualHotPercent >= hotSizePercent - errorMargin &&
                    actualHotPercent <= hotSizePercent + errorMargin)
            << "Actual: " << actualHotPercent
            << ", Expected: " << hotSizePercent;
      }
    }
  };

  doStressTest(10, 60);
  doStressTest(20, 50);
  doStressTest(30, 45);
  doStressTest(40, 60);
  doStressTest(60, 40);
}

TEST_F(MM2QTest, Serialization) { testSerializationBasic(MM2Q::Config{}); }

TEST_F(MM2QTest, Reconfigure) {
  MM2Q::Config config;
  config.hotSizePercent = 0;
  config.coldSizePercent = 0;
  config.defaultLruRefreshTime = 0;
  config.lruRefreshTime = 0;
  config.lruRefreshRatio = 0.8;
  config.mmReconfigureIntervalSecs = std::chrono::seconds(4);
  Container container(config, {});
  std::vector<std::unique_ptr<Node>> nodes;
  nodes.emplace_back(new Node{0});
  container.add(*nodes[0]);
  container.recordAccess(*nodes[0], AccessMode::kRead); // promote 0 to warm
  sleep(1);
  nodes.emplace_back(new Node{1});
  container.add(*nodes[1]);
  container.recordAccess(*nodes[1], AccessMode::kRead); // promote 1 to warm
  sleep(2);

  // node 0 (age 3) gets promoted
  EXPECT_TRUE(container.recordAccess(*nodes[0], AccessMode::kRead));

  sleep(2);
  nodes.emplace_back(new Node{2});
  container.add(*nodes[2]);
  container.recordAccess(*nodes[2], AccessMode::kRead); // promote 2 to warm
  // set refresh time to 4 * 0.8 = 3.2 = 3
  container.recordAccess(*nodes[2], AccessMode::kRead);

  // node 0 (age 2) does not get promoted
  EXPECT_FALSE(container.recordAccess(*nodes[0], AccessMode::kRead));
}

TEST_F(MM2QTest, TailHits) {
  MM2Q::Config config;
  const size_t numItemInSlab = Slab::kSize / sizeof(Node);
  config.lruRefreshTime = 0;
  config.coldSizePercent = 50;
  config.hotSizePercent = 0;
  using LruType = MM2Q::LruType;

  // test with tail lists
  config.tailSize = numItemInSlab;
  {
    Container container(config, {});
    std::vector<std::unique_ptr<Node>> nodes;

    // add 2 slabs of items, which should all be in Cold
    for (uint32_t i = 0; i < numItemInSlab * 2; i++) {
      nodes.emplace_back(new Node{static_cast<int>(i)});
      container.add(*nodes[i]);
    }
    EXPECT_EQ(numItemInSlab, getListSize(container, LruType::ColdTail));
    EXPECT_EQ(numItemInSlab, getListSize(container, LruType::Cold));
    EXPECT_EQ(2 * numItemInSlab,
              container.getEvictionAgeStat(0).coldQueueStat.size);
    EXPECT_EQ(0, getListSize(container, LruType::WarmTail));
    EXPECT_EQ(0, getListSize(container, LruType::Warm));
    EXPECT_EQ(0, container.getEvictionAgeStat(0).warmQueueStat.size);

    // promote 1 slab of them to Warm tail
    for (uint32_t i = 0; i < numItemInSlab; i++) {
      container.recordAccess(*nodes[i], AccessMode::kRead);
    }
    EXPECT_EQ(numItemInSlab, getListSize(container, LruType::ColdTail));
    EXPECT_EQ(0, getListSize(container, LruType::Cold));
    EXPECT_EQ(numItemInSlab,
              container.getEvictionAgeStat(0).coldQueueStat.size);
    EXPECT_EQ(numItemInSlab, getListSize(container, LruType::WarmTail));
    EXPECT_EQ(0, getListSize(container, LruType::Warm));
    EXPECT_EQ(numItemInSlab,
              container.getEvictionAgeStat(0).warmQueueStat.size);

    // access all items, warm before cold
    // after rebalance, there should be half in warm tail, half in cold tail
    // (because warm can take at most 50% of items)
    for (uint32_t i = 0; i < numItemInSlab; i++) {
      container.recordAccess(*nodes[i], AccessMode::kRead);
    }
    for (uint32_t i = 0; i < numItemInSlab; i++) {
      container.recordAccess(*nodes[i + numItemInSlab], AccessMode::kRead);
    }
    EXPECT_EQ(numItemInSlab, getListSize(container, LruType::ColdTail));
    EXPECT_EQ(0, getListSize(container, LruType::Cold));
    EXPECT_EQ(numItemInSlab, getListSize(container, LruType::WarmTail));
    EXPECT_EQ(0, getListSize(container, LruType::Warm));

    // cold tail should have 2 * numItemInSlab hits, and warm tail numItemInslab
    // hits
    EXPECT_EQ((2 + 1) * numItemInSlab / 2,
              container.getStats().numTailAccesses);

    // cannot disable tail lists because a cold roll is required
    EXPECT_TRUE(container.tailTrackingEnabled_);
    config.addExtraConfig(0);
    ASSERT_EQ(0, config.tailSize);
    EXPECT_THROW(container.setConfig(config), std::invalid_argument);
  }

  // disable tail lists; we shouldn't have non-empty tails anymore
  config.tailSize = 0;
  {
    Container container(config, {});
    std::vector<std::unique_ptr<Node>> nodes;

    // add 2 slabs of items; tails should remain empty
    for (uint32_t i = 0; i < numItemInSlab * 2; i++) {
      nodes.emplace_back(new Node{static_cast<int>(i)});
      container.add(*nodes[i]);
    }
    EXPECT_EQ(0, getListSize(container, LruType::ColdTail));
    EXPECT_EQ(numItemInSlab * 2, getListSize(container, LruType::Cold));
    EXPECT_EQ(numItemInSlab * 2,
              container.getEvictionAgeStat(0).coldQueueStat.size);
    EXPECT_EQ(0, getListSize(container, LruType::WarmTail));
    EXPECT_EQ(0, getListSize(container, LruType::Warm));
    EXPECT_EQ(0, container.getEvictionAgeStat(0).warmQueueStat.size);

    // promote 1 slab of them to Warm
    for (uint32_t i = 0; i < numItemInSlab; i++) {
      container.recordAccess(*nodes[i], AccessMode::kRead);
    }
    EXPECT_EQ(0, getListSize(container, LruType::ColdTail));
    EXPECT_EQ(numItemInSlab, getListSize(container, LruType::Cold));
    EXPECT_EQ(0, getListSize(container, LruType::WarmTail));
    EXPECT_EQ(numItemInSlab, getListSize(container, LruType::Warm));

    config.addExtraConfig(1);
    ASSERT_FALSE(container.tailTrackingEnabled_);
    // cannot enable tail hits because cold roll is required
    EXPECT_THROW(container.setConfig(config), std::invalid_argument);
  }
}

// If we previously have only 3 lists for 2Q and now want to expand to 5
// without turning on tail hits tracking, we should be able to avoid cold
// roll by adjusting the lists to the correct position
TEST_F(MM2QTest, DeserializeToMoreLists) {
  MM2Q::Config config;
  config.lruRefreshTime = 0;
  config.coldSizePercent = 50;
  config.hotSizePercent = 30;
  const size_t numItems = 10;
  const size_t numItemsCold = numItems * config.coldSizePercent / 100;
  const size_t numItemsHot = numItems * config.hotSizePercent / 100;
  const size_t numItemsWarm =
      numItems * (100 - config.coldSizePercent - config.hotSizePercent) / 100;
  using LruType = MM2Q::LruType;
  std::vector<std::unique_ptr<Node>> nodes;

  Container c1(config, {});
  //
  // insert items to lists
  for (uint32_t i = 0; i < numItems; i++) {
    nodes.emplace_back(new Node{static_cast<int>(i)});
    c1.add(*nodes[i]);
  }
  for (uint32_t i = 0; i < numItemsWarm; i++) {
    c1.recordAccess(*nodes[i], AccessMode::kRead);
  }
  ASSERT_EQ(numItemsHot, c1.getEvictionAgeStat(0).hotQueueStat.size);
  ASSERT_EQ(numItemsCold, c1.getEvictionAgeStat(0).coldQueueStat.size);
  ASSERT_EQ(numItemsWarm, c1.getEvictionAgeStat(0).warmQueueStat.size);

  // move lists so that it only have 3 lists
  auto& lists = c1.lru_.lists_;
  std::swap(lists[1], lists[LruType::Hot]);  // move Hot to pos 1
  std::swap(lists[2], lists[LruType::Cold]); // move Cold to pos 2
  lists.pop_back();
  lists.pop_back();
  ASSERT_EQ(3, lists.size());

  // save state
  auto serializedData = c1.saveState();

  // deserialize to c2
  Container c2(serializedData, {});

  // check list sizes
  ASSERT_EQ(numItemsHot, c2.getEvictionAgeStat(0).hotQueueStat.size);
  ASSERT_EQ(numItemsCold, c2.getEvictionAgeStat(0).coldQueueStat.size);
  ASSERT_EQ(numItemsWarm, c2.getEvictionAgeStat(0).warmQueueStat.size);
}

TEST_F(MM2QTest, QueueAges) {
  MM2Q::Config config;
  config.lruRefreshTime = 0;
  config.coldSizePercent = 50;
  config.hotSizePercent = 30;
  const size_t numItems = 10;
  const size_t numItemsCold = numItems * config.coldSizePercent / 100;
  const size_t numItemsHot = numItems * config.hotSizePercent / 100;
  const size_t numItemsWarm = numItems - numItemsCold - numItemsHot;
  std::vector<std::unique_ptr<Node>> nodes;

  Container c1(config, {});
  for (uint32_t i = 0; i < numItems; i++) {
    nodes.emplace_back(new Node{static_cast<int>(i)});
    c1.add(*nodes[i]);
    std::this_thread::sleep_for(std::chrono::seconds{1});
  }
  // nodes ordering:
  //  hot: 9 -> 8 -> 7
  //  warm: none
  //  cold: 6 -> 5 -> 4 -> 3 -> 2 -> 1 -> 0
  ASSERT_EQ(3, c1.getEvictionAgeStat(0).hotQueueStat.oldestElementAge);
  ASSERT_EQ(0, c1.getEvictionAgeStat(0).warmQueueStat.oldestElementAge);
  ASSERT_EQ(10, c1.getEvictionAgeStat(0).coldQueueStat.oldestElementAge);

  for (uint32_t i = 0; i < numItemsWarm; i++) {
    c1.recordAccess(*nodes[i], AccessMode::kRead);
  }
  // nodes ordering:
  //  hot: 9 -> 8 -> 7
  //  warm: 1 -> 0
  //  cold: 6 -> 5 -> 4 -> 3 -> 2

  ASSERT_EQ(3, c1.getEvictionAgeStat(0).hotQueueStat.oldestElementAge);
  // warm-queue oldest element age is still 0 since it was just promoted
  ASSERT_EQ(0, c1.getEvictionAgeStat(0).warmQueueStat.oldestElementAge);
  ASSERT_EQ(8, c1.getEvictionAgeStat(0).coldQueueStat.oldestElementAge);
}

TEST_F(MM2QTest, TailTrackingEnabledCheck) {
  MM2Q::Config config;

  serialization::MultiDListObject lrus;

  {
    ASSERT_EQ(0, config.tailSize);
    Container c(config, {});
    // a new container created with zero tailSize should have tail tracking
    // disabled
    EXPECT_FALSE(c.tailTrackingEnabled_);
  }

  config.addExtraConfig(1);
  {
    Container c1(config, {});
    // a new container created with non-zero tailSize should have tail tracking
    // enabled.
    EXPECT_TRUE(c1.tailTrackingEnabled_);

    // and serialization should preserve that
    auto serializedData = c1.saveState();
    lrus = *serializedData.lrus();
    Container c2(serializedData, {});
    EXPECT_TRUE(c2.tailTrackingEnabled_);

    // disabling tail tracking should throw
    config.addExtraConfig(0);
    EXPECT_THROW(c2.setConfig(config), std::invalid_argument);
  }

  {
    serialization::MM2QObject serializedData;
    EXPECT_FALSE(*serializedData.tailTrackingEnabled());
    *serializedData.config()->hotSizePercent() = 10;
    *serializedData.config()->coldSizePercent() = 20;
    *serializedData.lrus() = lrus;

    // serialization object should by default have the flags dirty
    Container c(serializedData, {});
    EXPECT_FALSE(c.tailTrackingEnabled_);

    // cannot turn on tail hits tracking
    auto newConfig = c.getConfig();
    newConfig.addExtraConfig(2);

    EXPECT_THROW(c.setConfig(newConfig), std::invalid_argument);
  }
}

TEST_F(MM2QTest, CombinedLockingIteration) {
  MM2QTest::Config config{};
  config.useCombinedLockForIterators = true;
  config.lruRefreshTime = 0;
  Container c(config, {});
  std::vector<std::unique_ptr<Node>> nodes;
  createSimpleContainer(c, nodes);

  // access to move items from cold to warm
  for (auto& node : nodes) {
    ASSERT_TRUE(c.recordAccess(*node, AccessMode::kRead));
  }

  // trying to remove through iterator should work as expected.
  // no need of iter++ since remove will do that.
  verifyIterationVariants(c);
  for (auto iter = c.getEvictionIterator(); iter;) {
    auto& node = *iter;
    ASSERT_TRUE(node.isInMMContainer());

    // this will move the iter.
    c.remove(iter);
    ASSERT_FALSE(node.isInMMContainer());
    if (iter) {
      ASSERT_NE((*iter).getId(), node.getId());
    }
  }
  verifyIterationVariants(c);

  ASSERT_EQ(c.getStats().size, 0);
  for (const auto& node : nodes) {
    ASSERT_FALSE(node->isInMMContainer());
  }
}
} // namespace cachelib
} // namespace facebook
