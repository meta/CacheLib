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

#include <cstdint>

#include "cachelib/allocator/CacheStats.h"
#include "cachelib/allocator/memory/Slab.h"

namespace facebook {
namespace cachelib {

namespace detail {

// tracks the state of the pool from the last time we ran the pickVictim.
struct Info {
  // the class Id that we belong to. For computing deltas.
  ClassId id{Slab::kInvalidClassId};

  // number of slabs the pool's allocation class had.
  unsigned long long nSlabs{0};

  // our last record of evictions.
  unsigned long long evictions{0};

  // our last record of allocation failures
  unsigned long long allocFailures{0};

  // number of attempts remaining for hold off period when we acquire a slab.
  unsigned int holdOffRemaining{0};

  // number of hits for this allocation class in this pool
  uint64_t hits{0};

  // accumulative number of hits in the tail slab of this allocation class
  uint64_t accuTailHits{0};

  double decayedAccuTailHits{0.0};

  // accumalative number of requests seen for this allocation class
  uint64_t numRequests{0};

  uint64_t numRequestsAtLastDecay{0};

  // TODO(sugak) this is changed to unblock the LLVM upgrade The fix is not
  // completely understood, but it's a safe change T16521551 - Info() noexcept
  // = default;
  Info() = default;
  Info(ClassId _id,
       unsigned long long slabs,
       unsigned long long evicts,
       uint64_t h,
       uint64_t th,
       double dath,
       uint64_t nr,
       uint64_t nrld) noexcept
      : id(_id), nSlabs(slabs), evictions(evicts), hits(h), accuTailHits(th), decayedAccuTailHits(dath), numRequests(nr), numRequestsAtLastDecay(nrld) {}

  // number of rounds we hold off for when we acquire a slab.
  static constexpr unsigned int kNumHoldOffRounds = 10;

  // return the delta of slabs for this alloc class from the current state.
  //
  // @param poolStats   the current pool stats for this pool.
  // @return the delta of the number of slabs acquired.
  int64_t getDeltaSlabs(const PoolStats& poolStats) const {
    const auto& acStats = poolStats.mpStats.acStats;
    XDCHECK(acStats.find(id) != acStats.end());
    return static_cast<int64_t>(acStats.at(id).totalSlabs()) -
           static_cast<int64_t>(nSlabs);
  }

  // return the delta of evictions for this alloc class from the current
  // state.
  //
  // @param poolStats   the current pool stats for this pool.
  // @return the delta of the number of evictions
  int64_t getDeltaEvictions(const PoolStats& poolStats) const {
    const auto& cacheStats = poolStats.cacheStats;
    XDCHECK(cacheStats.find(id) != cacheStats.end());
    return static_cast<int64_t>(cacheStats.at(id).numEvictions()) -
           static_cast<int64_t>(evictions);
  }

  // return the delta of hits for this alloc class from the current state.
  //
  // @param poolStats   the current pool stats for this pool.
  // @return the delta of the number of hits
  uint64_t deltaHits(const PoolStats& poolStats) const {
    XDCHECK(poolStats.cacheStats.find(id) != poolStats.cacheStats.end());
    // When a thread goes out of scope, numHitsForClass will decrease. In this
    // case, we simply consider delta as 0.  TODO: change following if to
    // XDCHECK_GE(poolStats.numHitsForClass(id), hits) once all use cases
    // are using CacheStats::ThreadLocalStats
    if (poolStats.numHitsForClass(id) <= hits) {
      return 0;
    }

    return poolStats.numHitsForClass(id) - hits;
  }

  uint64_t deltaRequests(const PoolStats& poolStats) const {
    const auto& cacheStats = poolStats.cacheStats.at(id);
    auto totalRequests = poolStats.numHitsForClass(id) + cacheStats.allocAttempts;
    return totalRequests > numRequests
        ? totalRequests - numRequests
        : 0;
  }

  uint64_t deltaRequestsSinceLastDecay(const PoolStats& poolStats) const {
    const auto& cacheStats = poolStats.cacheStats.at(id);
    auto totalRequests = poolStats.numHitsForClass(id) + cacheStats.allocAttempts;
    return totalRequests > numRequestsAtLastDecay
        ? totalRequests - numRequestsAtLastDecay
        : 0;
  }

  // return the delta of alloc failures for this alloc class from the current
  // state.
  //
  // @param poolStats   the current pool stats for this pool.
  // @return the delta of allocation failures
  uint64_t deltaAllocFailures(const PoolStats& poolStats) const {
    XDCHECK(poolStats.cacheStats.find(id) != poolStats.cacheStats.end());
    const auto& c = poolStats.cacheStats.at(id);
    if (c.allocFailures <= allocFailures) {
      return 0;
    }
    return c.allocFailures - allocFailures;
  }

  // return the delta of hits per slab for this alloc class from the current
  // state.
  //
  // @param poolStats   the current pool stats for this pool.
  // @return the delta of the hits per slab
  uint64_t deltaHitsPerSlab(const PoolStats& poolStats) const {
    return deltaHits(poolStats) / poolStats.numSlabsForClass(id);
  }

  // return the delta of hits per slab for this alloc class from the current
  // state after removing one slab
  //
  // @param poolStats   the current pool stats for this pool.
  // @return the projected delta of the hits per slab, or UINT64_MAX if alloc
  // class only has 1 slab
  uint64_t projectedDeltaHitsPerSlab(const PoolStats& poolStats) const {
    const auto nSlab = poolStats.numSlabsForClass(id);
    return nSlab == 1 ? UINT64_MAX : deltaHits(poolStats) / (nSlab - 1);
  }

  // return the delta of hits in the tail slab for this allocation class
  //
  // @param poolStats  the current pool stats for this pool.
  // @return the marginal hits
  uint64_t getMarginalHits(const PoolStats& poolStats) const {
    return poolStats.cacheStats.at(id).containerStat.numTailAccesses -
           accuTailHits;
  }

  double getDecayedMarginalHits(const PoolStats& poolStats, double decayFactor=0.0) const {
    // decayed past + now
    return decayedAccuTailHits + getMarginalHits(poolStats) * (1 - decayFactor);
  }

  // returns true if the hold off is active for this alloc class.
  bool isOnHoldOff() const noexcept { return holdOffRemaining > 0; }

  // reduces the hold off by one.
  void reduceHoldOff() noexcept {
    XDCHECK(isOnHoldOff());
    --holdOffRemaining;
  }

  void resetHoldOff() noexcept { holdOffRemaining = 0; }

  // initializes the hold off.
  void startHoldOff() noexcept { holdOffRemaining = kNumHoldOffRounds; }

  void updateHits(const PoolStats& poolStats) noexcept {
    hits = poolStats.numHitsForClass(id);
  }

  void updateRequests(const PoolStats& poolStats) noexcept {
    const auto& cacheStats = poolStats.cacheStats.at(id);
    numRequests = poolStats.numHitsForClass(id) + cacheStats.allocAttempts;
  }

  void updateTailHits(const PoolStats& poolStats, double decayFactor=0.0) noexcept {
    const auto& cacheStats = poolStats.cacheStats.at(id);
    decayedAccuTailHits = (decayedAccuTailHits + getMarginalHits(poolStats)) * decayFactor;
    accuTailHits = cacheStats.containerStat.numTailAccesses;
    numRequestsAtLastDecay = poolStats.numHitsForClass(id) + cacheStats.allocAttempts;
  }


  // updates the current record to store the current state of slabs and the
  // evictions we see.
  void updateRecord(const PoolStats& poolStats) {
    // Update number of slabs
    const auto& acStats = poolStats.mpStats.acStats;
    XDCHECK(acStats.find(id) != acStats.end());
    nSlabs = acStats.at(id).totalSlabs();

    // Update evictions
    const auto& cacheStats = poolStats.cacheStats.at(id);
    evictions = cacheStats.numEvictions();

    // update tail hits
    accuTailHits = cacheStats.containerStat.numTailAccesses;

    allocFailures = cacheStats.allocFailures;
  }
};
} // namespace detail
} // namespace cachelib
} // namespace facebook
