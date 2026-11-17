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

#include "cachelib/allocator/MemoryMonitor.h"

#include <fmt/core.h>
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>

#include <algorithm>
#include <vector>

#include "cachelib/allocator/PoolResizeStrategy.h"
#include "cachelib/common/Exceptions.h"

namespace facebook::cachelib {

constexpr size_t kGBytes = 1024 * 1024 * 1024;

MemoryMonitor::MemoryMonitor(CacheBase& cache,
                             const Config& config,
                             std::shared_ptr<RebalanceStrategy> strategy,
                             uint64_t initialNumSlabsToAdvise)
    : cache_(cache),
      mode_(config.mode),
      strategy_(std::move(strategy)),
      poolIds_(config.poolIds),
      enableEmptyPoolRecovery_(config.enableEmptyPoolRecovery),
      numSlabsToAdvise_(initialNumSlabsToAdvise),
      percentAdvisePerIteration_(config.maxAdvisePercentPerIter),
      percentReclaimPerIteration_(config.maxReclaimPercentPerIter),
      lowerLimit_(config.lowerLimitGB * kGBytes),
      upperLimit_(config.upperLimitGB * kGBytes),
      maxLimitPercent_(config.maxAdvisePercent),
      reclaimRateLimitWindowSecs_(config.reclaimRateLimitWindowSecs),
      rateLimiter_(
          // Detect rate of decrease in free memory and
          // rate of increase in resident memory mode
          config.mode == FreeMemory ? false : true) {
  if (!strategy_) {
    strategy_ = std::make_shared<PoolResizeStrategy>();
  }
  // There should be at least a slab worth of difference between upper
  // and lower memory limits.
  XDCHECK_LT(lowerLimit_, upperLimit_ - Slab::kSize);
  validatePoolIds(cache_, poolIds_);
}

MemoryMonitor::~MemoryMonitor() {
  try {
    stop();
  } catch (const std::exception&) {
  }
}

void MemoryMonitor::work() {
  // Poll interval can change. Keep rate limiter window size updated.
  const auto reclaimWindowMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          reclaimRateLimitWindowSecs_)
          .count();
  const auto intervalMs = getInterval().count();
  XDCHECK_GT(intervalMs, 0);
  const auto windowSize =
      reclaimWindowMs == 0 ? 0
                           : std::max<int64_t>(1, reclaimWindowMs / intervalMs);
  rateLimiter_.setWindowSize(static_cast<size_t>(windowSize));
  switch (mode_) {
  case FreeMemory:
    checkFreeMemory();
    break;
  case ResidentMemory:
    checkResidentMemory();
    break;
  case TestMode:
    checkPoolsAndAdviseReclaim();
    break;
  default:
    throw std::runtime_error("Unsupported memory monitoring mode");
  }
}

void MemoryMonitor::updateNumSlabsToAdvise(int32_t numSlabs) {
  auto curNumSlabsToAdvise = numSlabsToAdvise_.load(std::memory_order_acquire);
  size_t newNumSlabsToAdvise;
  do {
    newNumSlabsToAdvise = curNumSlabsToAdvise + numSlabs;
    if (numSlabs < 0 &&
        static_cast<uint64_t>(-numSlabs) > curNumSlabsToAdvise) {
      throw std::invalid_argument(
          fmt::format("Invalid numSlabs {} to update numSlabsToAdvise {}",
                      numSlabs, curNumSlabsToAdvise));
    }
  } while (!numSlabsToAdvise_.compare_exchange_weak(
      curNumSlabsToAdvise, newNumSlabsToAdvise, std::memory_order_acq_rel));
}

void MemoryMonitor::checkFreeMemory() {
  auto memFree = util::getMemAvailable();
  memAvailableSize_ = memFree;
  rateLimiter_.addValue(memFree);
  if (memFree < lowerLimit_) {
    XLOGF(DBG,
          "Free memory size of {} bytes is below the limit of {} bytes",
          memFree,
          lowerLimit_);
    adviseAwaySlabs();
  } else if (memFree > upperLimit_ &&
             (enableEmptyPoolRecovery_ || getMonitoredSlabsAdvised() > 0)) {
    XLOGF(DBG,
          "Free memory size of {} bytes is above the limit of {} bytes",
          memFree,
          upperLimit_);
    reclaimSlabs();
    if (enableEmptyPoolRecovery_) {
      return;
    }
  }
  checkPoolsAndAdviseReclaim();
}

void MemoryMonitor::checkResidentMemory() {
  auto rss = util::getRSSBytes();
  memRssSize_ = rss;
  if (enableEmptyPoolRecovery_ && rss == 0) {
    return;
  }
  rateLimiter_.addValue(rss);
  if (rss > upperLimit_) {
    XLOGF(DBG,
          "Resident memory size of {} bytes is above the limit of {} bytes",
          rss,
          upperLimit_);
    adviseAwaySlabs();
  } else if (rss < lowerLimit_ &&
             (enableEmptyPoolRecovery_ || getMonitoredSlabsAdvised() > 0)) {
    XLOGF(DBG,
          "Resident memory size of {} bytes is below the limit of {} bytes",
          rss,
          lowerLimit_);
    reclaimSlabs();
    if (enableEmptyPoolRecovery_) {
      return;
    }
  }
  checkPoolsAndAdviseReclaim();
}

namespace {
size_t bytesToSlabs(size_t bytes) { return bytes / Slab::kSize; }
} // namespace

size_t MemoryMonitor::getPoolUsedSlabs(PoolId poolId) const noexcept {
  return bytesToSlabs(cache_.getPool(poolId).getCurrentUsedSize());
}

size_t MemoryMonitor::getPoolSlabs(PoolId poolId) const noexcept {
  return bytesToSlabs(cache_.getPool(poolId).getPoolUsableSize());
}

void MemoryMonitor::validatePoolIds(const CacheBase& cache,
                                    const std::set<PoolId>& poolIds) {
  const auto regularPoolIds = cache.getRegularPoolIds();
  for (const auto pid : poolIds) {
    if (!regularPoolIds.contains(pid)) {
      throw std::invalid_argument(fmt::format(
          "Memory monitor pool id {} is not a regular pool of the cache",
          static_cast<int>(pid)));
    }
  }
}

std::set<PoolId> MemoryMonitor::getMonitoredPoolIds() const {
  return poolIds_.empty() ? cache_.getRegularPoolIds() : poolIds_;
}

size_t MemoryMonitor::getMonitoredSlabsAdvised() const {
  return cache_.getNumSlabsAdvised(getMonitoredPoolIds());
}

size_t MemoryMonitor::getTotalSlabs() const noexcept {
  const auto pools = getMonitoredPoolIds();
  return std::accumulate(pools.begin(), pools.end(), 0ull,
                         [this](auto total, const auto& poolId) {
                           return total + getPoolSlabs(poolId);
                         });
}

size_t MemoryMonitor::getSlabsInUse() const noexcept {
  const auto pools = getMonitoredPoolIds();
  return std::accumulate(pools.begin(), pools.end(), 0ull,
                         [this](auto total, const auto& poolId) {
                           return total + getPoolUsedSlabs(poolId);
                         });
}

void MemoryMonitor::checkPoolsAndAdviseReclaim() {
  auto results = cache_.calcNumSlabsToAdviseReclaim(
      numSlabsToAdvise_.load(std::memory_order_acquire), getMonitoredPoolIds());
  if (results.poolAdviseReclaimMap.empty()) {
    return;
  }
  // all result would either be advise or reclaim. It is not possible for
  // some of them to be advise and some to reclaim

  // Advise slabs, if marked for advise
  if (results.advise) {
    for (auto& result : results.poolAdviseReclaimMap) {
      uint64_t slabsAdvised = 0;
      PoolId poolId = result.first;
      uint64_t slabsToAdvise = result.second;
      while (slabsAdvised < slabsToAdvise) {
        const auto classId = strategy_->pickVictimForResizing(cache_, poolId);
        if (classId == Slab::kInvalidClassId) {
          break;
        }
        try {
          const auto now = util::getCurrentTimeMs();
          auto stats = cache_.getPoolStats(poolId);
          cache_.releaseSlab(poolId, classId, SlabReleaseMode::kAdvise);
          ++slabsAdvised;
          const auto elapsed_time =
              static_cast<uint64_t>(util::getCurrentTimeMs() - now);
          // Log the event about the Pool which released the Slab along with
          // the number of slabs.
          stats_.addSlabReleaseEvent(
              classId, Slab::kInvalidClassId, /* No Class info */
              elapsed_time, poolId, stats.numSlabsForClass(classId),
              0 /* receiver slabs */, stats.allocSizeForClass(classId),
              0 /* receiver alloc size */, stats.evictionAgeForClass(classId),
              0 /* receiver eviction age */,
              stats.numFreeAllocsForClass(classId));

        } catch (const exception::SlabReleaseAborted& e) {
          cache_.incrementAbortedSlabReleases();
          // Check if this is due to shutdown or timeout
          if (cache_.isFastShutdownTriggered()) {
            XLOGF(WARN,
                  "Shutdown in progress, aborting slab advise from pool {} for"
                  " allocation class {}. Error: {}",
                  static_cast<int>(poolId), static_cast<int>(classId),
                  e.what());
            return;
          }
          // It's a timeout, log and continue with next slab
          XLOGF(WARN,
                "Timeout while advising slab from pool {} for allocation class"
                " {}. Continuing with remaining slabs. Error: {}",
                static_cast<int>(poolId), static_cast<int>(classId), e.what());
        } catch (const std::exception& e) {
          XLOGF(
              CRITICAL,
              "Error trying to advise away a slab from pool {} for allocation "
              "class {}. Error: {}",
              static_cast<int>(poolId), static_cast<int>(classId), e.what());
        }
      }
      XLOGF(DBG, "Advised away {} slabs from Pool ID: {}, to free {} bytes",
            slabsAdvised, static_cast<int>(poolId), slabsAdvised * Slab::kSize);
    }
    return;
  } else {
    // Reclaim slabs, if marked for reclaim
    for (auto& result : results.poolAdviseReclaimMap) {
      PoolId poolId = result.first;
      uint64_t slabsToReclaim = result.second;
      auto slabsReclaimed = cache_.reclaimSlabs(poolId, slabsToReclaim);
      XLOGF(
          DBG,
          "Reclaimed {} of {} slabs for Pool ID: {}, to grow cache by {} bytes",
          slabsReclaimed, slabsToReclaim, static_cast<int>(poolId),
          slabsReclaimed * Slab::kSize);
    }
  }
}

void MemoryMonitor::adviseAwaySlabs() {
  const auto totalSlabsInUse = getSlabsInUse();
  const auto totalSlabs = getTotalSlabs();

  if (totalSlabsInUse == 0 || totalSlabs == 0) {
    if (!poolIds_.empty()) {
      XLOG_EVERY_MS(WARN, 60000,
                    "The memory monitor pools have no slabs in use; nothing "
                    "can be advised away");
      return;
    }
    // If there are no used slabs and we're still having to advise away, then
    // the cache size is too big!
    XLOG(DBG, "There are no slabs in use to advise away");
    return;
  }
  const auto numAdvised = getMonitoredSlabsAdvised();
  // allSlabs represents the total number of slabs across all monitored pools
  const auto allSlabs = numAdvised + totalSlabs;

  // Check if we've already hit or exceeded the maximum limit
  const auto maxNumAdvisedSlabs = (maxLimitPercent_ * allSlabs / 100);
  // Advise percentAdvisePerIteration_% of upperLimit_ - lowerLimit_
  // every iteration
  auto slabsToAdvise = bytesToSlabs(upperLimit_ - lowerLimit_) *
                       percentAdvisePerIteration_ / 100;
  auto remainingSlabsAllowedToAdviseAway =
      numAdvised >= maxNumAdvisedSlabs ? 0 : maxNumAdvisedSlabs - numAdvised;
  // Clamp slabsToAdvise to stay within maxLimitPercent_
  slabsToAdvise = std::min(slabsToAdvise, remainingSlabsAllowedToAdviseAway);
  if (slabsToAdvise == 0) {
    // Only log CRITICAL if we hit the max limit, not if slabsToAdvise was
    // already 0 due to configuration (e.g., upperLimit_ == lowerLimit_)
    if (remainingSlabsAllowedToAdviseAway == 0) {
      XLOGF(CRITICAL,
            "Already advised {} slabs of {} ({}"
            "%) which is at or exceeds the maximum limit of {}"
            "%. Disabling advising which may result in an OOM.",
            numAdvised, allSlabs, numAdvised * 100 / allSlabs,
            maxLimitPercent_);
    }
    return;
  }
  XLOGF(DBG, "Advising away {} slabs to free {} bytes", slabsToAdvise,
        slabsToAdvise * Slab::kSize);
  updateNumSlabsToAdvise(slabsToAdvise);
}

void MemoryMonitor::reclaimSlabs() {
  // Reclaim percentReclaimPerIteration_% of upperLimit_ - lowerLimit_
  // every iteration
  const auto reclaimBytes =
      (upperLimit_ - lowerLimit_) * percentReclaimPerIteration_ / 100;
  // Rate limit reclaimed memory if free memory is dropping or rss is rising
  // to prevent OOM
  const auto rateLimitedReclaimBytes = rateLimiter_.throttle(reclaimBytes);
  if (reclaimBytes > rateLimitedReclaimBytes) {
    XLOGF(DBG, "Rate limiting reclaim down from {} bytes to {} bytes",
          reclaimBytes, rateLimitedReclaimBytes);
  }
  auto slabsToReclaim = bytesToSlabs(rateLimitedReclaimBytes);
  const auto numAdvised = getMonitoredSlabsAdvised();
  if (slabsToReclaim > numAdvised) {
    slabsToReclaim = numAdvised;
  }
  if (enableEmptyPoolRecovery_) {
    reclaimObservedSlabs(slabsToReclaim, numAdvised);
    return;
  }
  if (slabsToReclaim == 0) {
    return;
  }

  const auto totalSlabsInUse = getSlabsInUse();
  if (totalSlabsInUse == 0) {
    XLOG(CRITICAL, "There are no slabs in use by items cache, cannot reclaim");
    return;
  }
  XLOGF(DBG, "Reclaiming {} slabs to increase cache size by {} bytes",
        slabsToReclaim, slabsToReclaim * Slab::kSize);
  updateNumSlabsToAdvise(-static_cast<int32_t>(slabsToReclaim));
}

void MemoryMonitor::reclaimObservedSlabs(size_t slabsToReclaim,
                                         size_t numAdvised) {
  const auto poolIds = getMonitoredPoolIds();
  // A retry needs a fresh low-pressure decision and rate-limit allowance, not
  // outstanding target debt that could run in the deadband or under pressure.
  SCOPE_EXIT {
    numSlabsToAdvise_.store(cache_.getNumSlabsAdvised(poolIds),
                            std::memory_order_release);
  };
  if (slabsToReclaim == 0) {
    return;
  }

  if (getSlabsInUse() != 0) {
    const auto results = cache_.calcNumSlabsToAdviseReclaim(
        numAdvised - slabsToReclaim, poolIds);
    if (results.advise) {
      return;
    }
    for (const auto& [poolId, count] : results.poolAdviseReclaimMap) {
      if (slabsToReclaim == 0) {
        break;
      }
      const auto requested = std::min<size_t>(count, slabsToReclaim);
      const auto reclaimed = cache_.reclaimSlabs(poolId, requested);
      XDCHECK_LE(reclaimed, requested);
      slabsToReclaim -= reclaimed;
    }
    return;
  }

  // In-use weights are undefined for an empty cache. Give every advised pool
  // a share, rotating the first pool when the allowance is smaller than the
  // number of pools.
  std::vector<std::pair<PoolId, size_t>> pools;
  for (const auto poolId : poolIds) {
    const auto advised = cache_.getPool(poolId).getNumSlabsAdvised();
    if (advised != 0) {
      pools.emplace_back(poolId, advised);
    }
  }
  const auto first =
      std::find_if(pools.begin(), pools.end(), [this](auto pool) {
        return static_cast<size_t>(pool.first) >= nextRecoveryPool_;
      });
  std::rotate(pools.begin(), first, pools.end());
  auto poolsLeft = pools.size();
  for (const auto& [poolId, advised] : pools) {
    if (slabsToReclaim == 0) {
      break;
    }
    const auto share =
        slabsToReclaim / poolsLeft + (slabsToReclaim % poolsLeft != 0);
    --poolsLeft;
    const auto requested = std::min(advised, share);
    const auto reclaimed = cache_.reclaimSlabs(poolId, requested);
    XDCHECK_LE(reclaimed, requested);
    slabsToReclaim -= reclaimed;
    nextRecoveryPool_ = static_cast<size_t>(poolId) + 1;
  }
}

RateLimiter::RateLimiter(bool detectIncrease)
    : detectIncrease_(detectIncrease) {}

void RateLimiter::addValue(int64_t value) {
  if (windowSize_ < 2) {
    // Window size not large enough to calculate rate of change.
    // This effectively disables rate limiting.
    return;
  }
  values_.push_back(value);
  auto prevValue = values_.front();
  // We may remove multiple values if window size shrinks
  while (values_.size() > windowSize_) {
    values_.pop_front();
  }
  if (detectIncrease_) {
    rateOfChange_ = (value - prevValue) / static_cast<int64_t>(windowSize_);
  } else {
    rateOfChange_ = (prevValue - value) / static_cast<int64_t>(windowSize_);
  }
}

size_t RateLimiter::throttle(int64_t delta) {
  if (rateOfChange_ < 0 || windowSize_ < 2) {
    return delta; // No throttling
  }
  // Fully throttled when we either have insufficient number of samples or
  // rate of change is faster than proposed delta change.
  if (values_.size() < windowSize_ || delta < rateOfChange_) {
    return 0;
  }
  // Throttle down delta by rate of change. The greater the rate of change, the
  // more the delta is throttled.
  return delta - rateOfChange_;
}

} // namespace facebook::cachelib
