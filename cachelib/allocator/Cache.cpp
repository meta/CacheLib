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

#include "cachelib/allocator/Cache.h"

#include <folly/logging/xlog.h>

#include <mutex>

#include "cachelib/allocator/RebalanceStrategy.h"
#include "cachelib/common/PercentileStats.h"

namespace facebook::cachelib {

void CacheBase::setRebalanceStrategy(
    PoolId pid, std::shared_ptr<RebalanceStrategy> strategy) {
  std::unique_lock<std::mutex> l(lock_);
  poolRebalanceStrategies_[pid] = std::move(strategy);
}

std::shared_ptr<RebalanceStrategy> CacheBase::getRebalanceStrategy(
    PoolId pid) const {
  std::unique_lock<std::mutex> l(lock_);
  auto it = poolRebalanceStrategies_.find(pid);
  if (it != poolRebalanceStrategies_.end() && it->second) {
    return it->second;
  }
  return nullptr;
}

void CacheBase::setResizeStrategy(PoolId pid,
                                  std::shared_ptr<RebalanceStrategy> strategy) {
  std::unique_lock<std::mutex> l(lock_);
  poolResizeStrategies_[pid] = std::move(strategy);
}

std::shared_ptr<RebalanceStrategy> CacheBase::getResizeStrategy(
    PoolId pid) const {
  std::unique_lock<std::mutex> l(lock_);
  auto it = poolResizeStrategies_.find(pid);
  if (it != poolResizeStrategies_.end() && it->second) {
    return it->second;
  }
  return nullptr;
}

void CacheBase::setPoolOptimizeStrategy(
    std::shared_ptr<PoolOptimizeStrategy> strategy) {
  std::unique_lock<std::mutex> l(lock_);
  poolOptimizeStrategy_ = std::move(strategy);
}

std::shared_ptr<PoolOptimizeStrategy> CacheBase::getPoolOptimizeStrategy()
    const {
  std::unique_lock<std::mutex> l(lock_);
  return poolOptimizeStrategy_;
}

void CacheBase::visitEstimates(const util::CounterVisitor& v,
                               const util::PercentileStats::Estimates& est,
                               folly::StringPiece name) {
  util::PercentileStats::visitQuantileEstimates(v, est, name);
}

void CacheBase::updateObjectCacheStats(const std::string& statPrefix) const {
  getObjectCacheCounters(
      {[this, &statPrefix](folly::StringPiece key, uint64_t value,
                           util::CounterVisitor::CounterType type) {
        std::string prefix = statPrefix + key.str();
        if (type == util::CounterVisitor::CounterType::RATE) {
          counters_.updateDelta(prefix, value);
        } else {
          counters_.updateCount(prefix, value);
        }
      }});
}

void CacheBase::updatePoolStats(const std::string& statPrefix,
                                PoolId pid) const {
  const PoolStats stats = getPoolStats(pid);
  updatePoolStats(statPrefix, stats);
}

void CacheBase::updatePoolStats(const std::string& statPrefix,
                                const PoolStats& stats) const {
  const std::string prefix = statPrefix + "pool." + stats.poolName + ".";

  counters_.updateCount(prefix + "size", stats.poolSize);
  counters_.updateCount(prefix + "usable_size", stats.poolUsableSize);
  counters_.updateCount(prefix + "advised_size", stats.poolAdvisedSize);
  counters_.updateDelta(prefix + "alloc.attempts", stats.numAllocAttempts());
  counters_.updateDelta(prefix + "alloc.failures", stats.numAllocFailures());
  counters_.updateCount(prefix + "alloc.active", stats.numActiveAllocs());
  counters_.updateCount(prefix + "alloc.free", stats.numFreeAllocs());

  const std::string evictionKey = prefix + "evictions";
  counters_.updateDelta(evictionKey, stats.numEvictions());
  uint64_t evictionDelta = counters_.getDelta(evictionKey);

  counters_.updateCount(prefix + "items", stats.numItems());
  counters_.updateDelta(prefix + "hits", stats.numPoolGetHits);
  counters_.updateCount(prefix + "free_memory_bytes", stats.freeMemoryBytes());
  counters_.updateCount(prefix + "slabs.free", stats.mpStats.freeSlabs);
  counters_.updateCount(prefix + "slabs.advised", stats.mpStats.numSlabAdvise);
  counters_.updateCount(prefix + "slabs.rebalanced",
                        stats.mpStats.numSlabRebalance);
  counters_.updateCount(prefix + "slabs.resized", stats.mpStats.numSlabResize);
  counters_.updateCount(prefix + "slabs.total", stats.mpStats.allocatedSlabs());
  counters_.updateCount(prefix + "slabs.unallocated",
                        stats.mpStats.slabsUnAllocated);
  counters_.updateCount(prefix + "fragmentation_bytes",
                        stats.totalFragmentation());
  counters_.updateCount(prefix + "allocated_bytes",
                        stats.poolUsableSize - stats.freeMemoryBytes() -
                            stats.totalFragmentation());

  util::CounterVisitor uploadStats{[this](folly::StringPiece name, double val) {
    counters_.updateCount(name.toString(), static_cast<uint64_t>(val));
  }};

  // evictionAgeSecs are only populated if the evictions actually happen
  if (evictionDelta) {
    visitEstimates(uploadStats, stats.evictionAgeSecs,
                   prefix + "eviction_age_secs");
  }

  // the below stats are populated by looking at the tail of MMContainers. So
  // these will be populated irrespective of whether evictions happen.
  counters_.updateCount(prefix + "evictions.age.min", stats.minEvictionAge());
  counters_.updateCount(prefix + "evictions.age.max", stats.maxEvictionAge());
}

void CacheBase::updateCompactCacheStats(const std::string& statPrefix,
                                        const ICompactCache& c) const {
  const std::string prefix = statPrefix + "ccache." + c.getName() + ".";
  const auto& stats = c.getStats();

  counters_.updateCount(prefix + "size", c.getSize());

  counters_.updateDelta(prefix + "get.total", stats.get);
  counters_.updateDelta(prefix + "get.hits", stats.getHit);
  counters_.updateDelta(prefix + "get.miss", stats.getMiss);
  counters_.updateDelta(prefix + "get.errs", stats.getErr);
  counters_.updateDelta(prefix + "get.tailHits", stats.tailHits);

  counters_.updateDelta(prefix + "set.total", stats.set);
  counters_.updateDelta(prefix + "set.replace", stats.setHit);
  counters_.updateDelta(prefix + "set.inserts", stats.setMiss);

  counters_.updateDelta(prefix + "evictions", stats.evictions);

  counters_.updateDelta(prefix + "del.total", stats.del);
  counters_.updateDelta(prefix + "del.hits", stats.delHit);
  counters_.updateDelta(prefix + "del.miss", stats.delMiss);
  counters_.updateDelta(prefix + "del.errs", stats.delErr);

  counters_.updateDelta(prefix + "purge.errs", stats.purgeErr);
  counters_.updateDelta(prefix + "purge.success", stats.purgeSuccess);

  counters_.updateDelta(prefix + "timeout.lock", stats.lockTimeout);
  counters_.updateDelta(prefix + "timeout.promote", stats.promoteTimeout);

  const double hitRate =
      util::hitRatioCalc(counters_.getDelta(prefix + "get.total"),
                         counters_.getDelta(prefix + "get.miss"));
  counters_.updateCount(prefix + "hit_rate",
                        util::narrow_cast<uint64_t>(hitRate));
}

void CacheBase::updateLegacyEventTrackerStats(
    const std::string& statPrefix) const {
  const std::string prefix = statPrefix + "legacy_event_tracker.";
  for (const auto& kv : getLegacyEventTrackerStatsMap()) {
    counters_.updateCount(prefix + kv.first, kv.second);
  }
}

void CacheBase::updateEventTrackerStats(const std::string& statPrefix) const {
  const std::string prefix = statPrefix + "event_tracker.";
  for (const auto& kv : getEventTrackerStatsMap()) {
    counters_.updateDelta(prefix + kv.first, kv.second);
  }
}

void CacheBase::updateNvmCacheStats(const std::string& statPrefix) const {
  const std::string prefix = statPrefix + "nvm.";
  auto statsMap = getNvmCacheStatsMap();
  for (const auto& kv : statsMap.getCounts()) {
    counters_.updateCount(prefix + kv.first,
                          util::narrow_cast<uint64_t>(kv.second));
  }

  for (const auto& kv : statsMap.getRates()) {
    counters_.updateDelta(prefix + kv.first,
                          util::narrow_cast<uint64_t>(kv.second));
  }
}

void CacheBase::updateGlobalCacheStats(const std::string& statPrefix) const {
  auto getPct = [](uint64_t s, uint64_t d) {
    double res = d == 0 ? 0.0 : (s * 100.0 / d);
    return util::narrow_cast<uint64_t>(res);
  };

  auto uploadStatsNanoToMicro =
      util::CounterVisitor{[this](folly::StringPiece name, double val) {
        constexpr unsigned int nanosInMicro = 1000;
        counters_.updateCount(name.toString(),
                              static_cast<uint64_t>(val) / nanosInMicro);
      }};

  auto uploadStats =
      util::CounterVisitor{[this](folly::StringPiece name, double val) {
        counters_.updateCount(name.toString(), static_cast<uint64_t>(val));
      }};

  const auto memStats = getCacheMemoryStats();
  counters_.updateCount(statPrefix + "mem.advised_size", memStats.advisedSize);
  counters_.updateCount(
      statPrefix + "mem.advised_size.cache_pct",
      getPct(memStats.advisedSize, memStats.configuredRamCacheRegularSize));
  counters_.updateCount(
      statPrefix + "mem.advised_size.pct",
      getPct(memStats.advisedSize,
             memStats.maxAdvisedPct * memStats.configuredRamCacheRegularSize /
                 100));
  counters_.updateCount(statPrefix + "mem.system_free",
                        memStats.memAvailableSize);
  counters_.updateCount(statPrefix + "mem.process_rss", memStats.memRssSize);
  counters_.updateCount(statPrefix + "mem.size", memStats.ramCacheSize);
  counters_.updateCount(statPrefix + "mem.size.configured",
                        memStats.configuredRamCacheSize);
  counters_.updateCount(statPrefix + "mem.size.configured.regular",
                        memStats.configuredRamCacheRegularSize);
  counters_.updateCount(statPrefix + "mem.size.configured.compact",
                        memStats.configuredRamCacheCompactSize);

  counters_.updateCount(statPrefix + "mem.usable_size",
                        memStats.usableRamCacheSize());

  counters_.updateCount(statPrefix + "mem.unreserved_size",
                        memStats.unReservedSize);

  counters_.updateCount(statPrefix + "mem.access_container_size",
                        memStats.accessContainerSize);
  counters_.updateCount(statPrefix + "mem.chained_item_access_container_size",
                        memStats.chainedItemAccessContainerSize);

  counters_.updateCount(statPrefix + "nvm.size", memStats.nvmCacheSize);
  counters_.updateCount(
      statPrefix + "cache.size.configured",
      memStats.configuredRamCacheSize + memStats.nvmCacheSize);

  const auto stats = getGlobalCacheStats();

  // Eviction Stats
  //   Note that ram evictions can be higher than cache evictions. For example,
  //   if an item is evicted from ram but it still has a copy in nvm, then
  //   this does not count as an eviction. Only when this is item is evicted
  //   from both ram and nvm, this is counted as a single eviction from cache.
  // Ram Evictions: item evicted from ram but it can be inserted into nvm
  const std::string ramEvictionKey = statPrefix + "ram.evictions";
  counters_.updateDelta(ramEvictionKey, stats.numEvictions);
  // Nvm Evictions: item evicted from nvm but it can be still in ram
  const std::string nvmEvictionKey = statPrefix + "nvm.evictions";
  counters_.updateDelta(nvmEvictionKey, stats.numNvmEvictions);
  // Cache Evictions: item leaves the cache entirely because of evictions from
  //                  ram or nvm. No need to enable itemDestructor
  counters_.updateDelta(statPrefix + "cache.evictions",
                        stats.numCacheEvictions);

  // Destructor Stats
  //   These are only populated when destructor callback is supplied. They
  //   are only called when an item is removed/evicted from both ram and nvm.
  counters_.updateDelta(statPrefix + "cache.destructor_calls.ram",
                        stats.numRamDestructorCalls);
  counters_.updateDelta(statPrefix + "cache.destructor_calls.nvm",
                        stats.numNvmDestructorCalls);

  // get the new delta to see if uploading any eviction age stats or lifetime
  // stats makes sense.
  uint64_t ramEvictionDelta = counters_.getDelta(ramEvictionKey);
  if (ramEvictionDelta) {
    visitEstimates(uploadStats, stats.ramEvictionAgeSecs,
                   statPrefix + "ram.eviction_age_secs");
    visitEstimates(uploadStats, stats.ramItemLifeTimeSecs,
                   statPrefix + "ram.item_lifetime_secs");
  }

  // get the new delta to see if uploading any eviction age stats or lifetime
  // stats makes sense.
  uint64_t nvmEvictionDelta = counters_.getDelta(nvmEvictionKey);
  if (nvmEvictionDelta) {
    visitEstimates(uploadStats, stats.nvmSmallLifetimeSecs,
                   statPrefix + "nvm.item_lifetime_secs.small");
    visitEstimates(uploadStats, stats.nvmLargeLifetimeSecs,
                   statPrefix + "nvm.item_lifetime_secs.large");
    visitEstimates(uploadStats, stats.nvmEvictionSecondsPastExpiry,
                   statPrefix + "nvm.evictions.secs_past_expiry");
    visitEstimates(uploadStats, stats.nvmEvictionSecondsToExpiry,
                   statPrefix + "nvm.evictions.secs_to_expiry");
  }

  counters_.updateDelta(statPrefix + "cache.alloc_attempts",
                        stats.allocAttempts);
  counters_.updateDelta(statPrefix + "cache.eviction_attempts",
                        stats.evictionAttempts);
  counters_.updateDelta(statPrefix + "cache.alloc_failures",
                        stats.allocFailures);
  counters_.updateDelta(statPrefix + "cache.invalid_allocs",
                        stats.invalidAllocs);

  counters_.updateDelta(statPrefix + "cache.gets", stats.numCacheGets);
  counters_.updateDelta(statPrefix + "cache.gets.miss", stats.numCacheGetMiss);
  counters_.updateDelta(statPrefix + "cache.gets.expiries",
                        stats.numCacheGetExpiries);
  counters_.updateDelta(statPrefix + "cache.removes", stats.numCacheRemoves);
  counters_.updateDelta(statPrefix + "cache.removes.ram_hits",
                        stats.numCacheRemoveRamHits);
  counters_.updateDelta(statPrefix + "cache.refcount_overflows",
                        stats.numRefcountOverflow);
  counters_.updateDelta(statPrefix + "cache.destructors.exceptions",
                        stats.numDestructorExceptions);
  counters_.updateDelta(statPrefix + "cache.aborted_slab_releases",
                        stats.numAbortedSlabReleases);

  counters_.updateDelta(statPrefix + "reaper.visited_items",
                        stats.reaperStats.numVisitedItems);
  counters_.updateDelta(statPrefix + "reaper.reaped_items",
                        stats.reaperStats.numReapedItems);
  counters_.updateDelta(statPrefix + "reaper.visit_errs",
                        stats.reaperStats.numVisitErrs);
  counters_.updateDelta(statPrefix + "reaper.traverses",
                        stats.reaperStats.numTraversals);
  counters_.updateCount(statPrefix + "reaper.latency.traverse_last_ms",
                        stats.reaperStats.lastTraversalTimeMs);
  counters_.updateCount(statPrefix + "reaper.latency.traverse_avg_ms",
                        stats.reaperStats.avgTraversalTimeMs);
  counters_.updateDelta(statPrefix + "reaper.skipped_slabs",
                        stats.numReaperSkippedSlabs);

  counters_.updateDelta(statPrefix + "rebalancer.runs",
                        stats.rebalancerStats.numRuns);
  counters_.updateDelta(statPrefix + "rebalancer.rebalanced_slabs",
                        stats.rebalancerStats.numRebalancedSlabs);
  counters_.updateCount(statPrefix + "rebalancer.latency.loop_last_ms",
                        stats.rebalancerStats.lastRebalanceTimeMs);
  counters_.updateCount(statPrefix + "rebalancer.latency.loop_avg_ms",
                        stats.rebalancerStats.avgRebalanceTimeMs);

  counters_.updateCount(statPrefix + "rebalancer.latency.release_last_ms",
                        stats.rebalancerStats.lastReleaseTimeMs);
  counters_.updateCount(statPrefix + "rebalancer.latency.release_avg_ms",
                        stats.rebalancerStats.avgReleaseTimeMs);

  counters_.updateCount(statPrefix + "rebalancer.latency.pick_last_ms",
                        stats.rebalancerStats.lastPickTimeMs);
  counters_.updateCount(statPrefix + "rebalancer.latency.pick_avg_ms",
                        stats.rebalancerStats.avgPickTimeMs);

  const auto slabReleaseStats = getSlabReleaseStats();
  counters_.updateDelta(statPrefix + "slabs.rebalancer_runs",
                        slabReleaseStats.numSlabReleaseForRebalanceAttempts);
  counters_.updateDelta(statPrefix + "slabs.resizer_runs",
                        slabReleaseStats.numSlabReleaseForResizeAttempts);
  counters_.updateDelta(statPrefix + "slabs.adviser_runs",
                        slabReleaseStats.numSlabReleaseForAdviseAttempts);
  counters_.updateDelta(statPrefix + "slabs.move_attempts",
                        slabReleaseStats.numMoveAttempts);
  counters_.updateDelta(statPrefix + "slabs.move_success",
                        slabReleaseStats.numMoveSuccesses);
  counters_.updateDelta(statPrefix + "slabs.eviction_attempts",
                        slabReleaseStats.numEvictionAttempts);
  counters_.updateDelta(statPrefix + "slabs.eviction_success",
                        slabReleaseStats.numEvictionSuccesses);
  counters_.updateCount(statPrefix + "slabs.release_stuck",
                        slabReleaseStats.numSlabReleaseStuck);

  counters_.updateDelta(statPrefix + "evictions.filtered",
                        stats.numEvictionsFiltered);
  counters_.updateDelta(statPrefix + "evictions.concurrent_fill_failure",
                        stats.numEvictionFailureFromConcurrentFill);
  counters_.updateDelta(statPrefix + "evictions.concurrent_access_failure",
                        stats.numEvictionFailureFromConcurrentAccess);
  counters_.updateDelta(statPrefix + "evictions.put_token_lock_failure",
                        stats.numEvictionFailureFromPutTokenLock);
  counters_.updateDelta(statPrefix + "evictions.remove_failure",
                        stats.numEvictionFailureFromAccessContainer);
  counters_.updateDelta(statPrefix + "evictions.moving_failure",
                        stats.numEvictionFailureFromMoving);
  counters_.updateDelta(statPrefix + "evictions.remove_parent_failure",
                        stats.numEvictionFailureFromParentAccessContainer);
  counters_.updateDelta(
      statPrefix + "evictions.remove_parent_failure_parent_unlinked",
      stats.numEvictionFailureFromParentUnlinked);
  counters_.updateDelta(
      statPrefix + "evictions.remove_parent_failure_parent_exclusive",
      stats.numEvictionFailureFromParentExclusive);
  counters_.updateDelta(statPrefix + "evictions.remove_parent_failure_ref_held",
                        stats.numEvictionFailureFromParentRefHeld);
  counters_.updateDelta(statPrefix + "evictions.moving_parent_failure",
                        stats.numEvictionFailureFromParentMoving);

  counters_.updateCount(statPrefix + "cache.instance_uptime",
                        stats.cacheInstanceUpTime);
  counters_.updateCount(statPrefix + "ram.uptime", stats.ramUpTime);
  counters_.updateCount(statPrefix + "nvm.uptime", stats.nvmUpTime);
  counters_.updateCount(statPrefix + "ram.new_cache", stats.isNewRamCache);
  // isNewNvmCache is always true when nvm is disabled, so gate on
  // nvmCacheEnabled to avoid falsely reporting a new cache.
  const bool isNewNvmCache = stats.nvmCacheEnabled && stats.isNewNvmCache;
  counters_.updateCount(statPrefix + "nvm.new_cache", isNewNvmCache);
  counters_.updateCount(statPrefix + "cache.new_cache",
                        stats.isNewRamCache || isNewNvmCache);

  counters_.updateCount(statPrefix + "nvm.enabled", stats.nvmCacheEnabled);

  if (stats.nvmCacheEnabled) {
    counters_.updateDelta(statPrefix + "nvm.alloc_attempts",
                          stats.numNvmAllocAttempts);
    counters_.updateDelta(statPrefix + "nvm.copy_out_offloaded",
                          stats.numNvmCopyOutOffloaded);
    counters_.updateDelta(statPrefix + "nvm.copy_out_offloaded_bytes",
                          stats.numNvmCopyOutOffloadedBytes);
    counters_.updateDelta(statPrefix + "nvm.copy_out_fallbacks",
                          stats.numNvmCopyOutFallbacks);
    counters_.updateDelta(statPrefix + "nvm.destructor_alloc",
                          stats.numNvmAllocForItemDestructor);
    counters_.updateDelta(statPrefix + "nvm.destructor_alloc_errors",
                          stats.numNvmItemDestructorAllocErrors);

    counters_.updateDelta(statPrefix + "nvm.gets", stats.numNvmGets);
    counters_.updateDelta(statPrefix + "nvm.gets.miss", stats.numNvmGetMiss);
    counters_.updateDelta(statPrefix + "nvm.gets.miss.errs",
                          stats.numNvmGetMissErrs);
    counters_.updateDelta(statPrefix + "nvm.gets.miss.inflight_remove",
                          stats.numNvmGetMissDueToInflightRemove);
    counters_.updateDelta(statPrefix + "nvm.gets.miss.fast",
                          stats.numNvmGetMissFast);
    counters_.updateDelta(statPrefix + "nvm.gets.miss.expired",
                          stats.numNvmGetMissExpired);
    counters_.updateDelta(statPrefix + "nvm.gets.coalesced",
                          stats.numNvmGetCoalesced);

    counters_.updateDelta(statPrefix + "nvm.puts", stats.numNvmPuts);
    counters_.updateDelta(statPrefix + "nvm.puts.clean",
                          stats.numNvmPutFromClean);
    counters_.updateDelta(statPrefix + "nvm.puts.errs", stats.numNvmPutErrs);
    counters_.updateDelta(statPrefix + "nvm.puts.aborted_on_tombstone",
                          stats.numNvmAbortedPutOnTombstone);
    counters_.updateDelta(statPrefix + "nvm.puts.aborted_on_inflightget",
                          stats.numNvmAbortedPutOnInflightGet);
    counters_.updateDelta(statPrefix + "nvm.puts.encode_failure",
                          stats.numNvmPutEncodeFailure);

    counters_.updateDelta(statPrefix + "nvm.evictions.clean",
                          stats.numNvmCleanEvict);
    counters_.updateDelta(statPrefix + "nvm.evictions.unclean",
                          stats.numNvmUncleanEvict);
    counters_.updateDelta(statPrefix + "nvm.evictions.clean_double_evict",
                          stats.numNvmCleanDoubleEvict);
    counters_.updateDelta(statPrefix + "nvm.evictions.expired",
                          stats.numNvmExpiredEvict);
    counters_.updateDelta(statPrefix + "nvm.evictions.filtered_on_compaction",
                          stats.numNvmCompactionFiltered);

    counters_.updateDelta(statPrefix + "nvm.deletes", stats.numNvmDeletes);
    counters_.updateDelta(statPrefix + "nvm.deletes.fast",
                          stats.numNvmSkippedDeletes);

    counters_.updateDelta(statPrefix + "nvm.rejects.clean",
                          stats.numNvmRejectsByClean);
    counters_.updateDelta(statPrefix + "nvm.rejects.expired",
                          stats.numNvmRejectsByExpiry);
    counters_.updateDelta(statPrefix + "nvm.rejects.ap",
                          stats.numNvmRejectsByAP);

    counters_.updateDelta(statPrefix + "nvm.encryption_errors",
                          stats.numNvmEncryptionErrors);
    counters_.updateDelta(statPrefix + "nvm.decryption_errors",
                          stats.numNvmDecryptionErrors);

    visitEstimates(uploadStatsNanoToMicro, stats.nvmLookupLatencyNs,
                   statPrefix + "nvm.lookup.latency_us");
    visitEstimates(uploadStatsNanoToMicro, stats.nvmInsertLatencyNs,
                   statPrefix + "nvm.insert.latency_us");
    visitEstimates(uploadStatsNanoToMicro, stats.nvmRemoveLatencyNs,
                   statPrefix + "nvm.remove.latency_us");
    visitEstimates(uploadStatsNanoToMicro, stats.nvmMakeBlobCbLatencyNs,
                   statPrefix + "nvm.make_blob_cb.latency_us");
    visitEstimates(uploadStatsNanoToMicro, stats.nvmMakeObjCbLatencyNs,
                   statPrefix + "nvm.make_obj_cb.latency_us");
    visitEstimates(uploadStats, stats.nvmPutSize,
                   statPrefix + "nvm.incoming_item_size_bytes");
    visitEstimates(uploadStats, stats.nvmHitTTASecs,
                   statPrefix + "nvm.hit_tta_secs");

    if (stats.numNvmDestructorRefcountOverflow > 0) {
      counters_.updateCount(statPrefix + "nvm.destructors.refcount_overflow",
                            stats.numNvmDestructorRefcountOverflow);
    }
  }

  counters_.updateCount(statPrefix + "items.total", stats.numItems);
  counters_.updateCount(statPrefix + "items.chained_child",
                        stats.numChainedChildItems);
  counters_.updateCount(statPrefix + "items.chained_parent",
                        stats.numChainedParentItems);
  counters_.updateCount(statPrefix + "items.active_handles",
                        stats.numActiveHandles);

  visitEstimates(uploadStatsNanoToMicro, stats.allocateLatencyNs,
                 statPrefix + "allocate.latency_us");
  visitEstimates(uploadStatsNanoToMicro, stats.moveChainedLatencyNs,
                 statPrefix + "move.chained.latency_us");
  visitEstimates(uploadStatsNanoToMicro, stats.moveRegularLatencyNs,
                 statPrefix + "move.regular.latency_us");

  const auto cacheHitRate = calculateCacheHitRate(statPrefix);
  counters_.updateCount(statPrefix + "cache.hit_rate",
                        util::narrow_cast<uint64_t>(cacheHitRate.overall));
  counters_.updateCount(statPrefix + "ram.hit_rate",
                        util::narrow_cast<uint64_t>(cacheHitRate.ram));
  counters_.updateCount(statPrefix + "nvm.hit_rate",
                        util::narrow_cast<uint64_t>(cacheHitRate.nvm));

  counters_.updateDelta(statPrefix + "cache.handle_wait_block",
                        stats.numHandleWaitBlocks);

  counters_.updateDelta(statPrefix + "cache.expensive_stats_export",
                        stats.numExpensiveStatsPolled);
}

CacheBase::CacheHitRate CacheBase::calculateCacheHitRate(
    const std::string& statPrefix) const {
  const uint64_t nvmGetsDiff = counters_.getDelta(statPrefix + "nvm.gets");
  const uint64_t nvmGetMissDiff =
      counters_.getDelta(statPrefix + "nvm.gets.miss");
  const uint64_t cacheGetsDiff = counters_.getDelta(statPrefix + "cache.gets");
  const uint64_t cacheGetMissDiff =
      counters_.getDelta(statPrefix + "cache.gets.miss");
  uint64_t misses;
  if (nvmGetsDiff > 0) {
    // With NvmCache, the true misses are:
    //  (Dram Misses - NvmCache Get Hits)
    // This is because not every DRAM lookup will result in a lookup in
    // NvmCache.

    // T110860736: if somehow we have more nvm hits than DRAM misses, just treat
    // is as 0 cache miss. It can happen since stats collection is racy.
    if (cacheGetMissDiff >= nvmGetsDiff - nvmGetMissDiff) {
      misses = cacheGetMissDiff - (nvmGetsDiff - nvmGetMissDiff);
    } else {
      misses = 0;
    }
  } else {
    misses = cacheGetMissDiff;
  }

  const double overall = util::hitRatioCalc(cacheGetsDiff, misses);
  const double ram = util::hitRatioCalc(cacheGetsDiff, cacheGetMissDiff);
  const double nvm = util::hitRatioCalc(nvmGetsDiff, nvmGetMissDiff);
  return {overall, ram, nvm};
}

void CacheBase::updateIndividualPoolStats(const std::string& statPrefix) const {
  for (const auto pid : getRegularPoolIds()) {
    updatePoolStats(statPrefix, pid);
  }
}

void CacheBase::exportStats(
    const std::string& statPrefix,
    std::chrono::seconds aggregationInterval,
    std::function<void(folly::StringPiece, uint64_t)> cb) const {
  updateGlobalCacheStats(statPrefix);
  updateNvmCacheStats(statPrefix);
  updateLegacyEventTrackerStats(statPrefix);
  updateEventTrackerStats(statPrefix);

  if (aggregatePoolStats_ && canAggregatePoolStats()) {
    updateAggregatedPoolStats(statPrefix);
  } else {
    // Log warning when aggregation is enabled but not possible
    if (aggregatePoolStats_) {
      XLOG(WARN) << "Pool stats aggregation is enabled but cannot be performed "
                    "due to too many allocation classes";
    }
    updateIndividualPoolStats(statPrefix);
  }

  for (const auto pid : getCCachePoolIds()) {
    try {
      const auto& c = getCompactCache(pid);
      updateCompactCacheStats(statPrefix, c);
    } catch (const std::invalid_argument&) {
      // ignore compact caches that are not attached.
    }
  }

  updateObjectCacheStats(statPrefix);

  return counters_.exportStats(aggregationInterval, cb);
}

bool CacheBase::canAggregatePoolStats() const {
  const auto poolIds = getRegularPoolIds();
  XDCHECK(!poolIds.empty(), "Regular pool IDs should not be empty");

  // If we only have one pool, there is nothing to aggregate
  if (poolIds.size() < 2) {
    return false;
  }

  // Collect all unique allocation sizes from all pools
  std::unordered_set<uint32_t> allAllocSizes;
  for (const auto pid : poolIds) {
    const auto& pool = getPool(pid);
    for (const auto& allocSize : pool.getAllocSizes()) {
      allAllocSizes.insert(allocSize);
    }
  }
  return allAllocSizes.size() <= MemoryAllocator::kMaxClasses;
}

void CacheBase::updateAggregatedPoolStats(const std::string& statPrefix) const {
  const auto poolIds = getRegularPoolIds();
  XDCHECK(!poolIds.empty(), "Regular pool IDs should not be empty");
  // Get the first pool stats to initialize the aggregated stats
  auto poolIdsIter = poolIds.begin();
  PoolStats aggregatedStats = getPoolStats(*poolIdsIter);
  ++poolIdsIter;

  // Aggregate all remaining pool stats
  for (; poolIdsIter != poolIds.end(); ++poolIdsIter) {
    const PoolStats stats = getPoolStats(*poolIdsIter);
    aggregatedStats += stats;
  }
  aggregatedStats.poolName = "aggregated";
  updatePoolStats(statPrefix, aggregatedStats);
}

void CacheBase::setEventTracker(EventTracker::Config&& config) {
  eventTracker_ = std::make_unique<EventTracker>(std::move(config));
}
} // namespace facebook::cachelib
