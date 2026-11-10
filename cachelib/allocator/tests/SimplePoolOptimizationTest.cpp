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

#include "cachelib/allocator/tests/SimplePoolOptimizationTest.h"

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/allocator/tests/TestBase.h"

namespace facebook {
namespace cachelib {
namespace tests {

TYPED_TEST_CASE(SimplePoolOptimizationTest, AllocatorTypes);

TYPED_TEST(SimplePoolOptimizationTest, PoolOptimizerBasic) {
  this->testPoolOptimizerBasic();
}

TYPED_TEST(SimplePoolOptimizationTest, StopWorkersStopsPoolOptimizer) {
  this->testStopWorkersStopsPoolOptimizer();
}

} // namespace tests
} // namespace cachelib
} // namespace facebook
