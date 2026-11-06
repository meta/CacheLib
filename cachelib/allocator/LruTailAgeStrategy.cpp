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

#include "cachelib/allocator/LruTailAgeStrategy.h"

#include <folly/logging/xlog.h>

#include <algorithm>
#include <functional>

namespace facebook::cachelib {

LruTailAgeStrategy::LruTailAgeStrategy(Config config)
    : RebalanceStrategy(LruTailAge), config_(std::move(config)) {}

uint64_t LruTailAgeStrategy::getOldestElementAge(
    const PoolEvictionAgeStats& poolEvictionAgeStats, ClassId cid) const {
  switch (config_.queueSelector) {
  case Config::QueueSelector::kHot:
    return poolEvictionAgeStats.classEvictionAgeStats.at(cid)
        .hotQueueStat.oldestElementAge;
  case Config::QueueSelector::kWarm:
    return poolEvictionAgeStats.classEvictionAgeStats.at(cid)
        .warmQueueStat.oldestElementAge;
  case Config::QueueSelector::kCold:
    return poolEvictionAgeStats.classEvictionAgeStats.at(cid)
        .coldQueueStat.oldestElementAge;
  default:
    XDCHECK(false) << "queue selector is invalid";
    return 0;
  }
}

uint64_t LruTailAgeStrategy::getProjectedAge(
    const PoolEvictionAgeStats& poolEvictionAgeStats, ClassId cid) const {
  switch (config_.queueSelector) {
  case Config::QueueSelector::kHot:
    return poolEvictionAgeStats.classEvictionAgeStats.at(cid)
        .hotQueueStat.projectedAge;
  case Config::QueueSelector::kWarm:
    return poolEvictionAgeStats.classEvictionAgeStats.at(cid)
        .warmQueueStat.projectedAge;
  case Config::QueueSelector::kCold:
    return poolEvictionAgeStats.classEvictionAgeStats.at(cid)
        .coldQueueStat.projectedAge;
  default:
    XDCHECK(false) << "queue selector is invalid";
    return 0;
  }
}

// Filters candidates by config criteria and returns the class with highest
// weighted projected tail age. Returns kInvalidClassId if no valid victim.
// - Filter out classes with fewer than minSlabs slabs.
// - Filter out classes that recently gained a slab.
// - Prioritize classes with excessive free memory.
ClassId LruTailAgeStrategy::pickVictim(
    const Config& config,
    PoolId pid,
    const PoolStats& poolStats,
    const PoolEvictionAgeStats& poolEvictionAgeStats) {
  auto victims = poolStats.getClassIds();

  // ignore allocation classes that have fewer than the threshold of slabs.
  victims =
      filterByNumEvictableSlabs(poolStats, std::move(victims), config.minSlabs);

  // ignore allocation classes that recently gained a slab. These will be
  // growing in their eviction age and we want to let the evictions stabilize
  // before we consider them again.
  victims = filterVictimsByHoldOff(pid, poolStats, std::move(victims));

  if (victims.empty()) {
    XLOG(DBG, "Rebalancing: No victims available");
    return Slab::kInvalidClassId;
  }

  auto victimClassId = pickVictimByFreeMem(
      victims, poolStats, config.getFreeMemThreshold(), getPoolState(pid));

  if (victimClassId != Slab::kInvalidClassId) {
    return victimClassId;
  }

  // the oldest projected age among the victims
  return *std::max_element(
      victims.begin(), victims.end(), [&](ClassId a, ClassId b) {
        return (
            getProjectedAge(poolEvictionAgeStats, a) *
                (config.getWeight ? config.getWeight(pid, a, poolStats) : 1.0) <
            getProjectedAge(poolEvictionAgeStats, b) *
                (config.getWeight ? config.getWeight(pid, b, poolStats) : 1.0));
      });
}

ClassId LruTailAgeStrategy::pickReceiver(
    const Config& config,
    PoolId pid,
    const PoolStats& stats,
    ClassId victim,
    const PoolEvictionAgeStats& poolEvictionAgeStats) const {
  auto receivers = stats.getClassIds();
  receivers.erase(victim);

  receivers = filterByNoEvictions(stats, receivers, getPoolState(pid));
  if (receivers.empty()) {
    return Slab::kInvalidClassId;
  }

  // the youngest age among the potential receivers
  return *std::min_element(
      receivers.begin(), receivers.end(), [&](ClassId a, ClassId b) {
        return (getOldestElementAge(poolEvictionAgeStats, a) *
                    (config.getWeight ? config.getWeight(pid, a, stats) : 1.0) <
                getOldestElementAge(poolEvictionAgeStats, b) *
                    (config.getWeight ? config.getWeight(pid, b, stats) : 1.0));
      });
}

RebalanceContext LruTailAgeStrategy::pickVictimAndReceiverImpl(
    const CacheBase& cache, PoolId pid, const PoolStats& poolStats) {
  if (!cache.getPool(pid).allSlabsAllocated()) {
    XLOGF(DBG,
          "Pool Id: {}"
          " does not have all its slabs allocated"
          " and does not need rebalancing.",
          static_cast<int>(pid));

    return kNoOpContext;
  }

  const auto config = getConfigCopy();

  const auto poolEvictionAgeStats =
      cache.getPoolEvictionAgeStats(pid, config.slabProjectionLength);

  RebalanceContext ctx;
  ctx.victimClassId = pickVictim(config, pid, poolStats, poolEvictionAgeStats);
  ctx.receiverClassId = pickReceiver(config, pid, poolStats, ctx.victimClassId,
                                     poolEvictionAgeStats);
  if (ctx.victimClassId == ctx.receiverClassId ||
      ctx.victimClassId == Slab::kInvalidClassId ||
      ctx.receiverClassId == Slab::kInvalidClassId) {
    return kNoOpContext;
  }

  // A victim with no live items loses nothing by giving up a slab, and its
  // tail age of 0 would always fail this check.
  if (!config.getWeight &&
      poolStats.mpStats.acStats.at(ctx.victimClassId).activeAllocs > 0) {
    const auto victimProjectedTailAge =
        getProjectedAge(poolEvictionAgeStats, ctx.victimClassId);
    const auto receiverTailAge =
        getOldestElementAge(poolEvictionAgeStats, ctx.receiverClassId);

    XLOGF(DBG, "Rebalancing: receiver = {}, receiverTailAge = {}, victim = {}",
          static_cast<int>(ctx.receiverClassId), receiverTailAge,
          static_cast<int>(ctx.victimClassId));

    const auto improvement = victimProjectedTailAge - receiverTailAge;
    if (victimProjectedTailAge < receiverTailAge ||
        improvement < config.minTailAgeDifference ||
        improvement < config.tailAgeDifferenceRatio *
                          static_cast<long double>(victimProjectedTailAge)) {
      return kNoOpContext;
    }
  }

  // start a hold off so that the receiver does not become a victim soon
  // enough.
  getPoolState(pid).at(ctx.receiverClassId).startHoldOff();
  return ctx;
}

ClassId LruTailAgeStrategy::pickVictimImpl(const CacheBase& cache,
                                           PoolId pid,
                                           const PoolStats& poolStats) {
  const auto config = getConfigCopy();
  const auto poolEvictionAgeStats =
      cache.getPoolEvictionAgeStats(pid, config.slabProjectionLength);
  return pickVictim(config, pid, poolStats, poolEvictionAgeStats);
}
} // namespace facebook::cachelib
