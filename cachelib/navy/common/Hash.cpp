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

#include "cachelib/navy/common/Hash.h"

#include <folly/hash/Checksum.h>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace facebook::cachelib::navy {
uint64_t hashBuffer(BufferView key, uint64_t seed) {
  return folly::hash::SpookyHashV2::Hash64(key.data(), key.size(), seed);
}

namespace {
#if defined(__x86_64__)
// Hardware CRC-32C via the SSE4.2 crc32 instruction, compiled with a target
// attribute and dispatched at runtime, so it is used even when folly (whose
// crc32c_hw is gated on compile-time SSE4.2 flags) was built without SIMD
// flags and would silently fall back to a ~20x slower table implementation.
__attribute__((target("sse4.2"))) uint32_t crc32cHardware(const uint8_t* data,
                                                          size_t n,
                                                          uint32_t crc) {
  while (n >= 8) {
    uint64_t v;
    memcpy(&v, data, 8);
    crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v));
    data += 8;
    n -= 8;
  }
  if (n >= 4) {
    uint32_t v;
    memcpy(&v, data, 4);
    crc = _mm_crc32_u32(crc, v);
    data += 4;
    n -= 4;
  }
  while (n > 0) {
    crc = _mm_crc32_u8(crc, *data++);
    n--;
  }
  return crc;
}

bool hasSse42() { return __builtin_cpu_supports("sse4.2"); }
#endif
} // namespace

uint32_t checksum(BufferView data, uint32_t startingChecksum) {
  // CRC-32C (Castagnoli) with raw seed semantics (default 0, no final
  // inversion). This matches the value produced by chained SSE4.2
  // _mm_crc32_* instructions and by Intel DSA's CRC generation as used by
  // the DTO library, so checksums can be offloaded to DSA and verified on
  // CPU (and vice versa) interchangeably.
#if defined(__x86_64__)
  static const bool kUseHw = hasSse42();
  if (kUseHw) {
    return crc32cHardware(data.data(), data.size(), startingChecksum);
  }
#endif
  return folly::crc32c(data.data(), data.size(), startingChecksum);
}
} // namespace facebook::cachelib::navy
