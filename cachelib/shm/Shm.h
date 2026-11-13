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

#include <folly/logging/xlog.h>

#include <memory>
#include <system_error>

#include "cachelib/common/Utils.h"
#include "cachelib/shm/PosixShmSegment.h"
#include "cachelib/shm/ShmCommon.h"
#include "cachelib/shm/SysVShmSegment.h"

namespace facebook {
namespace cachelib {

/**
 * This class supports using a shared memory api and map that to our address
 * space. The segments can be created by they corresponding key type. They can
 * be mapped into the address space. Segments once mapped cannot map to
 * another address space until the first mapping is detached.
 * getCurrentMapping tells the usable address space mapped to the segment. The
 * mappings are detached when the object is destroyed. However, the segments
 * are not removed when the object is destroyed to allow reattaching to them.
 * This class is not thread safe.
 *
 * Look into tests/test_shm.cpp for usage.
 */
class ShmSegment {
 public:
  // create a new segment with the given key
  // @param name   name of the segment
  // @param size   size of the segment.
  // @param opts   the options for the segment.
  // @param hugePageMountDir  directory of a POSIX segment: the hugetlbfs mount
  //        for huge pages; for normal pages "" means /dev/shm. Unused by SysV.
  ShmSegment(ShmNewT,
             std::string name,
             size_t size,
             bool usePosix,
             const ShmSegmentOpts& opts = {},
             const std::string& hugePageMountDir = "") {
    if (usePosix) {
      segment_ = std::make_unique<PosixShmSegment>(
          ShmNew, std::move(name), size, opts, hugePageMountDir);
    } else {
      segment_ =
          std::make_unique<SysVShmSegment>(ShmNew, std::move(name), size, opts);
    }
  }

  // attach to an existing segment with the given key
  // @param name   name of the segment
  // @param opts   the options for the segment.
  // @param hugePageMountDir  directory of a POSIX segment: the hugetlbfs mount
  //        for huge pages; for normal pages "" means /dev/shm. Unused by SysV.
  ShmSegment(ShmAttachT,
             std::string name,
             bool usePosix,
             const ShmSegmentOpts& opts = {},
             const std::string& hugePageMountDir = "") {
    if (usePosix) {
      segment_ = std::make_unique<PosixShmSegment>(
          ShmAttach, std::move(name), opts, hugePageMountDir);
    } else {
      segment_ =
          std::make_unique<SysVShmSegment>(ShmAttach, std::move(name), opts);
    }
  }

  ~ShmSegment() {
    try {
      detachCurrentMapping();
    } catch (const std::system_error& e) {
      XDCHECK_EQ(e.code().value(), EINVAL); // Invalid errno
    }
  }

  ShmSegment(const ShmSegment&) = delete;
  ShmSegment& operator=(const ShmSegment&) = delete;
  ShmSegment(ShmSegment&&) = default;
  ShmSegment& operator=(ShmSegment&&) = default;

  // maps the segment into the address for the segment's current length. If the
  // segment is invalid or deleted, returns false and does nothing.
  //
  // @param  addr     the address to be attached at. If nullptr, the segment
  //                  will be mapped at a random address chosen by the kernel
  //
  // @param alignment if no addr is passedd and alignment is non-zero, the
  //                  address mapped will be aligned to this size. Must be power
  //                  of two.
  //
  // @return true if the mapping was successful, false otherwise. Upon
  // success, getCurrentMapping can be used to fetch the details of the
  // address mapping
  bool mapAddress(void* addr, size_t alignment = 1) {
    if (isMapped() || !segment_->isActive()) {
      return false;
    }

    // ignore alignment if address is already specified. but fail on assert
    // builds.
    if (!addr) {
      if (alignment == 0 || !folly::isPowTwo(alignment)) {
        // if alignment is passed, it should be valid non-zero power of two
        // value and if address is also passed
        return false;
      } else if (alignment > segment_->getPageSize()) {
        // address was not set and alignment args are sane.  We first mmap an
        // area of memory that is sufficiently larger and use that to get an
        // aligned address that we will later remap below.  The reason for mmap
        // such a large memory area upfront is because we need to ensure that
        // starting from the aligned address we will not have any overlaps with
        // other mappings. we map with PROT_NONE because we do not intend to
        // read or write until after we remap it onto our shared memory. Note
        // that we only need to do this if alignment > pageSize since the OS
        // will page align by default in segment_->mapAddress() below.
        addr = cachelib::util::mmapAlignedZeroedMemory(
            alignment, segment_->getSize(), true /* need-access */);
        XDCHECK_EQ(reinterpret_cast<uint64_t>(addr) & (alignment - 1), 0ULL);
      }
    }

    XDCHECK_NE(alignment, 0ULL);
    void* retAddr = segment_->mapAddress(addr);
    XDCHECK(retAddr == addr || addr == nullptr);
    mapping_ = ShmAddr{retAddr, segment_->getSize()};
    return true;
  }

  // marks the shared memory resource to be detached once all the
  // mappings are unmapped.
  void markForRemoval() { segment_->markForRemoval(); }

  // current size of the segment
  size_t getSize() const { return segment_->getSize(); }

  std::string getKeyStr() { return segment_->getKeyStr(); }

  bool isActive() const noexcept { return segment_->isActive(); }

  bool isMarkedForRemoval() const noexcept {
    return segment_->isMarkedForRemoval();
  }

  // if its marked to be removed and there is nothing mapped, it is invalid.
  bool isInvalid() const noexcept { return !(isMapped() || isActive()); }

  // is the segment currently mapped to the address space
  bool isMapped() const noexcept { return mapping_.isMapped(); }

  // detaches the current mapping if it exists.
  void detachCurrentMapping() {
    if (!isMapped()) {
      return;
    }

    segment_->unMap(mapping_.addr);
    mapping_ = {};
  }

  // returns the current mapping and the usable length of the mapping.
  const ShmAddr& getCurrentMapping() const noexcept { return mapping_; }

 private:
  std::unique_ptr<ShmBase> segment_{};
  ShmAddr mapping_{};
};
} // namespace cachelib
} // namespace facebook
