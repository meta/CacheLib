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

#include <atomic>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <folly/Format.h>
#pragma GCC diagnostic pop

#include <folly/container/Array.h>
#include <folly/lang/Align.h>
#include <folly/synchronization/DistributedMutex.h>

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/CacheStats.h"
#include "cachelib/allocator/Util.h"
#include "cachelib/allocator/datastruct/MultiDList.h"
#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"

namespace facebook::cachelib {

// S3-FIFO eviction policy with two FIFO queues: Small and Main.
//
// New items enter the Small queue. On access (recordAccess), items that are accessed
// are marked.
//
// Eviction follows the S3-FIFO paper: when Small exceeds `smallSizePercent`
// of total size, the Small tail is processed — accessed items are promoted
// to Main, unaccessed items become eviction victims. When Small is within
// target, eviction comes from Main's tail, after item in main's tail are
// reinserted to head of main if they are accessed.
class MMS3FIFO {
 public:
  // unique identifier per MMType
  static const int kId;

  template <typename T>
  using Hook = DListHook<T>;
  using SerializationType = serialization::MMS3FIFOObject;
  using SerializationConfigType = serialization::MMS3FIFOConfig;
  using SerializationTypeContainer = serialization::MMS3FIFOCollection;

  // Main=0, Small=1: MultiDList rbegin() starts at highest index (Small)
  // tail first, then Main tail — giving correct S3-FIFO eviction order.
  enum LruType { Main, Small, NumTypes };

  struct Config {
    explicit Config(SerializationConfigType configState)
        : Config(*configState.lruRefreshTime(),
                 *configState.lruRefreshRatio(),
                 *configState.updateOnWrite(),
                 *configState.updateOnRead(),
                 *configState.smallSizePercent(),
                 *configState.ghostSizePercent()) {}

    Config(bool updateOnR, size_t smallSizePct, size_t ghostSizePct)
        : Config(/* time */ 60,
                 /* ratio */ 0.0,
                 /* updateOnW */ false,
                 updateOnR,
                 smallSizePct,
                 ghostSizePct,
                 /* mmReconfigureInterval */ 0,
                 /* useCombinedLockForIterators */ false) {}

    Config(uint32_t time,
           double ratio,
           bool updateOnW,
           bool updateOnR,
           size_t smallSizePct,
           size_t ghostSizePct)
        : Config(time,
                 ratio,
                 updateOnW,
                 updateOnR,
                 smallSizePct,
                 ghostSizePct,
                 /* mmReconfigureInterval */ 0,
                 /* useCombinedLockForIterators */ false) {}

    Config(uint32_t time,
           double ratio,
           bool updateOnW,
           bool updateOnR,
           size_t smallSizePct,
           size_t ghostSizePct,
           uint32_t mmReconfigureInterval,
           bool useCombinedLockForIterators)
        : defaultLruRefreshTime(time),
          lruRefreshRatio(ratio),
          updateOnWrite(updateOnW),
          updateOnRead(updateOnR),
          smallSizePercent(smallSizePct),
          ghostSizePercent(ghostSizePct),
          mmReconfigureIntervalSecs(
              std::chrono::seconds(mmReconfigureInterval)),
          useCombinedLockForIterators(useCombinedLockForIterators) {
      checkConfig();
    }

    Config() = default;
    Config(const Config& rhs) = default;
    Config(Config&& rhs) = default;

    Config& operator=(const Config& rhs) = default;
    Config& operator=(Config&& rhs) = default;

    void checkConfig() {
      if (smallSizePercent < 1 || smallSizePercent > 50) {
        throw std::invalid_argument(
            folly::sformat("Invalid small queue size {}. Small queue size "
                           "must be between 1% and 50% of total cache size.",
                           smallSizePercent));
      }
    }

    template <typename... Args>
    void addExtraConfig(Args...) {}

    uint32_t defaultLruRefreshTime{60};
    uint32_t lruRefreshTime{defaultLruRefreshTime};

    double lruRefreshRatio{0.};

    bool updateOnWrite{false};

    bool updateOnRead{true};

    // The size of the Small queue as a percentage of the total size.
    size_t smallSizePercent{10};

    // Reserved for ghost queue sizing. Stored and serialized even though the
    // ghost queue itself is not wired into eviction behavior yet.
    size_t ghostSizePercent{100};

    std::chrono::seconds mmReconfigureIntervalSecs{};

    bool useCombinedLockForIterators{false};
  };

  template <typename T, Hook<T> T::* HookPtr>
  struct Container {
   private:
    using LruList = MultiDList<T, HookPtr>;
    using Mutex = folly::DistributedMutex;
    using LockHolder = std::unique_lock<Mutex>;
    using PtrCompressor = typename T::PtrCompressor;
    using Time = typename Hook<T>::Time;
    using CompressedPtrType = typename T::CompressedPtrType;
    using RefFlags = typename T::Flags;

   public:
    Container() = default;
    Container(Config c, PtrCompressor compressor)
        : lru_(LruType::NumTypes, std::move(compressor)),
          config_(std::move(c)) {}
    Container(serialization::MMS3FIFOObject object, PtrCompressor compressor);

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    using Iterator = typename LruList::Iterator;

    class LockedIterator : public Iterator {
     public:
      LockedIterator(const LockedIterator&) = delete;
      LockedIterator& operator=(const LockedIterator&) = delete;

      LockedIterator(LockedIterator&&) noexcept = default;

      void destroy() {
        Iterator::reset();
        if (l_.owns_lock()) {
          l_.unlock();
        }
      }

      void resetToBegin() {
        if (!l_.owns_lock()) {
          l_.lock();
        }
        Iterator::resetToBegin();
      }

     private:
      LockedIterator& operator=(LockedIterator&&) noexcept = default;

      LockedIterator(LockHolder l, const Iterator& iter) noexcept;

      friend Container<T, HookPtr>;

      LockHolder l_;
    };

    // In S3-FIFO, recordAccess lazily marks Small items as accessed
    // (no lock, no list ops). Promotion to Main is deferred to eviction
    // time. Main is pure FIFO — recordAccess is a no-op.
    bool recordAccess(T& node, AccessMode mode) noexcept;

    // Adds the node to the Small queue head.
    bool add(T& node) noexcept;

    bool remove(T& node) noexcept;

    void remove(Iterator& it) noexcept;

    bool replace(T& oldNode, T& newNode) noexcept;

    LockedIterator getEvictionIterator() const noexcept;

    template <typename F>
    void withEvictionIterator(F&& f);

    template <typename F>
    void withContainerLock(F&& f);

    Config getConfig() const;

    void setConfig(const Config& newConfig);

    bool isEmpty() const noexcept { return size() == 0; }

    size_t size() const noexcept {
      return lruMutex_->lock_combine([this]() { return lru_.size(); });
    }

    EvictionAgeStat getEvictionAgeStat(uint64_t projectedLength) const noexcept;

    serialization::MMS3FIFOObject saveState() const noexcept;

    MMContainerStat getStats() const noexcept;

    static LruType getLruType(const T& node) noexcept {
      return isSmall(node) ? LruType::Small : LruType::Main;
    }

   private:
    EvictionAgeStat getEvictionAgeStatLocked(
        uint64_t projectedLength) const noexcept;

    static Time getUpdateTime(const T& node) noexcept {
      return (node.*HookPtr).getUpdateTime();
    }

    static void setUpdateTime(T& node, Time time) noexcept {
      (node.*HookPtr).setUpdateTime(time);
    }

    void removeLocked(T& node) noexcept;

    // Lazy promotion: when Small exceeds smallSizePercent, scan Small tail
    // and promote accessed items to Main. Called under lock before yielding
    // the eviction iterator. Const-safe because lru_ is mutable.
    void lazyPromoteSmallTailLocked() const noexcept;

    void lazyReinsertMainTailLocked() const noexcept;

    static uint32_t getKeyHash(const T& node) noexcept {
      return static_cast<uint32_t>(
          folly::hasher<folly::StringPiece>()(node.getKey()));
    }

    void ghostInsert(uint32_t keyHash) const noexcept {
      ghostSet_.insert(keyHash);
      ghostFifo_.push_back(keyHash);
      const size_t ghostMax =
          std::max<size_t>(1, lru_.size() * config_.ghostSizePercent / 100);
      while (ghostFifo_.size() > ghostMax) {
        ghostSet_.erase(ghostFifo_.front());
        ghostFifo_.pop_front();
      }
    }

    bool ghostContainsAndErase(uint32_t keyHash) const noexcept {
      return ghostSet_.erase(keyHash) > 0;
    }


    // Flag helpers: kMMFlag0 = "in Small queue"
    static bool isSmall(const T& node) noexcept {
      return node.template isFlagSet<RefFlags::kMMFlag0>();
    }
    static void markSmall(T& node) noexcept {
      node.template setFlag<RefFlags::kMMFlag0>();
    }
    static void unmarkSmall(T& node) noexcept {
      node.template unSetFlag<RefFlags::kMMFlag0>();
    }

    // Flag helpers: kMMFlag1 = "accessed" (for stats)
    static bool isAccessed(const T& node) noexcept {
      return node.template isFlagSet<RefFlags::kMMFlag1>();
    }
    static void markAccessed(T& node) noexcept {
      node.template setFlag<RefFlags::kMMFlag1>();
    }
    static void unmarkAccessed(T& node) noexcept {
      node.template unSetFlag<RefFlags::kMMFlag1>();
    }


    mutable folly::F14FastSet<uint32_t> ghostSet_;
    mutable std::deque<uint32_t> ghostFifo_;

    mutable folly::cacheline_aligned<Mutex> lruMutex_;

    mutable LruList lru_{};

    Config config_{};

    friend class MMTypeTest<MMS3FIFO>;
  };
};


/* Container Interface Implementation */
template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
MMS3FIFO::Container<T, HookPtr>::Container(serialization::MMS3FIFOObject object,
                                           PtrCompressor compressor)
    : lru_(*object.lrus(), std::move(compressor)), config_(*object.config()) {}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
bool MMS3FIFO::Container<T, HookPtr>::recordAccess(T& node,
                                                   AccessMode mode) noexcept {
  if ((mode == AccessMode::kWrite && !config_.updateOnWrite) ||
      (mode == AccessMode::kRead && !config_.updateOnRead)) {
    return false;
  }

  const auto curr = static_cast<Time>(util::getCurrentTimeSec());

  if (!node.isInMMContainer()) {
    return false;
  }

  // For Small items: lazily mark as accessed. Promotion to Main is
  // deferred to eviction time (lazyPromoteSmallTailLocked).
  if (isSmall(node)) {
    if (isAccessed(node)) {
      return false;
    }
    markAccessed(node);
    setUpdateTime(node, curr);
    return true;
  }

  // Throttle: if already accessed and recently updated, skip.
  if (isAccessed(node) && curr < getUpdateTime(node) + config_.lruRefreshTime) {
    return false;
  }

  markAccessed(node);
  setUpdateTime(node, curr);
  return true;
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
bool MMS3FIFO::Container<T, HookPtr>::add(T& node) noexcept {
  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());

  auto keyHash = getKeyHash(node);
  return lruMutex_->lock_combine([this, &node, currTime, keyHash]() {
    if (node.isInMMContainer()) {
      return false;
    }

    if (ghostContainsAndErase(keyHash)) {
      // Ghost hit: insert directly to Main (skip Small)
      lru_.getList(LruType::Main).linkAtHead(node);
    } else {
      lru_.getList(LruType::Small).linkAtHead(node);
      markSmall(node);
    }
    node.markInMMContainer();
    setUpdateTime(node, currTime);
    unmarkAccessed(node);

    return true;
  });
}


template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
void MMS3FIFO::Container<T, HookPtr>::lazyPromoteSmallTailLocked()
    const noexcept {
  const auto totalSize = lru_.size();
  if (totalSize == 0) {
    return;
  }
  const auto targetSmallSize = totalSize * config_.smallSizePercent / 100;
  auto& smallList = lru_.getList(LruType::Small);

  // Only process Small when it exceeds target size.
  // At stable state, this op is constant time.
  while (smallList.size() > targetSmallSize) {
    auto* tail = smallList.getTail();
    if (!tail) {
      break;
    }
    if (isAccessed(*tail)) {
      // Accessed: promote to Main
      smallList.remove(*tail);
      lru_.getList(LruType::Main).linkAtHead(*tail);
      unmarkSmall(*tail);
      unmarkAccessed(*tail);
    } else {
      break; // Unaccessed: leave as eviction victim
    }
  }
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
void MMS3FIFO::Container<T, HookPtr>::lazyReinsertMainTailLocked()
    const noexcept {
  auto& mainList = lru_.getList(LruType::Main);
  auto* tail = mainList.getTail();
  if (tail == nullptr || !isAccessed(*tail)) {
    return;
  }

  auto* cur = tail;
  auto* first = tail;

  // Find the contiguous accessed suffix at Main tail and clear access bits.
  while (cur && isAccessed(*cur)) {
    unmarkAccessed(*cur);
    first = cur;
    cur = mainList.getPrev(*cur);
  }

  // Move that entire suffix to head in one splice.
  // Reinsertion is expensive in a workload with high hit ratio.
  mainList.moveSuffixToHead(*first);
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
typename MMS3FIFO::Container<T, HookPtr>::LockedIterator
MMS3FIFO::Container<T, HookPtr>::getEvictionIterator() const noexcept {
  LockHolder l(*lruMutex_);
  lazyPromoteSmallTailLocked();

  const auto totalSize = lru_.size();
  const auto targetSmallSize =
      totalSize == 0 ? 0 : totalSize * config_.smallSizePercent / 100;
  if (lru_.getList(LruType::Small).size() > targetSmallSize || lru_.getList(LruType::Main).size() == 0) {
    // Small exceeds target — evict from Small first
    return LockedIterator{std::move(l), lru_.rbegin()};
  }
  lazyReinsertMainTailLocked();
  // Small within target — evict from Main (skip Small)
  return LockedIterator{std::move(l), lru_.rbegin(LruType::Main)};
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
template <typename F>
void MMS3FIFO::Container<T, HookPtr>::withEvictionIterator(F&& fun) {
  auto makeItr = [this]() {
    lazyPromoteSmallTailLocked();
    const auto totalSize = lru_.size();
    const auto targetSmallSize =
        totalSize == 0 ? 0 : totalSize * config_.smallSizePercent / 100;
    if (lru_.getList(LruType::Small).size() > targetSmallSize || lru_.getList(LruType::Main).size() == 0) {
      return Iterator{lru_.rbegin()};
    }
    lazyReinsertMainTailLocked();
    return Iterator{lru_.rbegin(LruType::Main)};
  };

  if (config_.useCombinedLockForIterators) {
    lruMutex_->lock_combine([&fun, &makeItr]() { fun(makeItr()); });
  } else {
    LockHolder lck{*lruMutex_};
    fun(makeItr());
  }
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
template <typename F>
void MMS3FIFO::Container<T, HookPtr>::withContainerLock(F&& fun) {
  lruMutex_->lock_combine([&fun]() { fun(); });
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
void MMS3FIFO::Container<T, HookPtr>::removeLocked(T& node) noexcept {
  if (isSmall(node)) {
    lru_.getList(LruType::Small).remove(node);
    unmarkSmall(node);
  } else {
    lru_.getList(LruType::Main).remove(node);
  }
  unmarkAccessed(node);
  node.unmarkInMMContainer();
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
bool MMS3FIFO::Container<T, HookPtr>::remove(T& node) noexcept {
  return lruMutex_->lock_combine([this, &node]() {
    if (!node.isInMMContainer()) {
      return false;
    }
    removeLocked(node);
    return true;
  });
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
void MMS3FIFO::Container<T, HookPtr>::remove(Iterator& it) noexcept {
  T& node = *it;
  XDCHECK(node.isInMMContainer());
  // Eviction from Small: record key hash in ghost so re-insertions
  if (isSmall(node)) {
    ghostInsert(getKeyHash(node));
  }
  ++it;
  removeLocked(node);
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
bool MMS3FIFO::Container<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  return lruMutex_->lock_combine([this, &oldNode, &newNode]() {
    if (!oldNode.isInMMContainer() || newNode.isInMMContainer()) {
      return false;
    }
    const auto updateTime = getUpdateTime(oldNode);

    if (isSmall(oldNode)) {
      lru_.getList(LruType::Small).replace(oldNode, newNode);
      unmarkSmall(oldNode);
      markSmall(newNode);
    } else {
      lru_.getList(LruType::Main).replace(oldNode, newNode);
    }

    oldNode.unmarkInMMContainer();
    newNode.markInMMContainer();
    setUpdateTime(newNode, updateTime);
    if (isAccessed(oldNode)) {
      markAccessed(newNode);
    } else {
      unmarkAccessed(newNode);
    }
    return true;
  });
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
typename MMS3FIFO::Config MMS3FIFO::Container<T, HookPtr>::getConfig() const {
  return lruMutex_->lock_combine([this]() { return config_; });
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
void MMS3FIFO::Container<T, HookPtr>::setConfig(const Config& c) {
  lruMutex_->lock_combine([this, &c]() { config_ = c; });
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
cachelib::EvictionAgeStat MMS3FIFO::Container<T, HookPtr>::getEvictionAgeStat(
    uint64_t projectedLength) const noexcept {
  return lruMutex_->lock_combine([this, projectedLength]() {
    return getEvictionAgeStatLocked(projectedLength);
  });
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
cachelib::EvictionAgeStat
MMS3FIFO::Container<T, HookPtr>::getEvictionAgeStatLocked(
    uint64_t projectedLength) const noexcept {
  EvictionAgeStat stat{};
  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());

  // Determine which queue would be evicted from, matching
  // getEvictionIterator logic. Report that queue in warmQueueStat
  // since getOldestElementAge() reads warmQueueStat.
  const auto totalSize = lru_.size();
  const auto targetSmallSize =
      totalSize == 0 ? 0 : totalSize * config_.smallSizePercent / 100;
  const auto& smallList = lru_.getList(LruType::Small);
  const bool evictFromSmall = smallList.size() > targetSmallSize;

  // warmQueueStat = eviction candidate queue (what would actually be evicted)
  {
    auto& list = evictFromSmall ? lru_.getList(LruType::Small)
                                : lru_.getList(LruType::Main);
    auto it = list.rbegin();
    stat.warmQueueStat.oldestElementAge =
        it != list.rend() ? currTime - getUpdateTime(*it) : 0;
    stat.warmQueueStat.size = list.size();
    for (size_t numSeen = 0; numSeen < projectedLength && it != list.rend();
         ++numSeen, ++it) {
    }
    stat.warmQueueStat.projectedAge = it != list.rend()
                                          ? currTime - getUpdateTime(*it)
                                          : stat.warmQueueStat.oldestElementAge;
  }

  // coldQueueStat = the other queue
  {
    auto& list = evictFromSmall ? lru_.getList(LruType::Main)
                                : lru_.getList(LruType::Small);
    auto it = list.rbegin();
    stat.coldQueueStat.oldestElementAge =
        it != list.rend() ? currTime - getUpdateTime(*it) : 0;
    stat.coldQueueStat.size = list.size();
    for (size_t numSeen = 0; numSeen < projectedLength && it != list.rend();
         ++numSeen, ++it) {
    }
    stat.coldQueueStat.projectedAge = it != list.rend()
                                          ? currTime - getUpdateTime(*it)
                                          : stat.coldQueueStat.oldestElementAge;
  }

  return stat;
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
serialization::MMS3FIFOObject MMS3FIFO::Container<T, HookPtr>::saveState()
    const noexcept {
  serialization::MMS3FIFOConfig configObject;
  *configObject.updateOnWrite() = config_.updateOnWrite;
  *configObject.updateOnRead() = config_.updateOnRead;
  *configObject.smallSizePercent() = config_.smallSizePercent;
  *configObject.ghostSizePercent() = config_.ghostSizePercent;
  *configObject.lruRefreshTime() = config_.lruRefreshTime;
  *configObject.lruRefreshRatio() = config_.lruRefreshRatio;

  serialization::MMS3FIFOObject object;
  *object.config() = configObject;
  *object.lrus() = lru_.saveState();
  return object;
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
MMContainerStat MMS3FIFO::Container<T, HookPtr>::getStats() const noexcept {
  auto stat = lruMutex_->lock_combine([this]() {
    // Report eviction age from the queue that would actually be evicted
    // from, matching getEvictionIterator logic. When Small exceeds target,
    // eviction comes from Small tail; otherwise from Main tail.
    // This prevents HitsPerSlabStrategy from seeing artificially short
    // eviction ages for classes with few items in Small.
    T* tail = nullptr;
    if (lru_.size() > 0) {
      const auto totalSize = lru_.size();
      const auto targetSmallSize = totalSize * config_.smallSizePercent / 100;
      const auto& smallList = lru_.getList(LruType::Small);
      if (smallList.size() > targetSmallSize) {
        tail = smallList.getTail();
      }
      if (!tail) {
        tail = lru_.getList(LruType::Main).getTail();
      }
    }
    return folly::make_array(lru_.size(),
                             tail == nullptr ? 0 : getUpdateTime(*tail),
                             static_cast<uint32_t>(0),
                             lru_.getList(LruType::Small).size(),
                             lru_.getList(LruType::Main).size());
  });
  // numHotAccesses = Small queue size, numColdAccesses = Main queue size
  return {stat[0], stat[1], stat[2], stat[3], stat[4], 0, 0};
}

// LockedIterator constructor
template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
MMS3FIFO::Container<T, HookPtr>::LockedIterator::LockedIterator(
    LockHolder l, const Iterator& iter) noexcept
    : Iterator(iter), l_(std::move(l)) {}
} // namespace facebook::cachelib
