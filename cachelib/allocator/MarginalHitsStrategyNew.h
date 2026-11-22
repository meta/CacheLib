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

#include "cachelib/allocator/MarginalHitsState.h"
#include "cachelib/allocator/RebalanceStrategy.h"

namespace facebook {
namespace cachelib {

// This strategy computes number of hits in the tail slab of LRU to estimate
// the potential (given one more slab, how many more hits can this LRU serve).
// And use a smoothed ranking of those potentials to decide victim and receiver.
class MarginalHitsStrategyNew : public RebalanceStrategy {
 public:
  // Config class for marginal hits strategy
  struct Config : public BaseConfig {
    // parameter for moving average, to smooth the ranking
    double movingAverageParam{0.3};

    // minimum number of slabs to retain in every allocation class.
    unsigned int minSlabs{1};

    // maximum free memory (equivalent to this many slabs) in every allocation
    // class
    unsigned int maxFreeMemSlabs{1};

    bool onlyUpdateHitIfRebalance{true};

    // enforcing thresholds between the victim and receiver class
    double minDiff{2.0};
    double minDiffRatio{0.00};

    ////// these parameters are for controlling the threshold auto-tuning
    unsigned int thresholdIncMinWindowSize{5};
    bool thresholdAI{true};
    bool thresholdMI{false};
    bool thresholdAD{false};
    bool thresholdMD{true};

    double emrLow{0.5};
    double emrHigh{0.95};
    double thresholdAIADStep{2.0};
    double thresholdMIMDFactor{2.0};
    ///////////////////////////

    uint64_t minRequestsObserved{50000};
    uint64_t minDecayInterval{50000};

    Config() noexcept {}
    explicit Config(double param) noexcept : Config(param, 1, 1) {}
    Config(double param, unsigned int minSlab, unsigned int maxFree) noexcept
        : movingAverageParam(param),
          minSlabs(minSlab),
          maxFreeMemSlabs(maxFree) {}
  };

  // Update the config. This will not affect the current rebalancing, but
  // will take effect in the next round
  void updateConfig(const BaseConfig& baseConfig) override final {
    std::lock_guard<std::mutex> l(configLock_);
    config_ = static_cast<const Config&>(baseConfig);
  }

  bool updateMinDff(double newValue) {
    if(config_.minDiff == newValue){
      return false;
    }
    std::lock_guard<std::mutex> l(configLock_);
    XLOGF(DBG, "marginal-hits, threshold auto-tuning, updating from {} to {}", config_.minDiff, newValue);
    config_.minDiff = newValue;
    return true;
  }

  explicit MarginalHitsStrategyNew(Config config = {});

 protected:
  // This returns a copy of the current config.
  // This ensures that we're always looking at the same config even though
  // someone else may have updated the config during rebalancing
  Config getConfigCopy() const {
    std::lock_guard<std::mutex> l(configLock_);
    return config_;
  }

  // pick victim and receiver classes from a pool
  RebalanceContext pickVictimAndReceiverImpl(
      const CacheBase& cache,
      PoolId pid,
      const PoolStats& poolStats) override final;

  // pick victim class from a pool to shrink
  ClassId pickVictimImpl(const CacheBase& cache,
                         PoolId pid,
                         const PoolStats& poolStats) override final;
  
  size_t computeNumRequests(PoolId pid, const PoolStats& poolStats) const;

  size_t computeRequestsSinceLastDecay(PoolId pid, const PoolStats& poolStats) const;

 private:
  // compute delta of tail hits for every class in this pool
  std::unordered_map<ClassId, double> computeClassMarginalHits(
      PoolId pid, const PoolStats& poolStats, double decayFactor);

  // pick victim and receiver according to smoothed rankings
  RebalanceContext pickVictimAndReceiverFromRankings(
      PoolId pid,
      const std::unordered_map<ClassId, bool>& validVictim,
      const std::unordered_map<ClassId, bool>& validReceiver);

  RebalanceContext pickVictimAndReceiverCandidates(
      const CacheBase& cache,
      PoolId pid,
      const PoolStats& poolStats,
      bool force);

  // marginal hits states for classes in each pools
  std::unordered_map<PoolId, MarginalHitsState<ClassId>> classStates_;

  // Config for this strategy, this can be updated anytime.
  // Do not access this directly, always use `getConfig()` to
  // obtain a copy first
  Config config_;
  mutable std::mutex configLock_;
};
} // namespace cachelib
} // namespace facebook