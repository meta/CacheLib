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

#pragma once
#include <fmt/core.h>
#include <folly/Random.h>

#include <memory>
#include <set>
#include <vector>

#include "cachelib/allocator/KAllocation.h"
#include "cachelib/allocator/tests/TestBase.h"

namespace facebook {
namespace cachelib {
namespace tests {
template <typename AccessType>
struct AccessTypeTest : public SlabAllocatorTestBase {
  // dummy implementation of a node that can be put inside the
  // AccessType::Container.
  struct Node {
    // use the implementation of key type we already have.
    using Key = KAllocation::Key;
    using Hasher = MurmurHash2;
    struct Releaser {
      void operator()(Node* node) const noexcept { node->decRef(); }
    };

    using Handle = std::unique_ptr<Node, Releaser>;
    using HandleMaker = std::function<Handle(Node*)>;

    explicit Node(const std::string& s) : key_(s) {}
    explicit Node(Key key) : key_(key.data(), key.size()) {}

    Key getKey() const { return {key_.data(), key_.length()}; }

    template <typename Hasher>
    typename Hasher::Ret getHash() const noexcept {
      return Hasher()(key_.data(), key_.length());
    }

    bool isAccessible() const noexcept { return accessible_; }
    unsigned int getRefCount() const noexcept { return refcount_; }

    void incRef() noexcept { refcount_++; }
    void decRef() noexcept { refcount_--; }

    std::string toString() const {
      return fmt::format(" key = {}, ref = {}, accessible = {}", key_,
                         refcount_.load(), accessible_);
    }

    void triggerHandleException(bool state) {
      shouldTriggerHandleException_ = state;
    }

    bool shouldTriggerHandleException() const {
      return shouldTriggerHandleException_;
    }

   protected:
    friend AccessTypeTest<AccessType>;

    void markAccessible() noexcept {
      ASSERT_FALSE(accessible_);
      accessible_ = true;
    }

    void unmarkAccessible() noexcept {
      ASSERT_TRUE(accessible_);
      accessible_ = false;
    }

   public:
    // Node does not perform pointer compression, but it needs to supply a dummy
    // PtrCompressor
    using CompressedPtrType = Node*;
    struct PtrCompressor {
      constexpr CompressedPtrType compress(Node* uncompressed) const {
        return uncompressed;
      }

      constexpr Node* unCompress(CompressedPtrType compressed) const {
        return compressed;
      }
    };
    // member hook
    typename AccessType::template Hook<Node> accessHook_;

   private:
    std::string key_;
    bool accessible_{false};
    std::atomic<unsigned int> refcount_{0};
    friend typename AccessType::template Container<
        Node,
        &Node::accessHook_,
        RWBucketLocks<folly::SharedMutex>>;
    std::atomic<bool> shouldTriggerHandleException_{false};
  };

  using Container = typename AccessType::template Container<
      Node,
      &Node::accessHook_,
      RWBucketLocks<folly::SharedMutex>>;
  using Config = typename AccessType::Config;
  using CompressedPtrType = typename Node::CompressedPtrType;
  using PtrCompressor = typename Node::PtrCompressor;
  using HandleMaker = typename Node::HandleMaker;

  std::string getRandomNewKey(const Container& c) {
    auto key = getRandomStr();
    while (c.find(key) != nullptr) {
      key = getRandomStr();
    }
    return key;
  }

  // create a basic container with 1000 nodes
  std::vector<std::unique_ptr<Node>> createSimpleContainer(Container& c);

  // iterate the container and return the keys found.
  std::set<std::string> iterateAndGetKeys(Container& c);

  void checkNodeAccessible(const Node& n) { ASSERT_TRUE(n.isAccessible()); }
  void checkNodeInaccessible(const Node& n) { ASSERT_FALSE(n.isAccessible()); }

  void testSimpleInsertAndRemove(Container& c,
                                 std::vector<std::unique_ptr<Node>>& nodes);

  void testInsert();
  void testReplace();
  void testRemove();
  void testFind();
  void testSerialization();
  void testHandleContexts();
  void testRemoveIf();

  // test iterator where you could get null handles from the handle maker
  void testHandleIterationWithExceptions();

  // basic creation of iterators.
  void testIteratorBasic();

  // run iterators while modifying the contatiner
  void testIteratorWithInserts();

  // try to iterate while calling saveState()
  void testIteratorWithSerialization();

  // test iterator when handler maker returns null
  void testIteratorMayContainNull();
};

template <typename AccessType>
std::vector<std::unique_ptr<typename AccessTypeTest<AccessType>::Node>>
AccessTypeTest<AccessType>::createSimpleContainer(Container& c) {
  std::vector<std::unique_ptr<Node>> nodes;

  for (int i = 0; i < 1000; i++) {
    auto key = getRandomNewKey(c);
    nodes.emplace_back(new Node(key));
    auto& node = *nodes.back();
    checkNodeInaccessible(node);
    c.insert(node);
    checkNodeAccessible(node);
  }

  return nodes;
}

template <typename AccessType>
std::set<std::string> AccessTypeTest<AccessType>::iterateAndGetKeys(
    Container& c) {
  std::set<std::string> visitedKeys;
  for (const auto& node : c) {
    visitedKeys.insert(node.getKey().str());
  }
  return visitedKeys;
}

template <typename AccessType>
void AccessTypeTest<AccessType>::testSimpleInsertAndRemove(
    Container& c, std::vector<std::unique_ptr<Node>>& nodes) {
  // trying to insert duplicates should fail.
  for (const auto& node : nodes) {
    std::unique_ptr<Node> newNode{new Node(node->getKey())};
    ASSERT_EQ(node->getKey(), newNode->getKey());
    ASSERT_EQ(c.find(node->getKey()), node);
    ASSERT_FALSE(c.insert(*newNode));
    // should not change anything unless it was inserted.
    ASSERT_FALSE(newNode->isAccessible());
  }

  // should be able to insert again after removing from the container.
  for (auto& node : nodes) {
    auto oldNode = std::move(node);
    node.reset(new Node(oldNode->getKey()));
    ASSERT_EQ(node->getKey(), oldNode->getKey());
    ASSERT_EQ(c.find(oldNode->getKey()), oldNode);
    ASSERT_FALSE(node->isAccessible());
    ASSERT_TRUE(oldNode->isAccessible());
    ASSERT_TRUE(c.remove(*oldNode));
    ASSERT_FALSE(oldNode->isAccessible());
    // insert the newly created one.
    ASSERT_TRUE(c.insert(*node));
    ASSERT_TRUE(node->isAccessible());
  }
}

template <typename AccessType>
void AccessTypeTest<AccessType>::testInsert() {
  Container c;
  auto nodes = createSimpleContainer(c);
  testSimpleInsertAndRemove(c, nodes);
}

template <typename AccessType>
void AccessTypeTest<AccessType>::testReplace() {
  Container c;
  auto nodes = createSimpleContainer(c);

  // duplicate the nodes and replace the ones in container.
  std::vector<std::unique_ptr<Node>> duplicatedNodes;
  for (auto& existingNode : nodes) {
    duplicatedNodes.emplace_back(new Node(existingNode->getKey()));
    auto& newNode = *duplicatedNodes.back();

    // new node should be inaccessible and
    // cannot be inserted into the container
    checkNodeInaccessible(newNode);
    ASSERT_FALSE(c.insert(newNode));
    checkNodeInaccessible(newNode);

    // but we can replace the old node with this new node
    ASSERT_EQ(c.insertOrReplace(newNode), existingNode);
    checkNodeAccessible(newNode);

    // the old node should no longer be accessible
    checkNodeInaccessible(*existingNode);
  }
}

template <typename AccessType>
void AccessTypeTest<AccessType>::testRemove() {
  Container c;
  auto nodes = createSimpleContainer(c);

  for (auto& node : nodes) {
    ASSERT_EQ(c.find(node->getKey()), node);
    ASSERT_TRUE(node->isAccessible());
    c.remove(*node);
    ASSERT_FALSE(node->isAccessible());
    ASSERT_EQ(c.find(node->getKey()), nullptr);
    // removing a node twice should return error.
    ASSERT_FALSE(c.remove(*node));
    ASSERT_FALSE(node->isAccessible());
  }

  int ntries = 100;
  while (ntries--) {
    auto key = getRandomNewKey(c);
    Node newNode{key};
    ASSERT_FALSE(newNode.isAccessible());
    ASSERT_FALSE(c.remove(newNode));
    ASSERT_FALSE(newNode.isAccessible());
  }
}

template <typename AccessType>
void AccessTypeTest<AccessType>::testFind() {
  Container c;
  auto nodes = createSimpleContainer(c);

  // should be able to find the nodes and find should bump up the refcount.
  for (auto& node : nodes) {
    auto oldCount = node->getRefCount();
    {
      auto nodeHandle = c.find(node->getKey());
      ASSERT_EQ(nodeHandle, node);
      ASSERT_EQ(node->getRefCount(), oldCount + 1);
    }
    // once we release the node handle, the refcount should drop to old one.
    ASSERT_EQ(node->getRefCount(), oldCount);
  }

  // refcount should be bumped up how many ever times we call find
  int ntries = 1000;
  const auto& node = nodes[0];
  const auto oldCount = node->getRefCount();
  const auto expectedCount = oldCount + ntries;
  {
    std::vector<typename Node::Handle> handles;
    while (ntries--) {
      auto handle = c.find(node->getKey());
      ASSERT_EQ(handle, node);
      handles.push_back(std::move(handle));
    }
    ASSERT_EQ(node->getRefCount(), expectedCount);
  }
  // once all the handles are released, the count should go back.
  ASSERT_EQ(node->getRefCount(), oldCount);
}

template <typename AccessType>
void AccessTypeTest<AccessType>::testSerialization() {
  Config config;
  std::unique_ptr<CompressedPtrType[]> memStart(
      new CompressedPtrType[config.getNumBuckets()]);
  size_t hashTableSize = sizeof(CompressedPtrType) * config.getNumBuckets();
  memset(memStart.get(), 0, hashTableSize);

  Container c1(config,
               reinterpret_cast<Node**>(memStart.get()),
               typename Node::PtrCompressor());
  auto nodes = createSimpleContainer(c1);

  testSimpleInsertAndRemove(c1, nodes);

  auto originalNumKeys = c1.getNumKeys();
  auto originalDistributionStats = c1.getDistributionStats();
  // save and restore container
  auto serializedData = c1.saveState();

  // c2 should behave the same as c1
  Container c2(serializedData,
               config,
               reinterpret_cast<Node**>(memStart.get()),
               hashTableSize,
               typename Node::PtrCompressor());

  auto testContainer = [&](Container& c) {
    for (auto& node : nodes) {
      auto handle = c.find(node->getKey());
      ASSERT_NE(nullptr, handle);
    }

    const auto restoredNumKeys = c.getNumKeys();
    ASSERT_EQ(originalNumKeys, restoredNumKeys);
    const auto restoredDistributionStats = c.getDistributionStats();
    ASSERT_EQ(originalDistributionStats.numBuckets,
              restoredDistributionStats.numBuckets);
    ASSERT_EQ(originalDistributionStats.itemDistribution,
              restoredDistributionStats.itemDistribution);
    testSimpleInsertAndRemove(c, nodes);
  };

  testContainer(c2);
  originalNumKeys = c2.getNumKeys();
  serializedData = c2.saveState();

  // now try to increas the locks. That should work.
  Container c3(serializedData,
               {config.getBucketsPower(), config.getLocksPower() + 3},
               reinterpret_cast<Node**>(memStart.get()), hashTableSize,
               typename Node::PtrCompressor());
  ASSERT_EQ(config.getBucketsPower(), c3.getConfig().getBucketsPower());
  ASSERT_EQ(config.getLocksPower() + 3, c3.getConfig().getLocksPower());
  testContainer(c3);

  // now try to change the bucket power and that should fail.
  serializedData = c3.saveState();
  ASSERT_THROW(
      Container(serializedData,
                {config.getBucketsPower() + 1, config.getLocksPower() + 3},
                reinterpret_cast<Node**>(memStart.get()), hashTableSize,
                typename Node::PtrCompressor()),
      std::invalid_argument);

  ASSERT_THROW(
      Container(serializedData,
                {config.getBucketsPower() - 1, config.getLocksPower() + 3},
                reinterpret_cast<Node**>(memStart.get()), hashTableSize,
                typename Node::PtrCompressor()),
      std::invalid_argument);
}

template <typename AccessType>
void AccessTypeTest<AccessType>::testIteratorWithSerialization() {
  Config config;
  std::unique_ptr<CompressedPtrType[]> memStart(
      new CompressedPtrType[config.getNumBuckets()]);
  memset(memStart.get(), 0,
         sizeof(CompressedPtrType*) * config.getNumBuckets());
  Container c{std::move(config), reinterpret_cast<Node**>(memStart.get()),
              typename Node::PtrCompressor()};

  auto nodes = createSimpleContainer(c);
  testSimpleInsertAndRemove(c, nodes);

  // should not be able to save the state when there is an iteration
  // happening.
  {
    auto iter = c.begin();
    ASSERT_THROW(c.saveState(), std::logic_error);
  }

  c.saveState();
}

template <typename AccessType>
void AccessTypeTest<AccessType>::testIteratorBasic() {
  Container c;

  // empty container should not have anything when you iterate.
  ASSERT_EQ(0, iterateAndGetKeys(c).size());

  auto nodes = createSimpleContainer(c);

  std::set<std::string> existingKeys;
  for (const auto& node : nodes) {
    existingKeys.insert(node->getKey().str());
  }

  // all nodes must be present in the absence of any writes.
  ASSERT_EQ(existingKeys, iterateAndGetKeys(c));

  std::set<std::string> visitedKeys1;
  std::set<std::string> visitedKeys2;
  // start two iterators and visit them with interleaving.
  auto iter1 = c.begin();
  while (iter1 != c.end() && visitedKeys1.size() < nodes.size() / 2) {
    visitedKeys1.insert(iter1->getKey().str());
    ++iter1;
  }

  auto iter2 = c.begin();
  while (iter2 != c.end() && visitedKeys2.size() < nodes.size() / 2) {
    visitedKeys2.insert(iter2->getKey().str());
    ++iter2;
  }

  while (iter1 != c.end() || iter2 != c.end()) {
    auto rand = folly::Random::rand32();
    if (iter1 != c.end() && rand % 2) {
      visitedKeys1.insert(iter1->getKey().str());
      ++iter1;
    }

    if (iter2 != c.end() && rand % 3) {
      visitedKeys2.insert(iter2->getKey().str());
      ++iter2;
    }
  }
  ASSERT_EQ(visitedKeys1, visitedKeys2);
  ASSERT_EQ(existingKeys, visitedKeys1);
}

template <typename AccessType>
void AccessTypeTest<AccessType>::testIteratorWithInserts() {
  Container c;
  auto nodes = createSimpleContainer(c);

  auto existingKeys = iterateAndGetKeys(c);

  std::vector<std::unique_ptr<Node>> deletedNodes;
  // start two iterators and visit them with interleaving.
  auto iter = c.begin();
  std::set<std::string> visitedKeys;
  while (iter != c.end() && visitedKeys.size() < nodes.size() / 2) {
    visitedKeys.insert(iter->getKey().str());
    ++iter;
  }

  std::set<std::string> insertedKeys;
  std::set<std::string> deletedKeys;
  std::vector<std::unique_ptr<Node>> newNodes;
  while (iter != c.end()) {
    auto rand = folly::Random::rand32() % 11;
    if (rand > 3) {
      visitedKeys.insert(iter->getKey().str());
      ++iter;
    } else if (rand > 1) {
      auto key = getRandomNewKey(c);
      newNodes.emplace_back(new Node(key));
      auto& node = *newNodes.back();
      ASSERT_TRUE(c.insert(node));
      insertedKeys.insert(key);
    } else {
      auto& node = *nodes.back();
      ASSERT_TRUE(c.remove(node));
      deletedKeys.insert(node.getKey().str());
      deletedNodes.push_back(std::move(nodes.back()));
      ASSERT_EQ(nullptr, nodes.back());
      nodes.pop_back();
    }
  }

  // all existing keys must have been visited, excecpt for deleted keys.
  for (const auto& key : existingKeys) {
    if (deletedKeys.find(key) != deletedKeys.end()) {
      continue;
    }
    ASSERT_NE(visitedKeys.end(), visitedKeys.find(key));
  }

  auto newKeys = iterateAndGetKeys(c);
  // new iteration should not find any deleted keys and must find all new
  // inserted keys as well.
  for (const auto& key : existingKeys) {
    if (deletedKeys.find(key) != deletedKeys.end()) {
      ASSERT_EQ(newKeys.end(), newKeys.find(key));
    } else {
      ASSERT_NE(newKeys.end(), newKeys.find(key));
    }
  }

  for (const auto& key : insertedKeys) {
    ASSERT_NE(nullptr, c.find(key));
    ASSERT_NE(newKeys.end(), newKeys.find(key));
  }
}

template <typename AccessType>
void AccessTypeTest<AccessType>::testHandleIterationWithExceptions() {
  Config config;
  using Handle = typename Node::Handle;
  unsigned int attempts = 0;
  unsigned int numNullHandles = 0;
  typename Node::HandleMaker h = [&](Node* n) {
    if (!n) {
      return Handle{nullptr};
    }

    if (++attempts % 2) {
      n->incRef();
      return Handle{n};
    } else {
      ++numNullHandles;
      throw exception::RefcountOverflow("");
    }
  };

  Container c(std::move(config), typename Node::PtrCompressor(), h);
  auto nodes = createSimpleContainer(c);
  auto existingKeys = iterateAndGetKeys(c);

  ASSERT_LT(existingKeys.size(), nodes.size());
  ASSERT_GT(existingKeys.size(), 0);
  ASSERT_NE(0, numNullHandles);
}

template <typename AccessType>
void AccessTypeTest<AccessType>::testRemoveIf() {
  Container c;
  auto nodes = createSimpleContainer(c);

  auto truePredicate = [](const Node&) { return true; };
  auto falsePredicate = [](const Node&) { return false; };

  for (auto& node : nodes) {
    ASSERT_EQ(c.find(node->getKey()), node);
    ASSERT_TRUE(node->isAccessible());

    auto h = c.removeIf(*node, truePredicate);
    ASSERT_EQ(h.get(), node.get());

    ASSERT_FALSE(node->isAccessible());

    ASSERT_EQ(c.find(node->getKey()), nullptr);

    // removing a node twice should return error.
    ASSERT_EQ(nullptr, c.removeIf(*node, truePredicate));
    ASSERT_FALSE(node->isAccessible());
  }

  for (auto& node : nodes) {
    ASSERT_TRUE(c.insert(*node));
  }

  for (auto& node : nodes) {
    ASSERT_EQ(c.find(node->getKey()), node);
    ASSERT_TRUE(node->isAccessible());

    auto h = c.removeIf(*node, falsePredicate);
    // fails
    ASSERT_EQ(h.get(), nullptr);

    ASSERT_TRUE(node->isAccessible());

    ASSERT_EQ(c.find(node->getKey()).get(), node.get());
  }
}

template <typename AccessType>
void AccessTypeTest<AccessType>::testIteratorMayContainNull() {
  std::string keyToReturnNull{};
  Container c{Config{}, PtrCompressor{},
              [&keyToReturnNull](Node* node) -> typename Node::Handle {
                if (!node) {
                  return nullptr;
                }
                if (node->getKey() == typename Node::Key{keyToReturnNull}) {
                  return nullptr;
                }
                node->incRef();
                return typename Node::Handle{node};
              }};

  // empty container should not have anything when you iterate.
  ASSERT_EQ(0, iterateAndGetKeys(c).size());

  auto nodes = createSimpleContainer(c);

  std::set<std::string> existingKeys;
  for (const auto& node : nodes) {
    existingKeys.insert(node->getKey().str());
  }

  // all nodes must be present in the absence of any writes.
  ASSERT_EQ(existingKeys, iterateAndGetKeys(c));

  // Mark one key for the handle maker to return null
  keyToReturnNull = *existingKeys.begin();

  std::set<std::string> visitedKeys1;
  std::set<std::string> visitedKeys2;
  // start two iterators and visit them with interleaving.
  auto iter1 = c.begin();
  while (iter1 != c.end() && visitedKeys1.size() < nodes.size() / 2) {
    visitedKeys1.insert(iter1->getKey().str());
    ++iter1;
  }

  auto iter2 = c.begin();
  while (iter2 != c.end() && visitedKeys2.size() < nodes.size() / 2) {
    visitedKeys2.insert(iter2->getKey().str());
    ++iter2;
  }

  while (iter1 != c.end() || iter2 != c.end()) {
    auto rand = folly::Random::rand32();
    if (iter1 != c.end() && rand % 2) {
      visitedKeys1.insert(iter1->getKey().str());
      ++iter1;
    }

    if (iter2 != c.end() && rand % 3) {
      visitedKeys2.insert(iter2->getKey().str());
      ++iter2;
    }
  }
  ASSERT_EQ(visitedKeys1, visitedKeys2);
  existingKeys.erase(keyToReturnNull);
  ASSERT_EQ(existingKeys, visitedKeys1);
}
} // namespace tests
} // namespace cachelib
} // namespace facebook
