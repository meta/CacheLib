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

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/RebalanceInfo.h"
#include "cachelib/allocator/memory/Slab.h"

namespace facebook {
namespace cachelib {

struct RebalanceContext {
  // Victim and Receiver must belong to the same pool
  ClassId victimClassId{Slab::kInvalidClassId};
  ClassId receiverClassId{Slab::kInvalidClassId};

  // to support multiple <victim, receiver> pairs at a time
  std::vector<std::pair<ClassId, ClassId>> victimReceiverPairs{};
  // tracking the diff between victim and receiver
  double diffValue{0.0};

  RebalanceContext() = default;
  RebalanceContext(ClassId victim, ClassId receiver)
      : victimClassId(victim), receiverClassId(receiver) {}
  
  explicit RebalanceContext(const std::vector<std::pair<ClassId, ClassId>>& pairs)
      : victimReceiverPairs(pairs) {}
  
  bool isEffective() const {
    auto isPairEffective = [](ClassId victim, ClassId receiver) {
      return victim != Slab::kInvalidClassId &&
            receiver != Slab::kInvalidClassId &&
            victim != receiver;
    };

    bool singleEffective = isPairEffective(victimClassId, receiverClassId);

    bool pairEffective = std::any_of(
        victimReceiverPairs.begin(), victimReceiverPairs.end(),
        [&](const std::pair<ClassId, ClassId>& p) {
          return isPairEffective(p.first, p.second);
        });

    return singleEffective || pairEffective;
  }
};

class PickVictimStrategy {
 public:
  virtual ~PickVictimStrategy() = default;

  virtual ClassId pickAllocClassForResizing(
      const CacheBase& cache, PoolId pid, const PoolStats& poolStats) const = 0;
};

class TotalSlabsStrategy : public PickVictimStrategy {
 public:
  ClassId pickAllocClassForResizing(const CacheBase& cache,
                                    PoolId pid,
                                    const PoolStats& poolStats) const override;
};

class AllocationSizeStrategy : public PickVictimStrategy {
 public:
  struct Config {
    unsigned int minSlabsForResizing = 10;
    unsigned int numClassesToPickFrom = 10;
  };

  AllocationSizeStrategy() : config_() {}

  explicit AllocationSizeStrategy(Config config) : config_(std::move(config)) {}

  ClassId pickAllocClassForResizing(const CacheBase& cache,
                                    PoolId pid,
                                    const PoolStats& poolStats) const override;

 private:
  Config config_;
};

// Base class for rebalance strategy.
// Given a pool, the rebalance strategy picks a victim allocation class to
// release slabs, or picks a pair of victim and receiver. The idea here is the
// user should inspect the stats corresponding to the pool they want to
// rebalance. They will then decide if and which allocation classes need to
// release slab and how many slabs should be released back to the pool, or
// moved.
//
// The actual release operation is handled by PoolRebalancer
class RebalanceStrategy {
 public:
  enum Type {
    PickNothingOrTest = 0,
    Random,
    MarginalHits,
    MarginalHitsNew,
    FreeMem,
    HitsPerSlab,
    LruTailAge,
    PoolResize,
    StressRebalance,
    Manual,
    NumTypes
  };
  struct BaseConfig {};
  RebalanceStrategy()
      : pickVictimStrategy_(std::make_unique<TotalSlabsStrategy>()) {}

  virtual ~RebalanceStrategy() = default;

  virtual std::map<std::string, std::string> exportConfig() const {
    return {{"rebalancer_type", fmt::format("{}", getTypeString())}};
  }

  // Pick an victim and receiver from the same pool
  //
  // @param allocator   Cache allocator that implements CacheBase @param pid
  // Pool to rebalance
  //
  // @return RebalanceContext   contains victim and receiver
  RebalanceContext pickVictimAndReceiver(const CacheBase& cache, PoolId pid);

  // Pick only the victim irrespective of who is receiving the slab. This is
  // used mostly for pool resizing.
  ClassId pickVictimForResizing(const CacheBase& cache, PoolId pid);

  virtual void updateConfig(const BaseConfig&) {}


  void recordRebalanceEvent(PoolId pid, RebalanceContext ctx, size_t maxQueueSize); 

  double getMinDiffValueFromRebalanceEvents(PoolId pid) const;

  unsigned int getRebalanceEventQueueSize(PoolId pid) const;

  void clearPoolRebalanceEvent(PoolId pid);

  double queryEffectiveMoveRate(PoolId pid) const;
  void setPickVictimStrategy(std::unique_ptr<PickVictimStrategy> strategy) {
    pickVictimStrategy_ = std::move(strategy);
  }

  Type getType() const { return type_; }

  std::string getTypeString() const {
    switch (type_) {
    case PickNothingOrTest:
      return "PickNothingOrTest";
    case Random:
      return "Random";
    case MarginalHits:
      return "MarginalHits";
    case MarginalHitsNew:
      return "MarginalHitsNew";
    case FreeMem:
      return "FreeMem";
    case HitsPerSlab:
      return "HitsPerSlab";
    case LruTailAge:
      return "LruTailAge";
    case PoolResize:
      return "PoolResize";
    case StressRebalance:
      return "StressRebalance";
    default:
      return "Invalid rebalance strategy";
    }
  }

 protected:
  using PoolState = std::array<detail::Info, MemoryAllocator::kMaxClasses>;
  static const RebalanceContext kNoOpContext;

  explicit RebalanceStrategy(Type strategyType)
      : type_(strategyType),
        pickVictimStrategy_(std::make_unique<TotalSlabsStrategy>()) {}

  virtual RebalanceContext pickVictimAndReceiverImpl(const CacheBase&,
                                                     PoolId,
                                                     const PoolStats&) {
    return {};
  }

  virtual ClassId pickVictimImpl(const CacheBase&, PoolId, const PoolStats&) {
    return Slab::kInvalidClassId;
  }

  // Returns true if the state was already initialized and set up. False if we
  // state was not present.
  bool poolStatePresent(PoolId pid) const { return poolState_.contains(pid); }

  PoolState& getPoolState(PoolId pid) { return poolState_.at(pid); }
  const PoolState& getPoolState(PoolId pid) const { return poolState_.at(pid); }

  // filter the candidates based on whether they have enough slabs to be a
  // victim. This is based on config_.minSlabs
  static std::set<ClassId> filterByNumEvictableSlabs(const PoolStats& stats,
                                                     std::set<ClassId> victims,
                                                     unsigned int numSlabs);

  // Filter the candidates based on whether they have allocation failures.
  static std::set<ClassId> filterByAllocFailure(const PoolStats& stats,
                                                std::set<ClassId> candidates,
                                                const PoolState& prevState);

  // filter out the candidates that are not evicting and have free memory
  // based on the previous state. The prev state is used to figure out if
  // evictions have happened recently
  static std::set<ClassId> filterByNoEvictions(const PoolStats& stats,
                                               std::set<ClassId> candidates,
                                               const PoolState& prevState);

  // helper function to filter an std::set based on a predicate. If the
  // predicate returns true, the element is filtered.
  //
  // @param input   the input set of alloc class ids @param pred    the
  // predicate to filter by. If predicate returns true, the element is removed
  // @param purpose string identifying the purpose for logging.
  static std::set<ClassId> filter(std::set<ClassId> input,
                                  std::function<bool(const ClassId& id)> pred,
                                  const std::string& purpose);

  // filter the candidates based on whether they have at least this much tail
  // age
  static std::set<ClassId> filterByMinTailAge(const PoolStats& stats,
                                              std::set<ClassId> candidates,
                                              unsigned int minTailAge);

  // filter the candidates based on whether they have at most this much tail
  // age
  static std::set<ClassId> filterByMaxTailAge(const PoolStats& stats,
                                              std::set<ClassId> candidates,
                                              unsigned int maxTailAge);

  // filter the candidates based on whether they recently gained a slab and
  // are in hold off period.
  std::set<ClassId> filterVictimsByHoldOff(PoolId pid,
                                           const PoolStats& stats,
                                           std::set<ClassId> victims);

  static std::set<ClassId> filterVictimsByTargetEvictionAge(
      const PoolStats& stats,
      std::set<ClassId> candidates,
      const folly::F14FastMap<uint32_t, uint32_t>& map);

  static std::set<ClassId> filterReceiversByTargetEvictionAge(
      const PoolStats& stats,
      std::set<ClassId> candidates,
      const folly::F14FastMap<uint32_t, uint32_t>& map);

  static ClassId pickVictimByFreeMem(const std::set<ClassId>& victims,
                                     const PoolStats& poolStats,
                                     size_t threshold,
                                     const PoolState& prevState);

  std::unordered_map<PoolId, std::deque<RebalanceContext>> recentRebalanceEvents_;

 private:
  // initialize the pool's state to the current stats.
  void initPoolState(PoolId pid, const PoolStats& stats);

  // process the stats for this pool and apply them to the previous state.
  // Get deltas for evictions and hits and determine if a slab got added.
  void recordCurrentState(PoolId pid, const PoolStats& stats);

  // Pick a receiver with max alloc failures. If no alloc failures, return
  // invalid classid.
  ClassId pickReceiverWithAllocFailures(const CacheBase& cache,
                                        PoolId pid,
                                        const PoolStats& stat);

  // Ensure pool state is initialized before calling impl, and update pool
  // state after calling impl.
  //
  // @param cache       Cache allocator that implements CacheBase @param pid
  // Pool to rebalance @param impl        Reblanacing or resizing
  // implementation to call @param noOp        No operation indicator for the
  // first time encountering a pool
  template <typename T>
  T executeAndRecordCurrentState(const CacheBase& cache,
                                 PoolId pid,
                                 const std::function<T(const PoolStats&)>& impl,
                                 T noOp);

  Type type_ = PickNothingOrTest;

  // maintain the state of the previous snapshot of pool for every pool.  We
  // ll use this for processing and getting the deltas for some of these.
  std::unordered_map<PoolId, PoolState> poolState_;

  // Strategy for picking victims for resizing and memory monitor when the
  // rebalance strategy does not pick a victim
  std::unique_ptr<PickVictimStrategy> pickVictimStrategy_;

  FRIEND_TEST(RebalanceStrategy, Basic);
};

} // namespace cachelib
} // namespace facebook
