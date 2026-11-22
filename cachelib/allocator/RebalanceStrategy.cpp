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

#include "cachelib/allocator/RebalanceStrategy.h"

#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/logging/xlog.h>

#include <algorithm>

namespace facebook::cachelib {

using detail::Info;

const RebalanceContext RebalanceStrategy::kNoOpContext = {
    Slab::kInvalidClassId, Slab::kInvalidClassId};

void RebalanceStrategy::recordCurrentState(PoolId pid, const PoolStats& stats) {
  auto& currRecord = poolState_[pid];
  for (const auto i : stats.getClassIds()) {
    currRecord[i].updateRecord(stats);
  }
}

ClassId TotalSlabsStrategy::pickAllocClassForResizing(
    const CacheBase&, PoolId, const PoolStats& stats) const {
  const auto& candidates = stats.mpStats.classIds;

  // Pick victim by maximum number of slabs
  const auto ret = *std::max_element(
      candidates.begin(), candidates.end(), [&](ClassId a, ClassId b) {
        return stats.mpStats.acStats.at(a).totalSlabs() <
               stats.mpStats.acStats.at(b).totalSlabs();
      });

  // if we dont find any, it is likely that the pool has its slabs in its
  // private free list.
  if (stats.mpStats.acStats.at(ret).totalSlabs() == 0) {
    return Slab::kInvalidClassId;
  }

  return ret;
}

ClassId AllocationSizeStrategy::pickAllocClassForResizing(
    const CacheBase& cache, PoolId pid, const PoolStats& stats) const {
  const auto& candidates = stats.mpStats.classIds;

  // Filter by minSlabsForResizing
  std::vector<ClassId> eligibleClasses;
  for (const auto& classId : candidates) {
    if (stats.mpStats.acStats.at(classId).totalSlabs() >=
        config_.minSlabsForResizing) {
      eligibleClasses.push_back(classId);
    }
  }

  if (!eligibleClasses.empty()) {
    // Sort by allocation size (largest first)
    std::sort(eligibleClasses.begin(), eligibleClasses.end(),
              [&](ClassId a, ClassId b) {
                return stats.allocSizeForClass(a) > stats.allocSizeForClass(b);
              });

    // Pick one randomly from the top numClassesToPickFrom classes
    const size_t numToConsider =
        std::min(static_cast<size_t>(config_.numClassesToPickFrom),
                 eligibleClasses.size());
    const size_t randomIndex = folly::Random::rand64(numToConsider);
    return eligibleClasses[randomIndex];
  }

  // Fallback if no eligible classes: use TotalSlabs heuristic
  TotalSlabsStrategy fallback;
  return fallback.pickAllocClassForResizing(cache, pid, stats);
}

void RebalanceStrategy::initPoolState(PoolId pid, const PoolStats& stats) {
  // default initialize
  auto& curr = poolState_[pid];
  for (auto id : stats.getClassIds()) {
    curr[id] =
        Info{id, stats.mpStats.acStats.at(id).totalSlabs(),
             stats.cacheStats.at(id).numEvictions(), stats.numHitsForClass(id),
             stats.cacheStats.at(id).containerStat.numTailAccesses,
             0,
             stats.numHitsForClass(id) + stats.cacheStats.at(id).allocAttempts,
             stats.numHitsForClass(id) + stats.cacheStats.at(id).allocAttempts}; // hits + allocs => nr of requests
  }
}

ClassId RebalanceStrategy::pickVictimByFreeMem(const std::set<ClassId>& victims,
                                               const PoolStats& stats,
                                               size_t threshold,
                                               const PoolState& prevState) {
  // filter out evicting alloc classes. Since these are evicting, the free
  // allocs is probably needed here.
  const auto nonEvicting = filter(
      victims,
      [&](ClassId id) { return prevState.at(id).getDeltaEvictions(stats) > 0; },
      "filtering evicting classes for free-mem");

  if (nonEvicting.empty()) {
    return Slab::kInvalidClassId;
  }

  const auto& mpStats = stats.mpStats;
  const auto it =
      std::max_element(nonEvicting.cbegin(),
                       nonEvicting.cend(),
                       [&mpStats](const ClassId& a, const ClassId& b) {
                         const auto& acStats = mpStats.acStats;
                         return acStats.at(a).getTotalFreeMemory() <
                                acStats.at(b).getTotalFreeMemory();
                       });

  if (threshold == 0 ||
      mpStats.acStats.at(*it).getTotalFreeMemory() <= threshold) {
    return Slab::kInvalidClassId;
  }

  return *it;
}

// filter the candidates by amount of evictable slabs
std::set<ClassId> RebalanceStrategy::filterByNumEvictableSlabs(
    const PoolStats& poolStats,
    std::set<ClassId> candidates,
    unsigned int minSlabs) {
  auto condition = [&](const ClassId& id) {
    const auto& acStat = poolStats.mpStats.acStats.at(id);
    return acStat.totalSlabs() <= minSlabs;
  };
  return filter(std::move(candidates), condition,
                "remove candidates by numSlabs");
}

std::set<ClassId> RebalanceStrategy::filterByAllocFailure(
    const PoolStats& stats,
    std::set<ClassId> candidates,
    const PoolState& prevState) {
  return filter(
      std::move(candidates),
      [&](ClassId cid) {
        return prevState.at(cid).deltaAllocFailures(stats) == 0;
      },
      "candidates with allocation failures");
}

std::set<ClassId> RebalanceStrategy::filterByNoEvictions(
    const PoolStats& stats,
    std::set<ClassId> candidates,
    const PoolState& prevState) {
  return filter(
      std::move(candidates),
      [&](ClassId cid) {
        return stats.mpStats.acStats.at(cid).freeSlabs > 0 ||
               prevState.at(cid).getDeltaEvictions(stats) == 0;
      },
      " candidates with free memory or no evictions");
}

std::set<ClassId> RebalanceStrategy::filterByMinTailAge(
    const PoolStats& stats,
    std::set<ClassId> candidates,
    unsigned int minTailAge) {
  return filter(
      std::move(candidates),
      [&](ClassId cid) { return stats.evictionAgeForClass(cid) < minTailAge; },
      fmt::format(" candidates with less than {} seconds for tail age",
                  minTailAge));
}

std::set<ClassId> RebalanceStrategy::filterByMaxTailAge(
    const PoolStats& stats,
    std::set<ClassId> candidates,
    unsigned int maxTailAge) {
  return filter(
      std::move(candidates),
      [&](ClassId cid) { return stats.evictionAgeForClass(cid) > maxTailAge; },
      fmt::format(" candidates with more than {} seconds for tail age",
                  maxTailAge));
}

std::set<ClassId> RebalanceStrategy::filter(
    std::set<ClassId> input,
    std::function<bool(const ClassId& id)> pred,
    const std::string& purpose) {
  if (input.empty()) {
    return input;
  }

  const auto before = input.size();

  auto it = input.begin();
  while (it != input.end()) {
    if (pred(*it)) {
      it = input.erase(it);
    } else {
      ++it;
    }
  }

  const auto after = input.size();
  XLOGF(DBG,
        "Rebalancing: filtered out {} classes for {}. Remaining: {}",
        before - after,
        purpose,
        after);
  return input;
}

std::set<ClassId> RebalanceStrategy::filterVictimsByHoldOff(
    PoolId pid, const PoolStats& stats, std::set<ClassId> victims) {
  auto condition = [this, &stats, pid](const ClassId& id) {
    auto& currRecord = poolState_.at(pid).at(id);
    if (!currRecord.isOnHoldOff()) {
      return false;
    }

    if (currRecord.getDeltaEvictions(stats) == 0) {
      currRecord.reduceHoldOff();
      // filter
      return true;
    } else {
      currRecord.resetHoldOff();
      return false;
    }
  };
  return filter(std::move(victims), condition, "remove recent receivers");
}

std::set<ClassId> RebalanceStrategy::filterReceiversByTargetEvictionAge(
    const PoolStats& stats,
    std::set<ClassId> candidates,
    const folly::F14FastMap<uint32_t, uint32_t>& map) {
  return filter(
      std::move(candidates),
      [&](ClassId cid) {
        auto it = map.find(cid);
        // filter out AC not in map or with evictionAge above target
        return (it == map.end() ||
                (stats.evictionAgeForClass(cid) >= it->second));
      },
      " candidates greater than or equal to eviction age target");
}

std::set<ClassId> RebalanceStrategy::filterVictimsByTargetEvictionAge(
    const PoolStats& stats,
    std::set<ClassId> candidates,
    const folly::F14FastMap<uint32_t, uint32_t>& map) {
  return filter(
      std::move(candidates),
      [&](ClassId cid) {
        auto it = map.find(cid);
        return (it != map.end() &&
                (stats.evictionAgeForClass(cid) < it->second));
      },
      " candidates below eviction age target");
}

RebalanceContext RebalanceStrategy::pickVictimAndReceiver(
    const CacheBase& cache, PoolId pid) {
  return executeAndRecordCurrentState<RebalanceContext>(
      cache,
      pid,
      [&](const PoolStats& stats) {
        // Pick receiver based on allocation failures. If nothing found,
        // fall back to strategy specific Impl
        RebalanceContext ctx;
        ctx.receiverClassId = pickReceiverWithAllocFailures(cache, pid, stats);
        if (ctx.receiverClassId != Slab::kInvalidClassId) {
          ctx.victimClassId = pickVictimImpl(cache, pid, stats);
          if (ctx.victimClassId == cachelib::Slab::kInvalidClassId) {
            ctx.victimClassId = pickVictimStrategy_->pickAllocClassForResizing(
                cache, pid, stats);
          }
          if (ctx.victimClassId != Slab::kInvalidClassId &&
              ctx.victimClassId != ctx.receiverClassId &&
              getPoolState(pid).at(ctx.victimClassId).nSlabs > 1) {
            // start a hold off so that the receiver does not become a victim
            // soon enough.
            getPoolState(pid).at(ctx.receiverClassId).startHoldOff();
            return ctx;
          }
        }
        return pickVictimAndReceiverImpl(cache, pid, stats);
      },
      kNoOpContext);
}

ClassId RebalanceStrategy::pickVictimForResizing(const CacheBase& cache,
                                                 PoolId pid) {
  // Pick only the victim irrespective of who is receiving the slab. This is
  // used mostly for pool resizing.
  auto victimClassId = executeAndRecordCurrentState<ClassId>(
      cache,
      pid,
      [&](const PoolStats& stats) { return pickVictimImpl(cache, pid, stats); },
      Slab::kInvalidClassId);

  if (victimClassId == cachelib::Slab::kInvalidClassId) {
    const auto poolStats = cache.getPoolStats(pid);
    victimClassId =
        pickVictimStrategy_->pickAllocClassForResizing(cache, pid, poolStats);
  }

  return victimClassId;
}

ClassId RebalanceStrategy::pickReceiverWithAllocFailures(
    const CacheBase&, PoolId pid, const PoolStats& stats) {
  auto receivers = stats.getClassIds();

  const auto receiverWithAllocFailures =
      filterByAllocFailure(stats, std::move(receivers), getPoolState(pid));
  if (receiverWithAllocFailures.empty()) {
    return Slab::kInvalidClassId;
  }

  const auto& poolState = getPoolState(pid);
  // pick the receiver with the most allocation failures
  return *std::max_element(receiverWithAllocFailures.begin(),
                           receiverWithAllocFailures.end(),
                           [&](ClassId a, ClassId b) {
                             return poolState.at(a).deltaAllocFailures(stats) <
                                    poolState.at(b).deltaAllocFailures(stats);
                           });
}

template <typename T>
T RebalanceStrategy::executeAndRecordCurrentState(
    const CacheBase& cache,
    PoolId pid,
    const std::function<T(const PoolStats&)>& impl,
    T noOp) {
  const auto poolStats = cache.getPoolStats(pid);

  // if this is the first time we are encountering this pool, initialize
  // the state and not do anything at the moment.
  if (!poolStatePresent(pid)) {
    initPoolState(pid, poolStats);
    return noOp;
  }

  auto rv = impl(poolStats);

  recordCurrentState(pid, poolStats);

  return rv;
}

///// for keeping rebalance decision histories
void RebalanceStrategy::recordRebalanceEvent(PoolId pid, RebalanceContext ctx, size_t maxQueueSize) {
  if(ctx.isEffective()) {
    auto& eventQueue = recentRebalanceEvents_[pid];
    eventQueue.emplace_back(ctx);
    if (eventQueue.size() > maxQueueSize) {
      eventQueue.pop_front();
    }
  }
}

unsigned int RebalanceStrategy::getRebalanceEventQueueSize(PoolId pid) const {
  const auto it = recentRebalanceEvents_.find(pid);
  if (it == recentRebalanceEvents_.end()) {
    return 0;
  }
  return it->second.size();
}

void RebalanceStrategy::clearPoolRebalanceEvent(PoolId pid) {
  recentRebalanceEvents_.erase(pid);
}

double RebalanceStrategy::queryEffectiveMoveRate(PoolId pid) const{
  const auto it = recentRebalanceEvents_.find(pid);
  if (it == recentRebalanceEvents_.end() || it->second.empty()) {
      return 1.0;
  }

  const auto& events = it->second;
  std::unordered_map<ClassId, int> netChanges;

  for (const auto& ctx : events) {
      netChanges[ctx.victimClassId]--;
      netChanges[ctx.receiverClassId]++;
  }

  int currentAbsNet = 0;
  for (const auto& [classId, net] : netChanges) {
      currentAbsNet += std::abs(net);
  }
  int totalEffectiveMoves = currentAbsNet / 2;

  return static_cast<double>(totalEffectiveMoves) / events.size();
}

double RebalanceStrategy::getMinDiffValueFromRebalanceEvents(PoolId pid) const {
  const auto it = recentRebalanceEvents_.find(pid);
  if (it == recentRebalanceEvents_.end() || it->second.empty()) {
    return 0.0; // Return 0.0 if the queue is empty or the pool ID is not found
  }

  const auto& events = it->second;

  // Find the minimum diffValue in the queue
  double minDiffValue = std::numeric_limits<double>::max();
  for (const auto& ctx : events) {
    minDiffValue = std::min(minDiffValue, ctx.diffValue);
  }

  return minDiffValue;
}

} // namespace facebook::cachelib
