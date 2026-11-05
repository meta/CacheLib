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

#include <dto.h>
#include <gtest/gtest.h>

namespace facebook::cachelib::navy::tests {
namespace {

template <typename Function>
void expectLinked(Function function) {
  // Keep a symbol reference even under LTO, without submitting work to DSA.
  Function volatile address = function;
  EXPECT_NE(nullptr, address);
}

TEST(DtoLink, ExplicitAsyncApi) {
  expectLinked(&dto_submit_memcpy);
  expectLinked(&dto_submit_crc);
  expectLinked(&dto_submit_memcpy_crc);
  expectLinked(&dto_async_poll);
  expectLinked(&dto_async_wait);
  expectLinked(&dto_async_crc_val);
  expectLinked(&dto_wait_blocks);
}

TEST(DtoLink, ExplicitBatchApi) {
  expectLinked(&dto_batch_create);
  expectLinked(&dto_batch_destroy);
  expectLinked(&dto_batch_reset);
  expectLinked(&dto_batch_add_crc);
  expectLinked(&dto_batch_add_memcpy_crc);
  expectLinked(&dto_batch_submit);
  expectLinked(&dto_batch_poll);
  expectLinked(&dto_batch_wait);
  expectLinked(&dto_batch_crc);
  expectLinked(&dto_batch_status);
  expectLinked(&dto_submit_batch_copy);
}

} // namespace
} // namespace facebook::cachelib::navy::tests
