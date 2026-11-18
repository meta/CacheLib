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

#include <memory>
#include <string>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <fmt/core.h>
#pragma GCC diagnostic pop

#include "cachelib/allocator/CacheDetails.h"
#include "cachelib/shm/ShmManager.h"

namespace facebook {
namespace cachelib {

// used as a read only into a shared cache. The cache is owned by another
// process and we peek into the items in the cache based on their offsets.
class ReadOnlySharedCacheView {
 public:
  // tries to attach to an existing cache with the cacheDir if present under
  // the correct shm mode.
  //
  // @param cacheDir    the directory that identifies the cache
  // @param usePosix    the posix compatbility status of original cache. This
  //                    would be part of the config that was used to
  //                    initialize the cache
  // @param addr        starting address that this segment should be mapped to
  //                    (exception will be thrown if it is not mounted to the
  //                     given address)
  //                    if nullptr, the segment will be mapped to a random
  //                    address chosen by the kernel
  // @param pageSize    page size backing the cache's main segment
  // @param hugePageMountDir hugetlbfs mount containing a POSIX huge-page cache
  // @param posixShmDir the cache's usePosixShmDirectory(), if any
  explicit ReadOnlySharedCacheView(const std::string& cacheDir,
                                   bool usePosixShm,
                                   void* addr = nullptr,
                                   const PageSize& pageSize = PageSize(),
                                   const std::string& hugePageMountDir = "",
                                   const std::string& posixShmDir = "")
      : shm_(ShmManager::attachShmReadOnly(cacheDir,
                                           detail::kShmCacheName,
                                           usePosixShm,
                                           addr,
                                           pageSize,
                                           hugePageMountDir,
                                           posixShmDir)) {}

  // returns the absolute address at which the shared memory mapping is mounted.
  // The caller can add a relative offset obtained from
  // CacheAllocator::getItemPtrAsOffset to this address in order to compute the
  // address at which that item is stored (within the process which manages this
  // ReadOnlySharedCacheView).
  uintptr_t getShmMappingAddress() const noexcept {
    auto mapping = shm_->getCurrentMapping();
    return reinterpret_cast<uintptr_t>(mapping.addr);
  }

  // computes an aboslute address in the cache, given a relative offset that
  // was obtained from CacheAllocator::getItemPtrAsOffset. It is the caller's
  // responsibility to ensure the memory backing the offset corresponds to an
  // active ItemHandle in the system.
  //
  // Returns a valid pointer if the offset is valid. Returns nullptr if no
  // shared memory mapping is mounted. Throws if the given offset is invalid.
  const void* getItemPtrFromOffset(uintptr_t offset) {
    auto mapping = shm_->getCurrentMapping();
    if (mapping.addr == nullptr) {
      return nullptr;
    }

    if (offset >= mapping.size) {
      throw std::invalid_argument(fmt::format(
          "Invalid offset {} with mapping of size {}", offset, mapping.size));
    }
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mapping.addr) +
                                   offset);
  }

 private:
  // the segment backing the cache
  std::unique_ptr<ShmSegment> shm_;
};

} // namespace cachelib
} // namespace facebook
