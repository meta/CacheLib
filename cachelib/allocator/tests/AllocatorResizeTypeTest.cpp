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

#include "cachelib/allocator/tests/AllocatorResizeTest.h"
#include "cachelib/allocator/tests/TestBase.h"

namespace facebook {
namespace cachelib {
namespace tests {

TYPED_TEST_CASE(AllocatorResizeTest, AllocatorTypes);

TYPED_TEST(AllocatorResizeTest, ShrinkWithFreeMem) {
  this->testShrinkWithFreeMem();
}

TYPED_TEST(AllocatorResizeTest, GrowWithFreeMem) {
  this->testGrowWithFreeMem();
}

TYPED_TEST(AllocatorResizeTest, BasicResize) { this->testBasicResize(); }
TYPED_TEST(AllocatorResizeTest, ResizeWithFreeSlabs) {
  this->testResizingWithFreeSlabs();
}

TYPED_TEST(AllocatorResizeTest, BasicResizeWithSharedMem) {
  this->testBasicResizeWithSharedMem();
}

TYPED_TEST(AllocatorResizeTest, ResizeMemMonitor) {
  this->testResizeMemMonitor();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorNoResize) {
  this->testMemMonitorNoResize();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorCompactCache) {
  this->testMemMonitorCompactCache();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorEmptySlabs) {
  this->testMemMonitorEmptySlabs();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorAdvisedCountsMatchReality) {
  this->testMemMonitorAdvisedCountsMatchReality();
}

TYPED_TEST(AllocatorResizeTest, MemoryAdviseWithSaveRestore) {
  this->testMemoryAdviseWithSaveRestore();
}

TYPED_TEST(AllocatorResizeTest, MemoryMonitorPerIterationAdviseReclaim) {
  this->testMemoryMonitorPerIterationAdviseReclaim();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorAdvisesAwayOverLimit) {
  this->testMemMonitorAdvisesAwayOverLimit();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorAdvisesAwayOverLimit2) {
  this->testMemMonitorAdvisesAwayOverLimit2();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorAdvisesAwayOverLimit3) {
  this->testMemMonitorAdvisesAwayOverLimit3();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorAdvisesAwayAllCacheBug) {
  this->testMemMonitorAdvisesAwayAllCacheBug();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorAdvisesTimeout) {
  this->testMemMonitorAdvisesTimeout();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorInvalidPoolIdsOnStartup) {
  this->testMemMonitorInvalidPoolIdsOnStartup();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorInvalidPoolIdsAtRuntime) {
  this->testMemMonitorInvalidPoolIdsAtRuntime();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorPoolSubsetAdviseReclaim) {
  this->testMemMonitorPoolSubsetAdviseReclaim();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorPoolSubsetMaxAdvise) {
  this->testMemMonitorPoolSubsetMaxAdvise();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorPoolSubsetWarmRestart) {
  this->testMemMonitorPoolSubsetWarmRestart();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorPoolSubsetWarmRestartDelayedWorkers) {
  this->testMemMonitorPoolSubsetWarmRestart(true);
}

TYPED_TEST(AllocatorResizeTest, MemMonitorDisabledWarmRestart) {
  this->testMemMonitorDisabledWarmRestart(false);
}

TYPED_TEST(AllocatorResizeTest, MemMonitorZeroIntervalWarmRestart) {
  this->testMemMonitorDisabledWarmRestart(true);
}

TYPED_TEST(AllocatorResizeTest, MemMonitorPoolSubsetRollbackAttach) {
  this->testMemMonitorPoolSubsetRollbackAttach();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorRestartWhileReadingStats) {
  this->testMemMonitorRestartWhileReadingStats();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorReplaceRunningMonitor) {
  this->testMemMonitorReplaceRunningMonitor();
}

TYPED_TEST(AllocatorResizeTest, MemMonitorRestartWithDifferentPools) {
  this->testMemMonitorReplaceRunningMonitor(true);
}

TYPED_TEST(AllocatorResizeTest, MemMonitorDisableAtRuntime) {
  this->testMemMonitorDisableAtRuntime(false);
}

TYPED_TEST(AllocatorResizeTest, MemMonitorZeroIntervalAtRuntime) {
  this->testMemMonitorDisableAtRuntime(true);
}

TYPED_TEST(AllocatorResizeTest, ShrinkGrowthAdviseRaceCondition) {
  this->testShrinkGrowthAdviseRaceCondition();
}

TYPED_TEST(AllocatorResizeTest, PoolResizerWithSlabReleaseTimeouts) {
  this->testPoolResizerWithSlabReleaseTimeouts();
}

} // end of namespace tests
} // end of namespace cachelib
} // end of namespace facebook
