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
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <utility>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <folly/Format.h>
#pragma GCC diagnostic pop

#include <folly/container/Array.h>
#include <folly/lang/Align.h>
#include <folly/logging/xlog.h>
#include <folly/container/EvictingCacheMap.h>
#include <folly/synchronization/DistributedMutex.h>

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/CacheStats.h"
#include "cachelib/allocator/Util.h"
#include "cachelib/allocator/datastruct/MultiDList.h"
#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"

namespace facebook::cachelib {

// ============================================================================
// S3FIFO Feature Collection Structures
// ============================================================================

constexpr int32_t kS3FIFOMaxBuckets = 64;
constexpr int32_t kS3FIFODefaultBuckets = 20;

// Feature vector fed to the ML model.
struct S3FIFOFeatureVector {
  int32_t numBuckets{kS3FIFODefaultBuckets};
  double logCacheCapacity{0.0};

  // Spatial hit distribution by queue.
  double hitRatioSmall{0.0};
  double hitRatioMain{0.0};
  double hitRatioGhost{0.0};

  // Workload proxies.
  double uniqueRatio{0.0};  // unique_objs / total_requests
  double oneHitRatio{0.0};  // one_hit_wonders / unique_objs

  int64_t totalRequests{0};
  int64_t totalHits{0};
  int64_t totalMisses{0};
  int64_t hitsSmall{0};
  int64_t hitsMain{0};
  int64_t hitsGhost{0};

  // Position-in-queue histograms (normalized to sum to 1).
  double histSmall[kS3FIFOMaxBuckets]{};
  double histMain[kS3FIFOMaxBuckets]{};
  double histGhost[kS3FIFOMaxBuckets]{};
};

// Bucketed hit-position tracker. Converts (insertCounter, currentCounter)
// into a histogram bucket in O(1). For ghost queue, also tracks mid-queue
// removals ("holes") so position is corrected.
struct S3FIFOHitPosTracker {
  int32_t numBuckets{kS3FIFODefaultBuckets};
  int64_t bucketSize{1};

  int64_t hitCounts[kS3FIFOMaxBuckets]{};
  int64_t totalHits{0};

  bool trackMiddleRemoval{false};
  int64_t removalCounters[kS3FIFOMaxBuckets]{};
  int64_t totalRemovals{0};
  int64_t currentBucket{0};

  bool haveWarmedUp{false};

  void init(int64_t expectedMaxPos, int32_t buckets, bool trackRemoval) {
    if (buckets <= 0) {
      buckets = kS3FIFODefaultBuckets;
    }
    if (buckets > kS3FIFOMaxBuckets) {
      buckets = kS3FIFOMaxBuckets;
    }
    numBuckets = buckets;
    auto newBucketSize = (expectedMaxPos + buckets - 1) / buckets;
    bucketSize = newBucketSize > 1 ? newBucketSize : 1;
    trackMiddleRemoval = trackRemoval;
  }

  void setWarmedUp() { haveWarmedUp = true; }

  int64_t recordInsert(int64_t insertCounter) {
    if (!haveWarmedUp || bucketSize == 0) {
      return 0;
    }
    int64_t newBucket = (insertCounter / bucketSize) % numBuckets;
    if (trackMiddleRemoval && newBucket != currentBucket) {
      int64_t b = (currentBucket + 1) % numBuckets;
      while (b != newBucket) {
        removalCounters[b] = 0;
        b = (b + 1) % numBuckets;
      }
      removalCounters[newBucket] = 0;
    }
    currentBucket = newBucket;
    return newBucket;
  }

  // Record that a ghost entry was hit and removed from the middle of the queue.
  void recordRemoval() {
    if (!haveWarmedUp || !trackMiddleRemoval || bucketSize == 0) {
      return;
    }
    removalCounters[currentBucket]++;
    totalRemovals++;
  }

  int64_t estimateHoles(int64_t insertBucket) const {
    if (!haveWarmedUp || !trackMiddleRemoval) {
      return 0;
    }
    int64_t holes = 0;
    int64_t bucket = insertBucket;
    while (bucket != currentBucket) {
      holes += removalCounters[bucket];
      bucket = (bucket + 1) % numBuckets;
    }
    holes += removalCounters[currentBucket];
    return holes;
  }

  void recordHit(int64_t insertTime, int64_t currentCounter) {
    if (!haveWarmedUp || bucketSize == 0) {
      return;
    }
    int64_t rawPosition = currentCounter - insertTime;
    // Only the ghost tracker corrects for mid-queue removals. For Small
    // and Main, holes == 0 and the insertBucket argument is ignored.
    int64_t holes = 0;
    if (trackMiddleRemoval) {
      int64_t insertBucket = (insertTime / bucketSize) % numBuckets;
      holes = estimateHoles(insertBucket);
    }
    int64_t adjusted = rawPosition - holes;
    if (adjusted < 0) {
      adjusted = 0;
    }
    int64_t posBucket = adjusted / bucketSize;
    if (posBucket >= numBuckets) {
      posBucket = numBuckets - 1;
    }
    hitCounts[posBucket]++;
    totalHits++;
  }

  void getHistogram(double* out) const {
    if (totalHits == 0) {
      std::memset(out, 0, numBuckets * sizeof(double));
      return;
    }
    double invTotal = 1.0 / static_cast<double>(totalHits);
    for (int i = 0; i < numBuckets; i++) {
      out[i] = static_cast<double>(hitCounts[i]) * invTotal;
    }
  }

  void reset() {
    std::memset(hitCounts, 0, sizeof(hitCounts));
    totalHits = 0;
    std::memset(removalCounters, 0, sizeof(removalCounters));
    totalRemovals = 0;
  }
};

// Aggregates per-queue counters and trackers. Snapshot into an
// S3FIFOFeatureVector via getFeatures().
struct S3FIFOFeatureCollector {
  int64_t cacheCapacity{0};
  int32_t numBuckets{kS3FIFODefaultBuckets};

  S3FIFOHitPosTracker smallTracker;
  S3FIFOHitPosTracker mainTracker;
  S3FIFOHitPosTracker ghostTracker;

  int64_t totalHitsSmall{0};
  int64_t totalHitsMain{0};
  int64_t totalHitsGhost{0};
  int64_t totalRequests{0};
  int64_t totalUnique{0};
  int64_t totalMisses{0};
  int64_t oneHitCount{0};

  int64_t sizeSmall{0};
  int64_t sizeMain{0};

  void init(int64_t capacity,
            int64_t smallSize,
            int64_t mainSize,
            int64_t ghostSize,
            int32_t buckets) {
    cacheCapacity = capacity;
    numBuckets = buckets > 0 ? buckets : kS3FIFODefaultBuckets;
    if (numBuckets > kS3FIFOMaxBuckets) {
      numBuckets = kS3FIFOMaxBuckets;
    }
    sizeSmall = smallSize;
    sizeMain = mainSize;

    smallTracker.init(smallSize, numBuckets, false);
    mainTracker.init(mainSize, numBuckets, false);
    ghostTracker.init(ghostSize, numBuckets, true);

    totalHitsSmall = 0;
    totalHitsMain = 0;
    totalHitsGhost = 0;
    totalRequests = 0;
    totalUnique = 0;
    totalMisses = 0;
    oneHitCount = 0;
  }

  void setWarmedUp() {
    smallTracker.setWarmedUp();
    mainTracker.setWarmedUp();
    ghostTracker.setWarmedUp();
  }

  // Re-seed stored sizes and tracker bucketSize after a meaningful queue
  // size shift (e.g., predictor changed smallSizePercent, or slabs moved).
  // Tracker hit counts are cleared as a side-effect of init().
  void refreshSizes(int64_t newCapacity,
                    int64_t newSmall,
                    int64_t newMain,
                    int64_t newGhost) {
    cacheCapacity = newCapacity;
    sizeSmall = newSmall;
    sizeMain = newMain;
    smallTracker.init(newSmall, numBuckets, false);
    mainTracker.init(newMain, numBuckets, false);
    ghostTracker.init(newGhost, numBuckets, true);
    setWarmedUp();
  }

  void reset() {
    totalHitsSmall = 0;
    totalHitsMain = 0;
    totalHitsGhost = 0;
    totalRequests = 0;
    totalUnique = 0;
    totalMisses = 0;
    oneHitCount = 0;
    smallTracker.reset();
    mainTracker.reset();
    ghostTracker.reset();
  }

  void getFeatures(S3FIFOFeatureVector& fv) const {
    fv.numBuckets = numBuckets;
    fv.logCacheCapacity = cacheCapacity > 0
                              ? std::log10(static_cast<double>(cacheCapacity))
                              : 0.0;

    fv.totalRequests = totalRequests;
    fv.hitsSmall = totalHitsSmall;
    fv.hitsMain = totalHitsMain;
    fv.hitsGhost = totalHitsGhost;
    fv.totalHits = totalHitsSmall + totalHitsMain + totalHitsGhost;
    fv.totalMisses = totalMisses;

    if (fv.totalHits > 0) {
      const double inv = 1.0 / static_cast<double>(fv.totalHits);
      fv.hitRatioSmall = static_cast<double>(totalHitsSmall) * inv;
      fv.hitRatioMain = static_cast<double>(totalHitsMain) * inv;
      fv.hitRatioGhost = static_cast<double>(totalHitsGhost) * inv;
    } else {
      fv.hitRatioSmall = 0.0;
      fv.hitRatioMain = 0.0;
      fv.hitRatioGhost = 0.0;
    }

    fv.uniqueRatio = totalRequests > 0
                         ? static_cast<double>(totalUnique) / totalRequests
                         : 0.0;
    fv.oneHitRatio =
        totalUnique > 0 ? static_cast<double>(oneHitCount) / totalUnique : 0.0;

    smallTracker.getHistogram(fv.histSmall);
    mainTracker.getHistogram(fv.histMain);
    ghostTracker.getHistogram(fv.histGhost);
  }

  std::string histStringify(const double* hist) const {
    std::string result = "[";
    for (int i = 0; i < numBuckets; i++) {
      if (i > 0) {
        result += ", ";
      }
      result += folly::to<std::string>(hist[i]);
    }
    result += "]";
    return result;
  }

  std::string toString() const {
    S3FIFOFeatureVector fv;
    getFeatures(fv);
    return folly::sformat(
        "S3FIFO Feature Vector:\n"
        "  sizeSmall: {}\n"
        "  sizeMain: {}\n"
        "  numBuckets: {}\n"
        "  logCacheCapacity: {:.4f}\n"
        "  totalRequests: {}\n"
        "  totalHits: {}\n"
        "  totalMisses: {}\n"
        "  hitRatioSmall: {:.4f}\n"
        "  hitRatioMain: {:.4f}\n"
        "  hitRatioGhost: {:.4f}\n"
        "  uniqueRatio: {:.4f}\n"
        "  oneHitRatio: {:.4f}\n"
        "  histSmall: {}\n"
        "  histMain: {}\n"
        "  histGhost: {}\n",
        sizeSmall, sizeMain, fv.numBuckets, fv.logCacheCapacity,
        fv.totalRequests, fv.totalHits, fv.totalMisses, fv.hitRatioSmall,
        fv.hitRatioMain, fv.hitRatioGhost, fv.uniqueRatio, fv.oneHitRatio,
        histStringify(fv.histSmall), histStringify(fv.histMain),
        histStringify(fv.histGhost));
  }
};

// Predicted parameters returned by the prediction callback.
struct S3FIFOPredictedParams {
  size_t smallSizePercent{10};
  size_t ghostSizePercent{100};
  int smallToMainPromoThreshold{2};
  double smallSkipRatio{0.0};
};

using S3FIFOPredictionCallback =
    std::function<S3FIFOPredictedParams(const S3FIFOFeatureVector&)>;

// Include the LightGBM predictor after the types it depends on.
#include "cachelib/allocator/S3FIFOLightGBMPredictor.h"

// ============================================================================
// MMS3FIFO Container
// ============================================================================

// S3-FIFO eviction policy with two FIFO queues: Small and Main.
//
// New items enter the Small queue. On access (recordAccess), items that are
// accessed are marked.
//
// Eviction follows the S3-FIFO paper: when Small exceeds `smallSizePercent`
// of total size, the Small tail is processed — accessed items are promoted
// to Main, unaccessed items become eviction victims. When Small is within
// target, eviction comes from Main's tail, after items in main's tail are
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

    Config(bool updateOnR,
           size_t smallSizePct,
           size_t ghostSizePct,
           uint8_t promoThreshold)
        : Config(/* time */ 60,
                 /* ratio */ 0.0,
                 /* updateOnW */ false,
                 updateOnR,
                 smallSizePct,
                 ghostSizePct,
                 /* mmReconfigureInterval */ 0,
                 /* useCombinedLockForIterators */ false) {
      smallToMainPromoThreshold = promoThreshold;
      checkConfig();
    }

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
      if (smallSizePercent < 1 || smallSizePercent > 100) {
        throw std::invalid_argument(
            folly::sformat("Invalid small queue size {}. Small queue size "
                           "must be between 1% and 100% of total cache size.",
                           smallSizePercent));
      }
      if (smallToMainPromoThreshold < 1 || smallToMainPromoThreshold > 2) {
        throw std::invalid_argument(folly::sformat(
            "Invalid smallToMainPromoThreshold {}. Must be 1 or 2.",
            smallToMainPromoThreshold));
      }
      if (smallSkipRatio != 0.0 && smallSkipRatio != 0.25) {
        throw std::invalid_argument(folly::sformat(
            "Invalid smallSkipRatio {}. Must be 0 or 0.25.", smallSkipRatio));
      }
      if (featureSizeDriftThreshold < 0.0 ||
          featureSizeDriftThreshold > 1.0) {
        throw std::invalid_argument(folly::sformat(
            "Invalid featureSizeDriftThreshold {}. Must be in [0, 1].",
            featureSizeDriftThreshold));
      }
      if (featureWarmupCheckSecs == 0) {
        throw std::invalid_argument(
            "featureWarmupCheckSecs must be > 0.");
      }
      if (featureCollectionSecs == 0) {
        throw std::invalid_argument(
            "featureCollectionSecs must be > 0.");
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

    // Small-to-Main promotion threshold. Only nodes whose access frequency
    // counter is >= this value get promoted from Small to Main. Valid
    // values: 1 or 2.
    uint8_t smallToMainPromoThreshold{2};

    // S4-FIFO "burst queue": skip incrementFreq for Small items whose
    // logical age (counter - insertTime) is below
    // smallSkipRatio * smallQueueSize. This prevents bursty one-hit-wonders
    // from being promoted into Main. Valid values: 0 (disabled) or 0.25.
    double smallSkipRatio{0.0};

    // ========== Feature collection ==========
    // Master on/off for feature logging. When off, all collection paths are
    // no-ops and no memory or locking cost is paid.
    bool enableFeatureCollection{true};

    // Time-based warmup. Anchored at Container construction. Once this
    // many seconds have elapsed, feature collection begins. 
    // Default 12 hours warmup
    uint64_t featureWarmupSecs{43200};

    // Rate-limit for the warmup-elapsed check on the hot path. The check
    // is performed at most once per this many seconds while !isWarmedUp_.
    // Do check every 10 minutes
    uint64_t featureWarmupCheckSecs{600};

    // Length of the feature-collection window once warmup completes.
    // After this elapses, exactly one prediction is emitted (subject to
    // the drift gate) and collection stops permanently.
    // Default 3 hours feature collection
    uint64_t featureCollectionSecs{10800};

    // Resolution of hit-position histograms.
    int32_t featureNumBuckets{kS3FIFODefaultBuckets};

    // Drift gate at apply time. Compare current Small/Main sizes to the
    // sizes captured at warmup-end (when collection started). If either
    // queue's size has drifted by more than this fraction, skip the
    // prediction entirely — the histogram bucketing is no longer
    // representative of the cache state. Collection still stops.
    double featureSizeDriftThreshold{0.30};

    // Below this cache size the trackers cannot produce a reliable signal,
    // so we stop collecting features and applying predictions.
    size_t featureMinCacheSize{25000};

    // If false, predicted smallSkipRatio is ignored when applying a
    // prediction. The runtime skip-queue path is still controlled by
    // config_.smallSkipRatio, but the model cannot move it.
    bool applyPredictedSmallSkipRatio{false};

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
          config_(std::move(c)) {
      initFeatureCollection();
    }
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

    // ========== Feature collection public API ==========

    bool isWarmedUp() const noexcept {
      return isWarmedUp_.load(std::memory_order_acquire);
    }

    S3FIFOFeatureVector getFeatures() const noexcept;

    // Swap out the prediction callback. Default is
    // facebook::cachelib::s3fifoLightGBMPredict.
    void setPredictionCallback(S3FIFOPredictionCallback callback) {
      predictionCallback_ = std::move(callback);
    }

    // Force a snapshot + prediction + apply, ignoring the wall-clock
    // interval gate. Intended for tests.
    void forceFeatureUpdate() noexcept;

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

    // Insert a key hash into ghost. The stamped Time is the gCounter value
    // at insert, used by the ghost hit-position tracker to compute age.
    //
    // EvictingCacheMap gives FIFO eviction order through its internal list since we never promote
    //
    // For duplicate ghost insertion, erase the old entry first and then insert the
    // new timestamp as a fresh ghost entry. This avoids keeping an old FIFO
    // position with a newer timestamp.
    void ghostInsert(uint32_t keyHash, Time stampedTS) {
      const size_t ghostMax =
          std::max<size_t>(1, lru_.size() * config_.ghostSizePercent / 100);

      ghostMap_.setMaxSize(ghostMax);

      // Insert if new, replace the value and bump the LRU up otherwise.
      ghostMap_.set(keyHash, stampedTS, true);
    }

    // We do not use lookup promotion for ghost hits. On ghost hit, the entry is
    // erased completely from both the index and the internal list.
    std::pair<bool, Time> ghostContainsAndErase(uint32_t keyHash) {
      auto it = ghostMap_.findWithoutPromotion(keyHash);

      if (it == ghostMap_.end()) {
        return {false, Time{0}};
      }

      const Time ts = it->second;

      ghostMap_.erase(keyHash);

      return {true, ts};
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

    // Flag helpers: kMMFlag1 (low bit) + kMMFlag2 (high bit) encode a
    // 2-bit access-frequency counter in [0,3]. Every access increments
    // (saturating at 3); every Main-tail reinsertion decrements (floored
    // at 0). Small-to-Main promotion requires freq >= promoThreshold.
    static constexpr uint8_t kMaxFreq = 3;

    static uint8_t getFreq(const T& node) noexcept {
      const uint8_t lo = node.template isFlagSet<RefFlags::kMMFlag1>() ? 1 : 0;
      const uint8_t hi = node.template isFlagSet<RefFlags::kMMFlag2>() ? 1 : 0;
      return static_cast<uint8_t>((hi << 1) | lo);
    }

    static void setFreq(T& node, uint8_t freq) noexcept {
      if (freq & 0x1) {
        node.template setFlag<RefFlags::kMMFlag1>();
      } else {
        node.template unSetFlag<RefFlags::kMMFlag1>();
      }
      if (freq & 0x2) {
        node.template setFlag<RefFlags::kMMFlag2>();
      } else {
        node.template unSetFlag<RefFlags::kMMFlag2>();
      }
    }

    static void incrementFreq(T& node) noexcept {
      const auto f = getFreq(node);
      if (f < kMaxFreq) {
        setFreq(node, static_cast<uint8_t>(f + 1));
      }
    }

    static void decrementFreq(T& node) noexcept {
      const auto f = getFreq(node);
      if (f > 0) {
        setFreq(node, static_cast<uint8_t>(f - 1));
      }
    }

    static void resetFreq(T& node) noexcept { setFreq(node, 0); }

    // Process-wide monotonic ID assigned to each Container instance for
    // log correlation. 
    static uint64_t nextCollectorId() {
      static std::atomic<uint64_t> counter{0};
      return counter.fetch_add(1, std::memory_order_relaxed);
    }

    void initFeatureCollection() {
      collectorId_ = nextCollectorId();
      if (config_.enableFeatureCollection) {
        featureCollector_.init(0, 0, 0, 0, config_.featureNumBuckets);
      }
    }

    // Called at top of recordAccess. Wall-clock gated; noop until warmup.
    void maybeUpdateFeatures() noexcept;

    // Caller must hold lruMutex_.
    void applyPredictedParamsLocked(const S3FIFOPredictedParams& params) {
      config_.smallSizePercent = params.smallSizePercent;
      config_.ghostSizePercent = params.ghostSizePercent;
      config_.smallToMainPromoThreshold =
          static_cast<uint8_t>(params.smallToMainPromoThreshold);
      if (config_.applyPredictedSmallSkipRatio) {
        config_.smallSkipRatio = params.smallSkipRatio;
      }
    }

    // Caller holds featureMutex_. Prediction updates must also be serialized
    // with eviction, which reads the tunables under lruMutex_.
    void applyPredictedParams(const S3FIFOPredictedParams& params) {
      lruMutex_->lock_combine(
          [this, &params]() { applyPredictedParamsLocked(params); });
    }

    // keyHash → gCounter-at-insert. O(1) lookup for ghost hits; the TS
    // lets us compute ghost hit position.
    mutable folly::EvictingCacheMap<uint32_t, Time> ghostMap_{1};


    mutable folly::cacheline_aligned<Mutex> lruMutex_;

    mutable LruList lru_{};

    Config config_{};

    // Per-queue logical counters. Each ticks once per insert into its
    // respective queue. A node's updateTime is stamped with the counter
    // value of the queue it enters, so (currentCounter - updateTime) is its
    // age (in inserts) in that queue.
    mutable std::atomic<Time> sCounter_{0};
    mutable std::atomic<Time> mCounter_{0};
    mutable std::atomic<Time> gCounter_{0};

    // ========== Feature collection state ==========
    mutable S3FIFOFeatureCollector featureCollector_;

    // Default is the bundled LightGBM ensemble. Can be swapped out by
    // setPredictionCallback (e.g., a mock for tests).
    S3FIFOPredictionCallback predictionCallback_{
        facebook::cachelib::s3fifoLightGBMPredict};

    // Anchor for the warmup timer; lazily set on first maybeUpdateFeatures.
    std::atomic<uint64_t> creationTime_{0};
    // Rate-limits the warmup-elapsed check on the hot path while !isWarmedUp_.
    std::atomic<uint64_t> lastWarmupCheckTime_{0};
    std::atomic<bool> isWarmedUp_{false};
    // Wall-clock at warmup-end; the 2h collection window is anchored here.
    std::atomic<uint64_t> warmupTime_{0};
    std::atomic<bool> hasUpdatedOnce_{false};
    // Per-container unique ID for log correlation. Assigned at construction.
    uint64_t collectorId_{0};

    // Serializes snapshot + reset. Separate from lruMutex_ to avoid
    // contention on the hot eviction path.
    mutable folly::cacheline_aligned<Mutex> featureMutex_;

    friend class MMTypeTest<MMS3FIFO>;
  };
};

/* Container Interface Implementation */
template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
MMS3FIFO::Container<T, HookPtr>::Container(serialization::MMS3FIFOObject object,
                                           PtrCompressor compressor)
    : lru_(*object.lrus(), std::move(compressor)), config_(*object.config()) {
  initFeatureCollection();
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
void MMS3FIFO::Container<T, HookPtr>::maybeUpdateFeatures() noexcept {
  if (!config_.enableFeatureCollection) {
    return;
  }

  // Single shot; once we've applied (or aborted on drift) we never run
  // the prediction path again.
  if (hasUpdatedOnce_.load(std::memory_order_acquire)) {
    config_.enableFeatureCollection = false;
    return;
  }

  const auto now = static_cast<uint64_t>(util::getCurrentTimeSec());

  // Lazy-init the warmup anchor on first call. The first thread to win
  // the CAS also emits the per-container creation banner.
  uint64_t creation = creationTime_.load(std::memory_order_acquire);
  if (creation == 0) {
    uint64_t expected = 0;
    if (creationTime_.compare_exchange_strong(expected, now,
                                              std::memory_order_acq_rel)) {
      creation = now;
      XLOGF(INFO,
            "[S4FIFO id={}] Container started at time {} — warmup window "
            "{}s, collection window {}s, initial smallSizePercent={} "
            "ghostSizePercent={} promoThreshold={} smallSkipRatio={:.4f}",
            collectorId_, creation, config_.featureWarmupSecs,
            config_.featureCollectionSecs, config_.smallSizePercent,
            config_.ghostSizePercent,
            static_cast<int>(config_.smallToMainPromoThreshold),
            config_.smallSkipRatio);
    } else {
      creation = expected;
    }
  }

  // Phase 1: pre-warmup wait. Rate-limit the elapsed-time check so the
  // hot path pays at most one wall-clock read per featureWarmupCheckSecs.
  if (!isWarmedUp_.load(std::memory_order_acquire)) {
    const uint64_t lastCheck =
        lastWarmupCheckTime_.load(std::memory_order_acquire);
    if (now - lastCheck < config_.featureWarmupCheckSecs) {
      return;
    }
    lastWarmupCheckTime_.store(now, std::memory_order_release);
    if (now - creation < config_.featureWarmupSecs) {
      return;
    }
    // Warmup window has elapsed: seed trackers with the current sizes
    // (these become the drift baseline) and start collection.
    featureMutex_->lock_combine([this, creation]() {
      if (isWarmedUp_.load(std::memory_order_acquire)) {
        return;
      }
      const uint64_t startTime =
          static_cast<uint64_t>(util::getCurrentTimeSec());
      const size_t totalSize = lru_.size();
      const size_t smallReal = lru_.getList(LruType::Small).size();
      const size_t mainReal = lru_.getList(LruType::Main).size();
      const size_t ghostSize =
          totalSize * config_.ghostSizePercent / 100;
      featureCollector_.init(totalSize, smallReal, mainReal, ghostSize,
                             config_.featureNumBuckets);
      featureCollector_.setWarmedUp();
      warmupTime_.store(startTime, std::memory_order_release);
      isWarmedUp_.store(true, std::memory_order_release);
      XLOGF(INFO,
            "[S4FIFO id={}] Warmup complete at time {} (anchor={}, "
            "elapsed={}s): totalSize={} smallReal={} mainReal={} "
            "ghostSize={} — collecting features for {}s",
            collectorId_, startTime, creation, startTime - creation,
            totalSize, smallReal, mainReal, ghostSize,
            config_.featureCollectionSecs);
    });
    return;
  }

  // Phase 2: collection window. Wait until featureCollectionSecs has
  // elapsed since warmup-end, then emit one prediction.
  const uint64_t warmEnd = warmupTime_.load(std::memory_order_acquire);
  if (now - warmEnd < config_.featureCollectionSecs) {
    return;
  }

  featureMutex_->lock_combine([this]() {
    if (hasUpdatedOnce_.load(std::memory_order_acquire)) {
      return;
    }
    const uint64_t applyTime =
        static_cast<uint64_t>(util::getCurrentTimeSec());
    const uint64_t warmEnd = warmupTime_.load(std::memory_order_acquire);

    // Always log the apply-time queue size diff against the warmup-end
    // baseline so the user can see drift even when it's under the gate.
    const int64_t storedSmall = featureCollector_.sizeSmall;
    const int64_t storedMain = featureCollector_.sizeMain;
    const int64_t curSmall =
        static_cast<int64_t>(lru_.getList(LruType::Small).size());
    const int64_t curMain =
        static_cast<int64_t>(lru_.getList(LruType::Main).size());
    auto drift = [](int64_t stored, int64_t live) {
      if (stored <= 0) {
        return live == 0 ? 0.0 : 1.0;
      }
      const double diff = static_cast<double>(live - stored);
      return std::fabs(diff) / static_cast<double>(stored);
    };
    const double driftSmall = drift(storedSmall, curSmall);
    const double driftMain = drift(storedMain, curMain);
    const double driftThreshold = config_.featureSizeDriftThreshold;

    XLOGF(INFO,
          "[S4FIFO id={}] Apply gate at time {} (collection={}s elapsed): "
          "small stored={} live={} (delta={} drift={:.2f}%); main "
          "stored={} live={} (delta={} drift={:.2f}%); threshold={:.2f}%",
          collectorId_, applyTime, applyTime - warmEnd, storedSmall,
          curSmall, curSmall - storedSmall, driftSmall * 100, storedMain,
          curMain, curMain - storedMain, driftMain * 100,
          driftThreshold * 100);

    if (driftSmall > driftThreshold || driftMain > driftThreshold) {
      XLOGF(INFO,
            "[S4FIFO id={}] Size drift exceeds threshold — skipping "
            "prediction, stopping collection",
            collectorId_);
      hasUpdatedOnce_.store(true, std::memory_order_release);
      config_.enableFeatureCollection = false;
      return;
    }

    S3FIFOFeatureVector features;
    featureCollector_.getFeatures(features);

    if (features.totalHits == 0) {
      XLOGF(INFO,
            "[S4FIFO id={}] Collection window elapsed with totalHits=0 — "
            "skipping prediction, stopping collection",
            collectorId_);
      hasUpdatedOnce_.store(true, std::memory_order_release);
      config_.enableFeatureCollection = false;
      return;
    }

    S3FIFOPredictedParams predicted = predictionCallback_(features);

    XLOGF(INFO, "[S4FIFO id={}] Feature snapshot at time {}:\n{}",
          collectorId_, applyTime, featureCollector_.toString());
    XLOGF(INFO,
          "[S4FIFO id={}] Raw predicted params: smallSizePercent={} "
          "ghostSizePercent={} smallToMainPromoThreshold={} "
          "smallSkipRatio={:.4f}",
          collectorId_, predicted.smallSizePercent,
          predicted.ghostSizePercent, predicted.smallToMainPromoThreshold,
          predicted.smallSkipRatio);
    // If # of objects too few, skip
    if (lru_.size() < config_.featureMinCacheSize) {
      XLOGF(INFO,
            "[S4FIFO id={}] Cache size {} is below the featureMinCacheSize "
            "{} — skipping prediction, stopping collection",
            collectorId_, lru_.size(), config_.featureMinCacheSize);
      hasUpdatedOnce_.store(true, std::memory_order_release);
      config_.enableFeatureCollection = false;
      return;
    }

    // Snapshot config before and after under lruMutex_, the same lock used
    // by eviction when reading these tunables.
    size_t beforeSmallPct = 0;
    size_t beforeGhostPct = 0;
    uint8_t beforePromo = 0;
    double beforeSkip = 0.0;
    size_t afterSmallPct = 0;
    size_t afterGhostPct = 0;
    uint8_t afterPromo = 0;
    double afterSkip = 0.0;

    lruMutex_->lock_combine([this, &predicted, &beforeSmallPct,
                             &beforeGhostPct, &beforePromo, &beforeSkip,
                             &afterSmallPct, &afterGhostPct, &afterPromo,
                             &afterSkip]() {
      beforeSmallPct = config_.smallSizePercent;
      beforeGhostPct = config_.ghostSizePercent;
      beforePromo = config_.smallToMainPromoThreshold;
      beforeSkip = config_.smallSkipRatio;

      applyPredictedParamsLocked(predicted);

      afterSmallPct = config_.smallSizePercent;
      afterGhostPct = config_.ghostSizePercent;
      afterPromo = config_.smallToMainPromoThreshold;
      afterSkip = config_.smallSkipRatio;
    });

    XLOGF(INFO,
          "[S4FIFO id={}] Applied params: smallSizePercent {}->{}, "
          "ghostSizePercent {}->{}, smallToMainPromoThreshold {}->{}, "
          "smallSkipRatio {:.4f}->{:.4f}",
          collectorId_, beforeSmallPct, afterSmallPct,
          beforeGhostPct, afterGhostPct,
          static_cast<int>(beforePromo),
          static_cast<int>(afterPromo), beforeSkip, afterSkip);

    hasUpdatedOnce_.store(true, std::memory_order_release);
    config_.enableFeatureCollection = false;
  });
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
void MMS3FIFO::Container<T, HookPtr>::forceFeatureUpdate() noexcept {
  if (!config_.enableFeatureCollection) {
    return;
  }
  featureMutex_->lock_combine([this]() {
    S3FIFOFeatureVector features;
    featureCollector_.getFeatures(features);
    const S3FIFOPredictedParams predicted = predictionCallback_(features);
    XLOGF(INFO,
          "[S4FIFO id={}] forceFeatureUpdate: predicted "
          "smallSizePercent={} ghostSizePercent={} "
          "smallToMainPromoThreshold={} smallSkipRatio={:.4f}",
          collectorId_, predicted.smallSizePercent,
          predicted.ghostSizePercent, predicted.smallToMainPromoThreshold,
          predicted.smallSkipRatio);
    applyPredictedParams(predicted);
    hasUpdatedOnce_.store(true, std::memory_order_release);
    config_.enableFeatureCollection = false;
  });
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
S3FIFOFeatureVector MMS3FIFO::Container<T, HookPtr>::getFeatures()
    const noexcept {
  S3FIFOFeatureVector features;
  featureMutex_->lock_combine(
      [this, &features]() { featureCollector_.getFeatures(features); });
  return features;
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
bool MMS3FIFO::Container<T, HookPtr>::recordAccess(T& node,
                                                   AccessMode mode) noexcept {
  if ((mode == AccessMode::kWrite && !config_.updateOnWrite) ||
      (mode == AccessMode::kRead && !config_.updateOnRead)) {
    return false;
  }

  // Wall-clock-gated feature snapshot. No-op until warmup.
  maybeUpdateFeatures();

  if (!node.isInMMContainer()) {
    return false;
  }

  const bool warmedUp = isWarmedUp_.load(std::memory_order_acquire);
  const bool collect = config_.enableFeatureCollection && warmedUp;

  if (isSmall(node)) {
    const Time curr = sCounter_.load(std::memory_order_relaxed);
    // S4-FIFO burst queue: skip incrementFreq if this item is still in
    // the newest smallSkipRatio fraction of the Small queue. "Age" here
    // is the number of Small inserts since this node was inserted, which
    // is a proxy for distance from the Small head.
    if (config_.smallSkipRatio > 0.0) {
      const Time insertTime = getUpdateTime(node);
      const int64_t age =
          static_cast<int64_t>(curr) - static_cast<int64_t>(insertTime);
      const int64_t skipThreshold = static_cast<int64_t>(
          config_.smallSkipRatio * lru_.getList(LruType::Small).size());
      if (age >= skipThreshold) {
        incrementFreq(node);
      }
    } else {
      incrementFreq(node);
    }

    if (collect) {
      featureCollector_.totalHitsSmall++;
      featureCollector_.smallTracker.recordHit(
          static_cast<int64_t>(getUpdateTime(node)),
          static_cast<int64_t>(curr));
      featureCollector_.totalRequests++;
    }
    return true;
  }

  // Main queue. updateTime is stamped in mCounter space.
  const Time mCurr = mCounter_.load(std::memory_order_relaxed);

  // Skip delay for now
  // // Throttle: skip if mCounter hasn't advanced enough since last access.
  // if (getFreq(node) > 0 &&
  //     mCurr < getUpdateTime(node) + config_.lruRefreshTime) {
  //   return false;
  // }

  incrementFreq(node);

  if (collect) {
    featureCollector_.totalHitsMain++;
    featureCollector_.mainTracker.recordHit(
        static_cast<int64_t>(getUpdateTime(node)),
        static_cast<int64_t>(mCurr));
    featureCollector_.totalRequests++;
  }
  return true;
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
bool MMS3FIFO::Container<T, HookPtr>::add(T& node) noexcept {
  const auto keyHash = getKeyHash(node);

  bool inserted = false;
  bool insertedMain = false;
  bool ghostHit = false;
  Time ghostTS{0};
  Time stampedTime{0};

  lruMutex_->lock_combine([&]() {
    if (node.isInMMContainer()) {
      return;
    }

    auto ghostResult = ghostContainsAndErase(keyHash);
    ghostHit = ghostResult.first;
    ghostTS = ghostResult.second;

    if (ghostHit) {
      // Ghost hit: insert directly to Main (skip Small).
      stampedTime = mCounter_.fetch_add(1, std::memory_order_relaxed) + 1;
      lru_.getList(LruType::Main).linkAtHead(node);
      insertedMain = true;
    } else {
      stampedTime = sCounter_.fetch_add(1, std::memory_order_relaxed) + 1;
      lru_.getList(LruType::Small).linkAtHead(node);
      markSmall(node);
    }
    node.markInMMContainer();
    setUpdateTime(node, stampedTime);
    resetFreq(node);
    inserted = true;
  });

  if (!inserted) {
    return false;
  }

  // Feature accounting. Guarded by enableFeatureCollection + warmup.
  if (config_.enableFeatureCollection &&
      isWarmedUp_.load(std::memory_order_acquire)) {
    if (ghostHit) {
      featureCollector_.totalHitsGhost++;
      featureCollector_.ghostTracker.recordHit(
          static_cast<int64_t>(ghostTS),
          static_cast<int64_t>(gCounter_.load(std::memory_order_relaxed)));
      featureCollector_.ghostTracker.recordRemoval();
    } else {
      featureCollector_.totalUnique++;
    }
    featureCollector_.totalMisses++;
    featureCollector_.totalRequests++;

    if (insertedMain) {
      featureCollector_.mainTracker.recordInsert(
          static_cast<int64_t>(stampedTime));
    } else {
      featureCollector_.smallTracker.recordInsert(
          static_cast<int64_t>(stampedTime));
    }
  }

  return true;
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
  const uint8_t promoThreshold = config_.smallToMainPromoThreshold;
  const bool collect = config_.enableFeatureCollection &&
                       isWarmedUp_.load(std::memory_order_acquire);

  // Only process Small when it exceeds target size.
  // At stable state, this op is constant time.
  while (smallList.size() > targetSmallSize) {
    auto* tail = smallList.getTail();
    if (!tail) {
      break;
    }
    if (getFreq(*tail) >= promoThreshold) {
      // Meets promotion threshold: move to Main head, reset counter, and
      // re-stamp updateTime with mCounter so mainTracker positions are
      // correct.
      smallList.remove(*tail);
      lru_.getList(LruType::Main).linkAtHead(*tail);
      unmarkSmall(*tail);
      resetFreq(*tail);
      const Time stamp =
          mCounter_.fetch_add(1, std::memory_order_relaxed) + 1;
      setUpdateTime(*tail, stamp);
      if (collect) {
        featureCollector_.mainTracker.recordInsert(
            static_cast<int64_t>(stamp));
      }
    } else {
      break; // Below threshold: leave as eviction victim
    }
  }
}

template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
void MMS3FIFO::Container<T, HookPtr>::lazyReinsertMainTailLocked()
    const noexcept {
  auto& mainList = lru_.getList(LruType::Main);
  auto* tail = mainList.getTail();
  if (tail == nullptr || getFreq(*tail) == 0) {
    return;
  }
  const bool collect = config_.enableFeatureCollection &&
                       isWarmedUp_.load(std::memory_order_acquire);

  auto* cur = tail;
  auto* first = tail;

  // Find the contiguous freq>0 suffix at Main tail; decrement each as we
  // reinsert. Items whose counter reaches 0 only on the next pass survive
  // one more cycle before eviction.
  while (cur && getFreq(*cur) > 0) {
    decrementFreq(*cur);
    const Time stamp = mCounter_.fetch_add(1, std::memory_order_relaxed) + 1;
    setUpdateTime(*cur, stamp);
    if (collect) {
      featureCollector_.mainTracker.recordInsert(static_cast<int64_t>(stamp));
    }
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
  if (lru_.getList(LruType::Small).size() > targetSmallSize ||
      lru_.getList(LruType::Main).size() == 0) {
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
    if (lru_.getList(LruType::Small).size() > targetSmallSize ||
        lru_.getList(LruType::Main).size() == 0) {
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
  resetFreq(node);
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
  const bool evictedFromSmall = isSmall(node);
  const uint8_t evictedFreq = getFreq(node);
  const auto keyHash = getKeyHash(node);
  ++it;
  removeLocked(node);

  if (evictedFromSmall) {
    const Time gStamp = gCounter_.fetch_add(1, std::memory_order_relaxed) + 1;
    ghostInsert(keyHash, gStamp);

    if (config_.enableFeatureCollection &&
        isWarmedUp_.load(std::memory_order_acquire)) {
      if (evictedFreq < config_.smallToMainPromoThreshold) {
        featureCollector_.oneHitCount++;
      }
      featureCollector_.ghostTracker.recordInsert(
          static_cast<int64_t>(gStamp));
    }
  }
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
    setFreq(newNode, getFreq(oldNode));
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

// Eviction age is logical now, this function is deprecated.
template <typename T, MMS3FIFO::Hook<T> T::* HookPtr>
cachelib::EvictionAgeStat
MMS3FIFO::Container<T, HookPtr>::getEvictionAgeStatLocked(
    uint64_t projectedLength) const noexcept {
  EvictionAgeStat stat{};
  const Time sCurr = sCounter_.load(std::memory_order_relaxed);
  const Time mCurr = mCounter_.load(std::memory_order_relaxed);

  // Determine which queue would be evicted from, matching
  // getEvictionIterator logic. Report that queue in warmQueueStat
  // since getOldestElementAge() reads warmQueueStat.
  const auto totalSize = lru_.size();
  const auto targetSmallSize =
      totalSize == 0 ? 0 : totalSize * config_.smallSizePercent / 100;
  const auto& smallList = lru_.getList(LruType::Small);
  const bool evictFromSmall = smallList.size() > targetSmallSize;

  // Node updateTime is in the counter space of the queue it lives in.
  auto ageOf = [&](const T& node) -> Time {
    const Time now = isSmall(node) ? sCurr : mCurr;
    return now - getUpdateTime(node);
  };

  {
    auto& list = evictFromSmall ? lru_.getList(LruType::Small)
                                : lru_.getList(LruType::Main);
    auto it = list.rbegin();
    stat.warmQueueStat.oldestElementAge =
        it != list.rend() ? ageOf(*it) : 0;
    stat.warmQueueStat.size = list.size();
    for (size_t numSeen = 0; numSeen < projectedLength && it != list.rend();
         ++numSeen, ++it) {
    }
    stat.warmQueueStat.projectedAge = it != list.rend()
                                          ? ageOf(*it)
                                          : stat.warmQueueStat.oldestElementAge;
  }

  {
    auto& list = evictFromSmall ? lru_.getList(LruType::Main)
                                : lru_.getList(LruType::Small);
    auto it = list.rbegin();
    stat.coldQueueStat.oldestElementAge =
        it != list.rend() ? ageOf(*it) : 0;
    stat.coldQueueStat.size = list.size();
    for (size_t numSeen = 0; numSeen < projectedLength && it != list.rend();
         ++numSeen, ++it) {
    }
    stat.coldQueueStat.projectedAge = it != list.rend()
                                          ? ageOf(*it)
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
