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
#include <set>

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/RebalanceStrategy.h"
#include "cachelib/allocator/SlabReleaseStats.h"
#include "cachelib/common/PeriodicWorker.h"

namespace facebook {
namespace cachelib {

namespace tests {
template <typename AllocatorT>
class AllocatorResizeTest;
}

// A utility to track rate of increase/decrease of values over a window of time
// and use it to throttle specified value at the same rate. This means if the
// rate of change in values over the window exceeds the specified value, the
// rate limiter throttles it down to 0, otherwise the value is reduced
// proportionally and not throttled at all when rate of change drops to 0.
class RateLimiter {
 public:
  // @param detectIncrease  Detect rate of increase if true, decrease otherwise
  explicit RateLimiter(bool detectIncrease);

  // Update window size
  // @param windowSize      The window size in terms of the number of values to
  //                        track for rate limiting
  void setWindowSize(size_t windowSize) { windowSize_ = windowSize + 1; }

  // specify a new sample value
  // @param value Sample value
  void addValue(int64_t value);

  // throttle down the proposed change in value based on current rate of change
  // @param value Value to throttle
  // @return throttled value
  size_t throttle(int64_t delta);

 private:
  // List of values over the window for rate of change calculation
  std::list<int64_t> values_;

  // Whether to detect an increase or decrease
  bool detectIncrease_{true};

  // Number of values to track for the window
  size_t windowSize_{0};

  // Current rate of increase/decrease.
  int64_t rateOfChange_{0};
};

// The goal of memory monitoring is to avoid an out-of-memory (OOM) situation,
// either by ensuring there's enough free memory available on the system or
// that the caching process does not exceed a given memory usage limit.
// Note: For processes running inside cgroups with memory limits, the free
// memory monitoring does not work. Instead use the resident memory monitoring
// to keep the process memory usage below the cgroup memory limit.
class MemoryMonitor : public PeriodicWorker {
 public:
  enum Mode { FreeMemory, ResidentMemory, TestMode, Disabled };

  struct Config {
    // Memory monitoring mode. Enable memory monitoring by setting this to
    // MemoryMonitor::ResidentMemory or MemoryMonitor::FreeMemory mode.
    Mode mode{Mode::Disabled};
    // percentage of memUpperLimit - memLowerLimit to be advised away
    // in an iteration.
    size_t maxAdvisePercentPerIter{5};
    // percentage of memUpperLimit - memLowerLimit to be reclaimed for
    // cache in an iteration.
    size_t maxReclaimPercentPerIter{5};
    // lower limit for free/resident memory in GBs.
    // Note: the lower/upper limit is used in exactly opposite ways for the
    // FreeMemory versus ResidentMemory mode.
    // 1. In the ResidentMemory mode, when the resident memory usage drops
    // below this limit, advised away slabs are reclaimed in proportion to
    // the size of pools, to increase cache size and raise resident memory
    // above this limit.
    // 2. In the FreeMemory mode, when the system free memory drops below this
    // limit, slabs are advised away from pools in proportion to their size to
    // raise system free memory above this limit.
    size_t lowerLimitGB{10};
    // upper limit for free/resident memory in GBs.
    // Note: the lower/upper limit is used in exactly opposite ways for the
    // FreeMemory versus ResidentMemory mode.
    // 1. In the ResidentMemory mode, when the resident memory usage exceeds
    // this limit, slabs are advised away from pools in proportion to their
    // size to reduce resident memory usage below this limit.
    // 2. In the FreeMemory mode, when the system free memory exceeds
    // this limit and if there are slabs that were advised away earlier,
    // they're reclaimed by pools in proportion to their sizes to reduce the
    // system free memory below this limit.
    size_t upperLimitGB{15};
    // maximum percentage of item cache that can be advised away
    size_t maxAdvisePercent{20};
    // On restart, the heap usage for applications grows slowly to steady state
    // over time. Memory monitoring may reclaim advised memory leaving
    // application vulnerable to OOMs due rapid growth in heap usage.
    // Cachelib supports rate limiting reclaiming of advised memory to avoid OOM
    // This is enabled by setting tracking window size to a non-zero value.
    // Setting this config to a value > 0 enables rate limiting reclaiming of
    // advised memory by the amount by which free/resident memory is
    // decreasing/increasing
    std::chrono::seconds reclaimRateLimitWindowSecs{0};
    // Pools from which slabs are advised away and into which they are
    // reclaimed. Empty means all regular (non compact cache) pools. When set,
    // the advise/reclaim targets, the maxAdvisePercent cap and the advised
    // slab accounting only consider these pools.
    //
    // Every id must be a regular pool (as returned by addPool()) when the
    // monitor starts, otherwise starting it throws std::invalid_argument. See
    // CacheAllocatorConfig::enableMemoryMonitor() for setting it on a cold
    // start.
    //
    // poolIds is not persisted. startNewMemMonitor() reclaims slabs advised
    // away from pools outside the configured set before starting the monitor.
    // With monitoring disabled, calling startNewMemMonitor() reclaims all
    // advised slabs. Warm attach does not reclaim slabs until that call.
    std::set<PoolId> poolIds{};

    bool disabled() const { return mode == Mode::Disabled; }
  };

  // Memory monitoring can be setup to run in one of the two following modes:
  //
  // 1. Free Memory Monitoring
  //
  // Setup a free memory monitor that periodically checks system free memory.
  // If it dips below lowerLimitGB bytes, it advises percentAdvisePerIteration
  // percent of (upperLimitGB - lowerLimitGB) at a time (in every iteration),
  // until system free memory is above the lowerLimitGB bytes. If the system
  // free memory exceeds upperLimitGB bytes, it reclaims
  // percentReclaimPerIteration percent of (upperLimitGB - lowerLimitGB)
  // at a time until system free memory drops below upperLimitGB. A maximum of
  // maxLimitPercent of total cache size (excluding compact cache) can be
  // advised away, after which advising stops to avoid cache from becoming
  // too small.
  // Given N bytes of memory on a host, typically N-M bytes are used by cache
  // leaving M bytes for the heap usage by cache process, kernel and other
  // processes running on the box. When non-cache memory usage exceeds M bytes
  // the host goes Out-Of-Memory (OOM) and may fail or kill the cache process.
  // The free memory monitor ensures that there's at least lowerLimitGB amount
  // of memory free by giving up to maxLimitPercent of the cache (excluding
  // compact cache), there by avoiding OOM condition.
  //
  // @param mode                 FreeMemory
  // @param cache                Cache
  // @param percentAdvisePerIteration
  //                             Percentage of upperLimitGB-lowerLimitGB to be
  //                             advised every poll period. This
  //                             governs the rate of advise
  // @param percentReclaimPerIteration
  //                             Percentage of upperLimitGB-lowerLimitGB to be
  //                             reclaimed every poll period. This
  //                             governs the rate of reclaim
  // @param lowerLimitGB         The lower limit of free memory in GBytes that
  //                             triggers advising away of memory from cache
  // @param upperLimitGB         The upper limit of free memory in GBytes that
  //                             triggers reclaiming of advised away memory
  // @param maxLimitPercent      Maximum percentage of item cache limit that can
  //                             be advised away before advising is disabled
  //                             leading to a probable OOM.
  // @param strategy             Strategy to use to determine the allocation
  //                             class in pool to steal slabs from, for advising
  // @param reclaimRateLimitWindowSecs
  //                             Specifies window in seconds over which
  //                             free memory values are tracked to detect
  //                             decreasing free memory values. Setting this to
  //                             non-zero value enables rate limiting reclaim.
  //
  // 2. Resident Memory Monitoring
  //
  // Setup a resident memory monitor to advise away memory to avoid OOM, by
  // by limiting process's total resident memory usage. The resident memory
  // usage of the process can be split into two parts, the cache and everything
  // else. When the resident memory usage exceeds the upperLimitGB, the monitor
  // gives away memory from cache, percentAdvisePerIteration of
  // (upperLimitGB - lowerLimitGB) every poll period, until the memory usage
  // drops below upperLimitGB. When the resident memory usage dips below
  // lowerLimitGB, the monitor reclaims memory for cache (if previously given
  // away), until the resident memory usage is above the lowerLimitGB,
  // percentReclaimPerIteration of (upperLimitGB - lowerLimitGB) at a time.
  //
  // @param cache                    Cachelib instance
  // @param config                   Memory monitor config
  // @param strategy                 Strategy to use to determine the
  //                                 allocation class in pool to steal slabs
  //                                 from, for advising
  // @param initialNumSlabsToAdvise  initial number of slabs to advise away,
  //                                 usually calculated by memory pool manager
  MemoryMonitor(CacheBase& cache,
                const Config& config,
                std::shared_ptr<RebalanceStrategy> strategy,
                size_t initialNumSlabsToAdvise);

  ~MemoryMonitor() override;

  // maximum percentage of regular cache memory that can be advised away.
  size_t getMaxAdvisePct() const noexcept { return maxLimitPercent_; }

  // amount of memory available on the host
  size_t getMemAvailableSize() const noexcept { return memAvailableSize_; }

  // rss size of the process
  size_t getMemRssSize() const noexcept { return memRssSize_; }

  SlabReleaseEvents getSlabReleaseEvents(PoolId pid) const {
    return stats_.getSlabReleaseEvents(pid);
  }

  // @throw std::invalid_argument if poolIds (Config::poolIds) has an id that
  //        is not a regular pool of the cache
  static void validatePoolIds(const CacheBase& cache,
                              const std::set<PoolId>& poolIds);

 private:
  // updates the number of slabs to be advised by numSlabs. This would either
  // increment (to advise away more slabs) or decrement (to reclaim some of the
  // previously advised away slabs) numSlabsToAdvise_ by numSlabs.
  //
  // @param numSlabs   number of slabs to add-to/subtract-from numSlabToAdvise_
  //
  // @throw std::invalid_argument if numSlabs is negative and its absolute
  //                              value is more than numSlabsToAdvise_
  //                              (ie total slabs to be reclaimed cannot be
  //                               more than total slabs advised away)
  void updateNumSlabsToAdvise(int32_t numSlabs);

  // check free memory and advise/reclaim if necessary
  void checkFreeMemory();

  // check resident memory and advise/reclaim if necessary
  void checkResidentMemory();

  // check pools for memory to be advised or reclaimed and execute
  // Checks the target number of slabs to be advised and compares with
  // the currently advised away slabs. Slabs are advised away or reclaimed
  // to make these two numbers equal
  void checkPoolsAndAdviseReclaim();

  // @param poolId the pool id
  // @return number of slabs in use by pool
  size_t getPoolUsedSlabs(PoolId poolId) const noexcept;

  // @param poolId the pool id
  // @return number of slabs in based on pool size
  size_t getPoolSlabs(PoolId poolId) const noexcept;

  // @return number of slabs based on the size of all monitored pools added
  //         together
  size_t getTotalSlabs() const noexcept;

  // @return number of slabs in use by all monitored pools together
  size_t getSlabsInUse() const noexcept;

  // @return poolIds_, or all regular pools if poolIds_ is empty
  std::set<PoolId> getMonitoredPoolIds() const;

  // @return number of slabs currently advised away from the monitored pools
  size_t getMonitoredSlabsAdvised() const;

  // advise away slabs to increase free memory or reduce RSS
  void adviseAwaySlabs();

  // reclaim slabs to increase cache size and reduce free memory/increase RSS
  void reclaimSlabs();

  // cache's interface for rebalancing
  CacheBase& cache_;

  // Memory monitoring mode
  Mode mode_;

  // user-defined rebalance strategy that would be used to pick a victim. If
  // this does not work out, we pick the first allocation class that has
  // non-zero slabs.
  std::shared_ptr<RebalanceStrategy> strategy_;

  // slab release stats for memory monitor.
  ReleaseStats stats_{};

  // Pools selected by Config::poolIds. Empty means all regular pools.
  const std::set<PoolId> poolIds_;

  // Number of slabs to advise away. This is target number of slabs to be
  // advised across all monitored pools. Note that this needs to be std::atomic
  // since there can be a race condition where one worker is stopping and
  // another is starting.
  std::atomic_size_t numSlabsToAdvise_;

  // number of slabs released as a part of resizing pools.
  std::atomic<unsigned int> slabsReleased_{0};

  // Specifies the percentage of the advising bounds (upperlimit - lowerlimit)
  // that is advised away per iteration of memory monitor.
  size_t percentAdvisePerIteration_{0};

  // Specifies the percentage of the advising bounds (upperlimit - lowerlimit)
  // that is reclaimed per iteration of memory monitor.
  size_t percentReclaimPerIteration_{0};

  // lower limit for free/resident memory in GBs.
  // Note: the lower/upper limit is used in exactly opposite ways for the
  // FreeMemory versus ResidentMemory mode.
  // 1. In the ResidentMemory mode, when the resident memory usage drops
  // below this limit, advised away slabs are reclaimed in proportion to
  // the size of pools, to increase cache size and raise resident memory
  // above this limit.
  // 2. In the FreeMemory mode, when the system free memory drops below this
  // limit, slabs are advised away from pools in proportion to their size to
  // raise system free memory above this limit.
  size_t lowerLimit_{0};

  // upper limit for free/resident memory in GBs.
  // Note: the lower/upper limit is used in exactly opposite ways for the
  // FreeMemory versus ResidentMemory mode.
  // 1. In the ResidentMemory mode, when the resident memory usage exceeds
  // this limit, slabs are advised away from pools in proportion to their
  // size to reduce resident memory usage below this limit.
  // 2. In the FreeMemory mode, when the system free memory exceeds
  // this limit and if there are slabs that were advised away earlier,
  // they're reclaimed by pools in proportion to their sizes to reduce the
  // system free memory below this limit.
  size_t upperLimit_{0};

  // the maximum percentage of total memory that can be advised away
  size_t maxLimitPercent_{0};

  // Specifies window in seconds over which resident memory values are tracked
  // to detect increasing resident memory values. Setting this to non-zero
  // value enables rate limiting reclaim
  std::chrono::seconds reclaimRateLimitWindowSecs_;

  // On restart, the heap usage for applications grows slowly to steady state
  // over time. Memory monitoring may reclaim advised memory leaving application
  // vulnerable to OOMs due rapid growth in heap usage.
  // Cachelib supports rate limiting reclaiming of advised memory to avoid OOM
  // where the rate of reclaim is reduced by rate of free/resident memory
  // decrease/increase.
  RateLimiter rateLimiter_;

  template <typename AllocatorT>
  friend class facebook::cachelib::tests::AllocatorResizeTest;

  // amount of memory available on the host
  std::atomic<size_t> memAvailableSize_{0};
  // rss size of the process
  std::atomic<size_t> memRssSize_{0};

  // implements the actual logic of running tryRebalancing and
  // updating the stats
  void work() final;
};

} // namespace cachelib
} // namespace facebook
