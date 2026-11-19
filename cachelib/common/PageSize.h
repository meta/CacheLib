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

#include <folly/lang/Bits.h>
#include <folly/logging/xlog.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>

#include <cstddef>
#include <set>

// On Linux, glibc's <bits/shm.h> provides SHM_HUGE_SHIFT only in >v2.36.  The
// canonical shift is in kernel UAPI <asm-generic/hugetlb_encode.h>.  Include
// only that header to avoid struct redefinition conflicts.
#ifdef __linux__
#include <asm-generic/hugetlb_encode.h>
#endif

#ifndef MAP_HUGETLB
#warning "MAP_HUGETLB not defined, disabling hugepage support"
#define MAP_HUGETLB 0
#define MAP_HUGE_SHIFT 0
#else
#ifndef MAP_HUGE_SHIFT
#ifdef HUGETLB_FLAG_ENCODE_SHIFT
#define MAP_HUGE_SHIFT HUGETLB_FLAG_ENCODE_SHIFT
#else
#warning "MAP_HUGE_SHIFT not defined, falling back to default huge page size"
#define MAP_HUGE_SHIFT 0
#endif
#endif
#endif

#ifndef SHM_HUGETLB
#warning "SHM_HUGETLB not defined, disabling hugepage support"
#define SHM_HUGETLB 0
#define SHM_HUGE_SHIFT 0
#else
#ifndef SHM_HUGE_SHIFT
#ifdef HUGETLB_FLAG_ENCODE_SHIFT
#define SHM_HUGE_SHIFT HUGETLB_FLAG_ENCODE_SHIFT
#else
#warning "SHM_HUGE_SHIFT not defined, falling back to default huge page size"
#define SHM_HUGE_SHIFT 0
#endif
#endif
#endif

namespace facebook {
namespace cachelib {

class PageSize {
 public:
  // Page size in bytes for a mapping or shm segment. kNormalPageSize selects
  // the system default page size; passing a supported huge-page size requests
  // HugeTLB-backed pages for the mapping. The valid huge-page sizes depend on
  // the CPU's base page granule; callers may pass any kernel-supported size
  // These constants are just conveniences, it does NOT mean they're available.
  static constexpr size_t kNormalPageSize = 0;
  // 4 KiB base page granule (x86-64, aarch64-4k)
  static constexpr size_t kHugePageSize2MB = 2ULL * 1024 * 1024;
  static constexpr size_t kHugePageSize1GB = 1024ULL * 1024 * 1024;
  // 16 KiB base page granule (aarch64-16k)
  static constexpr size_t kHugePageSize32MB = 32ULL * 1024 * 1024;
  static constexpr size_t kHugePageSize64GB = 64ULL * 1024 * 1024 * 1024;
  // 64 KiB base page granule (aarch64-64k)
  static constexpr size_t kHugePageSize512MB = 512ULL * 1024 * 1024;

  explicit PageSize(size_t pageSize = kNormalPageSize) : pageSize_(pageSize) {
    XDCHECK(pageSize == kNormalPageSize || folly::isPowTwo(pageSize_))
        << "Invalid page size " << pageSize_ << ", must be power-of-two";
  }

  /* the system base page size in bytes */
  static size_t systemPageSize();

  /* Huge-page sizes (in bytes) the running kernel supports, read from
   * /sys/kernel/mm/hugepages. Empty if none are available / not readable. */
  static const std::set<size_t>& supportedHugePageSizes();

  /* effective page size in bytes */
  size_t getPageSize() const noexcept;

  /* true if pageSize denotes a huge page (larger than the system base page) */
  bool isHugePage() const noexcept;

  /* mmap(2) huge-page flags for pageSize (0 if not huge or unsupported) */
  int hugePageMmapFlags() const noexcept;

  /* shmget(2) huge-page flags for pageSize (0 if not huge or unsupported) */
  int hugePageShmgetFlags() const noexcept;

  /* round up to the closest page size */
  size_t getPageAlignedSize(size_t size) const noexcept;

  /* returns page aligned size for the input that is atleast as big as the input
   * size */
  size_t pageAligned(size_t size) const noexcept;

  /* true if the length is page aligned  */
  bool isPageAlignedSize(size_t length) const noexcept;

  /* true if the address is page aligned */
  bool isPageAlignedAddr(void* addr) const noexcept;

  // return the page size (in bytes) of the address mapping in this process.
  //
  // @throw  std::invalid_argument if the address mapping is not found.
  static size_t getPageSizeInSMap(void* addr);

 private:
  size_t pageSize_;

  /* log2(pageSize); used to encode the MAP_HUGE_* / SHM_HUGE_* flag bits */
  unsigned hugePageSizeToShift() const noexcept;
};

} // namespace cachelib
} // namespace facebook
