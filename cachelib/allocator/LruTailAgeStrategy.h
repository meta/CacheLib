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

#include "cachelib/allocator/RebalanceStrategy.h"

namespace facebook {
namespace cachelib {

// This strategy picks the allocation class with the oldest projected tail age
// as the victim and the one with the youngest tail age as the receiver. A slab
// is released from the victim only if the difference between the victim's
// projected tail age and the receiver's tail age exceeds both the minimum tail
// age difference and the tail age difference ratio of the victim's projected
// tail age. A victim with no live items is released without this check.
class LruTailAgeStrategy : public RebalanceStrategy {
 public:
  struct Config : public BaseConfig {
    // the improvement (victim's projected tail age minus receiver's tail age)
    // must exceed this ratio of the victim's projected tail age for rebalancing
    double tailAgeDifferenceRatio{0.25};

    // minimum tail age difference between victim and receiver for a slab
    // rebalance to happen
    unsigned int minTailAgeDifference{100};

    // minimum number of slabs to retain in every allocation class.
    unsigned int minSlabs{1};

    // use free memory if it amounts to more than this many slabs.
    unsigned int numSlabsFreeMem{3};

    // how many slabs worth of items do we project to determine a victim.
    unsigned int slabProjectionLength{1};

    // Weight function can be changed as config parameter. By default,
    // weight function is null, and no weighted tail age is computed.
    // If the weight function is set, tailAgeDifferenceRatio and
    // minTailAgeDifference are ignored
    using WeightFn = std::function<double(
        const PoolId pid, const ClassId classId, const PoolStats& pStats)>;
    WeightFn getWeight = {};

    // This lets us specify which queue's eviction age to use.
    // Note not all eviction policies provide hot, warm, and cold queues.
    // We leave it up to the policy to determine how to define hot, warm, cold
    // eviction ages. For example, in LRU, we use the same eviction-age
    // for all three stats.
    enum class QueueSelector { kHot, kWarm, kCold };
    QueueSelector queueSelector{QueueSelector::kWarm};

    // The free memory threshold to be used to pick victim class.
    size_t getFreeMemThreshold() const noexcept {
      return numSlabsFreeMem * Slab::kSize;
    }

    Config() noexcept {}
    Config(double ratio, unsigned int _minSlabs) noexcept
        : tailAgeDifferenceRatio(ratio), minSlabs{_minSlabs} {}
    Config(double ratio,
           unsigned int _minSlabs,
           const WeightFn& _getWeight) noexcept
        : tailAgeDifferenceRatio(ratio),
          minSlabs{_minSlabs},
          getWeight(_getWeight) {}
  };

  // Updates configuration. Takes effect on next rebalancing attempt.
  void updateConfig(const BaseConfig& baseConfig) override {
    std::lock_guard<std::mutex> l(configLock_);
    config_ = static_cast<const Config&>(baseConfig);
  }

  explicit LruTailAgeStrategy(Config config = {});

  std::map<std::string, std::string> exportConfig() const override {
    return {{"rebalancer_type", fmt::format("{}", getTypeString())},
            {"tail_age_difference_ratio",
             fmt::format("{}", config_.tailAgeDifferenceRatio)},
            {"min_tail_age_difference",
             fmt::format("{}", config_.minTailAgeDifference)},
            {"min_slabs", fmt::format("{}", config_.minSlabs)},
            {"num_slabs_free_mem", fmt::format("{}", config_.numSlabsFreeMem)},
            {"slab_projection_length",
             fmt::format("{}", config_.slabProjectionLength)},
            {"queue_selector",
             fmt::format("{}", static_cast<int>(config_.queueSelector))}};
  }

 protected:
  // This returns a copy of the current config.
  // This ensures that we're always looking at the same config even though
  // someone else may have updated the config during rebalancing
  Config getConfigCopy() const {
    std::lock_guard<std::mutex> l(configLock_);
    return config_;
  }

  RebalanceContext pickVictimAndReceiverImpl(
      const CacheBase& cache,
      PoolId pid,
      const PoolStats& poolStats) override final;

  ClassId pickVictimImpl(const CacheBase& cache,
                         PoolId pid,
                         const PoolStats& poolStats) override final;

 private:
  static AllocInfo makeAllocInfo(PoolId pid,
                                 ClassId cid,
                                 const PoolStats& stats) {
    return AllocInfo{pid, cid, stats.allocSizeForClass(cid)};
  }

  ClassId pickVictim(const Config& config,
                     PoolId pid,
                     const PoolStats& stats,
                     const PoolEvictionAgeStats& poolEvictionAgeStats);

  ClassId pickReceiver(const Config& config,
                       PoolId pid,
                       const PoolStats& stats,
                       ClassId victim,
                       const PoolEvictionAgeStats& poolEvictionAgeStats) const;

  uint64_t getOldestElementAge(const PoolEvictionAgeStats& poolEvictionAgeStats,
                               ClassId cid) const;

  uint64_t getProjectedAge(const PoolEvictionAgeStats& poolEvictionAgeStats,
                           ClassId cid) const;

  // Config for this strategy, this can be updated anytime.
  // Do not access this directly, always use `getConfigCopy()` to
  // obtain a copy first
  Config config_;
  mutable std::mutex configLock_;
};
} // namespace cachelib
} // namespace facebook
