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

#include "cachelib/shm/ShmCommon.h"

#include <fmt/core.h>
#include <folly/Conv.h>
#include <folly/FileUtil.h>
#include <folly/Random.h>
#include <folly/Range.h>
#include <folly/String.h>
#include <folly/lang/Bits.h>
#include <folly/logging/xlog.h>
#include <sys/types.h>

#include <filesystem>
#include <system_error>

namespace facebook {
namespace cachelib {

/* static */ size_t PageSize::systemPageSize() {
  static const size_t kPageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  return kPageSize;
}

/* static */ const std::set<size_t>& PageSize::supportedHugePageSizes() {
  auto getSizes = []() {
    std::set<size_t> sizes;
    constexpr folly::StringPiece kPrefix{"hugepages-"};
    constexpr folly::StringPiece kSuffix{"kB"};
    std::error_code ec;
    try {
      for (const auto& entry : std::filesystem::directory_iterator(
               "/sys/kernel/mm/hugepages", ec)) {
        const auto name = entry.path().filename().string();
        const folly::StringPiece namePiece{name};
        if (!namePiece.startsWith(kPrefix) || !namePiece.endsWith(kSuffix)) {
          continue;
        }
        const auto mid = namePiece.subpiece(
            kPrefix.size(), namePiece.size() - kPrefix.size() - kSuffix.size());
        if (const auto kb = folly::tryTo<size_t>(mid); kb.hasValue()) {
          sizes.insert(kb.value() * 1024);
        }
      }
    } catch (const std::exception& e) {
      XLOG(WARN) << "Failed to get all huge page sizes: " << e.what();
    }
    return sizes;
  };
  static const std::set<size_t> kSizes = getSizes();
  return kSizes;
}

size_t PageSize::getPageSize() const noexcept {
  return pageSize_ != kNormalPageSize ? pageSize_ : systemPageSize();
}

bool PageSize::isHugePage() const noexcept {
  return pageSize_ > systemPageSize();
}

unsigned PageSize::hugePageSizeToShift() const noexcept {
  XDCHECK(isHugePage() && pageSize_ != 0);
  return folly::findLastSet(pageSize_) - 1;
}

int PageSize::hugePageMmapFlags() const noexcept {
#if defined(MAP_HUGETLB) && MAP_HUGETLB != 0
  if (isHugePage()) {
    return MAP_HUGETLB |
           static_cast<int>(hugePageSizeToShift() << MAP_HUGE_SHIFT);
  }
#endif
  return 0;
}

int PageSize::hugePageShmgetFlags() const noexcept {
#if defined(SHM_HUGETLB) && SHM_HUGETLB != 0
  if (isHugePage()) {
    return SHM_HUGETLB |
           static_cast<int>(hugePageSizeToShift() << SHM_HUGE_SHIFT);
  }
#endif
  return 0;
}

size_t PageSize::getPageAlignedSize(size_t size) const noexcept {
  const auto pageSize = getPageSize();
  if (size == 0) {
    return pageSize;
  }

  auto delta = size % pageSize;
  return delta == 0 ? size : size + pageSize - delta;
}

size_t PageSize::pageAligned(size_t size) const noexcept {
  const auto pageSize = getPageSize();
  XDCHECK(!(pageSize & (pageSize - 1)));
  return 1 + ((size - 1) | (pageSize - 1));
}

bool PageSize::isPageAlignedSize(size_t size) const noexcept {
  return ((size != 0) && (size % getPageSize() == 0));
}

bool PageSize::isPageAlignedAddr(void* addr) const noexcept {
  return reinterpret_cast<uintptr_t>(addr) % getPageSize() == 0;
}

namespace {
std::vector<folly::StringPiece> getSmapLines(const std::string& smapContent) {
  std::vector<folly::StringPiece> lines;
  folly::split('\n', smapContent, lines, true);
  XDCHECK(!lines.empty());
  return lines;
}

size_t getAddressVal(folly::StringPiece addr) {
  // addresses are in base 16
  const size_t ret = strtoull(addr.data(), nullptr, 16);
  XDCHECK_NE(ret, 0u);
  return ret;
}

bool lineAddressMatches(folly::StringPiece line, uintptr_t addr) {
  // line should be of form
  // 006de000-01397000 rw-p 00000000 00:00 0                          [heap]

  std::vector<folly::StringPiece> tokens;
  // split into tokens by space
  folly::split(' ', line, tokens, /* ignore empty */ true);

  XDCHECK(!tokens.empty());
  folly::StringPiece startAddr;
  folly::StringPiece endAddr;

  // split the first token using the '-' separator
  if (!folly::split('-', tokens[0], startAddr, endAddr)) {
    throw std::invalid_argument(
        fmt::format("Invalid address field {}", tokens[0]));
  }

  // parse the address values. smaps ranges are half-open [start, end).
  size_t start = getAddressVal(startAddr);
  size_t end = getAddressVal(endAddr);
  return start <= addr && addr < end;
}

bool isAddressLine(folly::StringPiece line) {
  // address lines contain lots of fields before the first :
  // 006de000-01397000 rw-p 00000000 00:00 0                          [heap]
  folly::StringPiece first, second;
  folly::split<false>(':', line, first, second);
  return first.find(' ') != std::string::npos;
}
} // namespace

/* static */ size_t PageSize::getPageSizeInSMap(void* addr) {
  std::string smapContent;
  folly::readFile("/proc/self/smaps", smapContent);
  const auto smapLines = getSmapLines(smapContent);

  bool foundMatching = false;
  for (auto line : smapLines) {
    const bool isAddr = isAddressLine(line);
    if (!foundMatching && isAddr &&
        lineAddressMatches(line, reinterpret_cast<uintptr_t>(addr))) {
      foundMatching = true;
      continue;
    }

    if (!foundMatching) {
      continue;
    }

    XDCHECK(foundMatching);
    XDCHECK(!isAddr);

    // Format is the following
    // KernelPageSize:        4 kB
    folly::StringPiece fieldName, value;
    folly::split(':', line, fieldName, value);
    if (fieldName != "MMUPageSize") {
      continue;
    }

    value = folly::skipWhitespace(value);

    folly::StringPiece sizeVal;
    folly::StringPiece unitVal;
    folly::split(' ', value, sizeVal, unitVal);
    XDCHECK_EQ(unitVal, "kB");
    return folly::to<size_t>(sizeVal) * 1024;
  }
  throw std::invalid_argument("address mapping not found in /proc/self/smaps");
}

} // namespace cachelib
} // namespace facebook
