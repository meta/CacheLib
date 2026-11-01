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

#include <gtest/gtest.h>

#include "cachelib/navy/common/Hash.h"

namespace facebook::cachelib::navy::tests {
// navy::checksum must compute raw CRC-32C (Castagnoli, reflected poly
// 0x82F63B78), seed 0, no final inversion. This is the value produced by
// chained SSE4.2 _mm_crc32_* instructions and by Intel DSA CRC generation
// (as used by the DTO library). If this test fails, DSA-offloaded checksums
// would not verify against CPU-computed ones (and vice versa), and on-disk
// format versions must be revisited.
TEST(Hash, ChecksumIsRawCrc32c) {
  auto cs = [](folly::StringPiece s, uint32_t seed = 0) {
    return checksum(
        BufferView{s.size(), reinterpret_cast<const uint8_t*>(s.data())},
        seed);
  };
  EXPECT_EQ(0x00000000u, cs(""));
  EXPECT_EQ(0x93AD1061u, cs("a"));
  EXPECT_EQ(0x58E3FA20u, cs("123456789"));
  EXPECT_EQ(0xB3FEE25Eu, cs("The quick brown fox jumps over the lazy dog"));

  uint8_t bytes[256];
  for (size_t i = 0; i < sizeof(bytes); i++) {
    bytes[i] = static_cast<uint8_t>(i);
  }
  EXPECT_EQ(0x2436A9DBu, checksum(BufferView{sizeof(bytes), bytes}));

  // Chaining with a starting checksum must equal a single-shot computation.
  folly::StringPiece full{"123456789"};
  auto part1 = cs("1234");
  EXPECT_EQ(cs(full), cs("56789", part1));
}

TEST(Hash, HashedKeyCollision) {
  HashedKey hk1{"key 1"};
  HashedKey hk2{"key 2"};

  // Simulate a case where hash matches but key doesn't.
  HashedKey hk3 = HashedKey::precomputed(hk2.key(), hk1.keyHash());

  EXPECT_NE(hk1, hk2);
  EXPECT_NE(hk1, hk3);
  EXPECT_NE(hk2, hk3);
}
} // namespace facebook::cachelib::navy::tests
