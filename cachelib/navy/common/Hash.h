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

#include <cstdint>

#include "cachelib/common/Hash.h"
#include "cachelib/navy/common/Buffer.h"

namespace facebook {
namespace cachelib {
namespace navy {
// Default hash function
uint64_t hashBuffer(BufferView key, uint64_t seed = 0);

// Default checksumming function: CRC-32C (Castagnoli) with raw seed
// semantics (no final inversion), DSA-offload compatible.
uint32_t checksum(BufferView data, uint32_t startingChecksum = 0);

// Convenience utils to convert a piece of buffer to a hashed key
inline HashedKey makeHK(const void* ptr, size_t size) {
  return HashedKey{
      folly::StringPiece{reinterpret_cast<const char*>(ptr), size}};
}
inline HashedKey makeHK(BufferView key) {
  return makeHK(key.data(), key.size());
}
inline HashedKey makeHK(const Buffer& key) {
  BufferView view = key.view();
  return makeHK(view.data(), view.size());
}
inline HashedKey makeHK(const char* cstr) {
  return HashedKey{folly::StringPiece{cstr}};
}

} // namespace navy
} // namespace cachelib
} // namespace facebook
