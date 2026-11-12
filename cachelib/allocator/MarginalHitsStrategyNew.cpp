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

#include "cachelib/allocator/MarginalHitsStrategyNew.h"

#include <folly/logging/xlog.h>

#include <algorithm>
#include <functional>

namespace facebook::cachelib {

MarginalHitsStrategyNew::MarginalHitsStrategyNew(Config config)
    : RebalanceStrategy(MarginalHits), config_(std::move(config)) {}

RebalanceContext MarginalHitsStrategyNew::pickVictimAndReceiverImpl(
    const CacheBase& cache, PoolId pid, const PoolStats& poolStats) {
    return pickVictimAndReceiverCandidates(cache, pid, poolStats, false);
}

RebalanceContext MarginalHitsStrategyNew::pickVictimAndReceiverCandidates(
    const CacheBase& cache, PoolId pid, const PoolStats& poolStats, bool force) {
  const auto config = getConfigCopy();
  if (!cache.getPool(pid).allSlabsAllocated()) {
    XLOGF(DBG,
          "Pool Id: {} does not have all its slabs allocated"
          " and does not need rebalancing.",
          static_cast<int>(pid));
    return kNoOpContext;
  }


  auto scores = computeClassMarginalHits(pid, poolStats, config.movingAverageParam);
  auto classesSet = poolStats.getClassIds();
  std::vector<ClassId> classes(classesSet.begin(), classesSet.end());
  std::unordered_map<ClassId, bool> validVictim;
  std::unordered_map<ClassId, bool> validReceiver;
  for (auto it : classes) {
    auto acStats = poolStats.mpStats.acStats;
    // a class can be a victim only if it has more than config.minSlabs slabs
    validVictim[it] = acStats.at(it).totalSlabs() > config.minSlabs;
    // a class can be a receiver only if its free memory (free allocs, free
    // slabs, etc) is small
    validReceiver[it] = acStats.at(it).getTotalFreeMemory() <
                        config.maxFreeMemSlabs * Slab::kSize;
  }
  if (classStates_[pid].entities.empty()) {
    // initialization
    classStates_[pid].entities = classes;
    for (auto cid : classes) {
      classStates_[pid].smoothedRanks[cid] = 0;
    }
  }
  // we don't rely on this decay anymore
  classStates_[pid].updateRankings(scores, 0.0);
  RebalanceContext ctx = pickVictimAndReceiverFromRankings(pid, validVictim, validReceiver);

  auto numRequestObserved = computeNumRequests(pid, poolStats);
  if(!force && numRequestObserved < config.minRequestsObserved) {
    XLOGF(DBG, "haven't observed enough requests: {}/{}, wait until next round", numRequestObserved, config.minRequestsObserved);
    ctx = kNoOpContext;
  }
  if(!force && ctx.isEffective()) {
    //extra filterings
    auto receiverScore = scores.at(ctx.receiverClassId);
    auto victimScore = scores.at(ctx.victimClassId);
    auto improvement = receiverScore - victimScore;
    auto improvementRatio = improvement / (victimScore == 0 ? 1 : victimScore);
    ctx.diffValue = improvement;
    if ((config.minDiff > 0 && improvement < config.minDiff) || 
        (config.minDiffRatio > 0 && improvementRatio < config.minDiffRatio)){
        XLOGF(DBG, "Not enough to trigger rebalancing, receiver id: {}, victim id: {}, receiver score: {}, victim score: {}, improvement: {}, improvement ratio: {}, thresh1: {}, thresh2: {}",
              ctx.receiverClassId, ctx.victimClassId, receiverScore, victimScore, improvement, improvementRatio, config.minDiff, config.minDiffRatio);
        ctx = kNoOpContext;
    } else {
        XLOGF(DBG, "rebalancing, receiver id: {}, victim id: {}, receiver score: {}, victim score: {}, improvement: {}, improvement ratio: {}",
              ctx.receiverClassId, ctx.victimClassId, receiverScore, victimScore, improvement, improvementRatio);

    }
  } 

  if(!ctx.isEffective()){
    ctx = kNoOpContext;
  }
  auto& poolState = getPoolState(pid);
  auto deltaRequestsSinceLastDecay = computeRequestsSinceLastDecay(pid, poolStats);
  if((ctx.isEffective() || !config.onlyUpdateHitIfRebalance) || deltaRequestsSinceLastDecay >= config.minDecayInterval) {  
    for (const auto i : poolStats.getClassIds()) {
        poolState[i].updateTailHits(poolStats, config.movingAverageParam);
    }
  }

  if(numRequestObserved >= config.minRequestsObserved) {
    for (const auto i : poolStats.getClassIds()) {
      poolState[i].updateRequests(poolStats);
    }
  }

  // self-tuning threshold for the next round.
  if(ctx.isEffective()){
    // max window size: 2 * n_classes

    size_t classWithHits = 0;
    for (const auto& cid : classes) {
        if (poolState.at(cid).deltaHits(poolStats) > 0) {
            ++classWithHits;
        }
    }
    recordRebalanceEvent(pid, ctx, classWithHits * 2);
    auto effectiveMoveRate = queryEffectiveMoveRate(pid);
    auto windowSize = getRebalanceEventQueueSize(pid);
    XLOGF(DBG, 
          "Rebalancing: effective move rate = {}, window size = {}, diff = {}, threshold = {}, ({}->{})",
          effectiveMoveRate,
          windowSize, ctx.diffValue, config.minDiff, static_cast<int>(ctx.victimClassId), static_cast<int>(ctx.receiverClassId));
  
    if(effectiveMoveRate <= config.emrLow && windowSize >= config.thresholdIncMinWindowSize) {
        if(config.thresholdAI) {
          auto currentMin = getMinDiffValueFromRebalanceEvents(pid);
          if(updateMinDff(currentMin + config.thresholdAIADStep)) {
            clearPoolRebalanceEvent(pid);
          }
        } else if (config.thresholdMI){
          if(updateMinDff(config.minDiff * config.thresholdMIMDFactor)) {
            clearPoolRebalanceEvent(pid);
          }
        }
        
    } else if (effectiveMoveRate >= config.emrHigh && windowSize >= classWithHits) {
        if(config.thresholdAD) {
          if(updateMinDff(std::max(2.0, config.minDiff - config.thresholdAIADStep))) {
            clearPoolRebalanceEvent(pid);
          }
        } else if (config.thresholdMD){
          if(updateMinDff(std::max(2.0, config.minDiff / config.thresholdMIMDFactor))) {
            clearPoolRebalanceEvent(pid);
          }
        }
    }
  }
  
  return ctx;
}

ClassId MarginalHitsStrategyNew::pickVictimImpl(const CacheBase& cache,
                                             PoolId pid,
                                             const PoolStats& stats) {
  return pickVictimAndReceiverCandidates(cache, pid, stats, true).victimClassId;
}

std::unordered_map<ClassId, double>
MarginalHitsStrategyNew::computeClassMarginalHits(PoolId pid,
                                               const PoolStats& poolStats,
                                               double decayFactor) {
  const auto& poolState = getPoolState(pid);
  std::unordered_map<ClassId, double> scores;
  for (auto info : poolState) {
    if (info.id != Slab::kInvalidClassId) {
      // this score is the latest delta.
      scores[info.id] = info.getDecayedMarginalHits(poolStats, decayFactor);
    }
  }
  return scores;
}

size_t MarginalHitsStrategyNew::computeNumRequests(
    PoolId pid, const PoolStats& poolStats) const {
  const auto& poolState = getPoolState(pid);
  size_t totalRequests = 0;
  auto classesSet = poolStats.getClassIds();
  for (const auto& cid : classesSet) {
    totalRequests += poolState.at(cid).deltaRequests(poolStats);
  }
  return totalRequests;
}

size_t MarginalHitsStrategyNew::computeRequestsSinceLastDecay(
    PoolId pid, const PoolStats& poolStats) const {
  const auto& poolState = getPoolState(pid);
  size_t totalRequests = 0;
  auto classesSet = poolStats.getClassIds();
  for (const auto& cid : classesSet) {
    totalRequests += poolState.at(cid).deltaRequestsSinceLastDecay(poolStats);
  }
  return totalRequests;
}

RebalanceContext MarginalHitsStrategyNew::pickVictimAndReceiverFromRankings(
    PoolId pid,
    const std::unordered_map<ClassId, bool>& validVictim,
    const std::unordered_map<ClassId, bool>& validReceiver) {
  auto victimAndReceiver = classStates_[pid].pickVictimAndReceiverFromRankings(
      validVictim, validReceiver, Slab::kInvalidClassId);
  RebalanceContext ctx{victimAndReceiver.first, victimAndReceiver.second};
  if (ctx.victimClassId == Slab::kInvalidClassId ||
      ctx.receiverClassId == Slab::kInvalidClassId ||
      ctx.victimClassId == ctx.receiverClassId) {
    return kNoOpContext;
  }

  XLOGF(DBG,
        "Rebalancing: receiver = {}, smoothed rank = {}, victim = {}, smoothed "
        "rank = {}",
        static_cast<int>(ctx.receiverClassId),
        classStates_[pid].smoothedRanks[ctx.receiverClassId],
        static_cast<int>(ctx.victimClassId),
        classStates_[pid].smoothedRanks[ctx.victimClassId]);
  return ctx;
}
} // namespace facebook::cachelib