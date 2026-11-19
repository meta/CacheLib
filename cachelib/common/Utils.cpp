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

#include <dirent.h>
#include <fmt/core.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/debugging/exception_tracer/ExceptionTracer.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <atomic>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic pop
#include <folly/Random.h>
#include <folly/logging/xlog.h>

#include "cachelib/common/PageSize.h"
#include "cachelib/common/Utils.h"

namespace facebook {
namespace cachelib {
namespace util {

namespace {

constexpr size_t kPageSizeBytes = 4096;
const char* kProcShmMaxPath = "/proc/sys/kernel/shmmax";
const char* kProcShmAllPath = "/proc/sys/kernel/shmall";

// Get the value of kernel.shmmax in bytes.
// This function must be run as root.
//
// Use this to determine how much shm we can allocate on this system,
// if it's too low, the user may want to use `setShmMax` to set a higher
// limit.
uint64_t getShmMax() {
  std::ifstream shmMaxFile(kProcShmMaxPath);
  if (!shmMaxFile) {
    return 0;
  }

  uint64_t shmMax;
  if (shmMaxFile >> shmMax) {
    return shmMax;
  }

  return 0;
}

// Set the value of kernel.shmmax. This function must be run as root.
//
// @param shmMax  new shmmax value in bytes
//
// @return true on success, false otherwise
bool setShmMax(uint64_t shmMax) {
  std::ofstream shmMaxFile(kProcShmMaxPath, std::ios_base::trunc);
  if (!shmMaxFile) {
    return false;
  }

  shmMaxFile << shmMax;
  shmMaxFile.flush();
  return shmMaxFile.good();
}

// Get the value of kernel.shmall in number of pages.
// This function must be run as root.
//
// Use this to determine how much shm we can allocate *in total* on this system,
// if it's too low, the user may want to use `setShmAll` to set a higher
// limit.
uint64_t getShmAll() {
  std::ifstream shmAllFile(kProcShmAllPath);
  if (!shmAllFile) {
    return 0;
  }

  uint64_t shmAll;
  if (shmAllFile >> shmAll) {
    return shmAll;
  }

  return 0;
}

// Set the value of kernel.shmall. This function must be run as root.
//
// "shmall" is suggested to be set to "shmmax / PAGE_SIZE"
//
// @param shmAll  new shmmax value in bytes
//
// @return true on success, false otherwise
bool setShmAll(uint64_t shmAll) {
  std::ofstream shmAllFile(kProcShmAllPath, std::ios_base::trunc);
  if (!shmAllFile) {
    return false;
  }

  shmAllFile << shmAll;
  shmAllFile.flush();
  return shmAllFile.good();
}

} // namespace

void setShmIfNecessary(uint64_t bytes) {
  const auto curShmMax = getShmMax();
  if (curShmMax < bytes && !setShmMax(bytes)) {
    throw std::system_error(
        ENOMEM,
        std::system_category(),
        fmt::format("Cannot set shmmax to {} from {}", bytes, curShmMax));
  }

  const auto curShmAll = getShmAll();
  if (curShmAll * kPageSizeBytes < bytes) {
    // always set this to be just bigger than shmmax
    const auto desiredNumPages = bytes / kPageSizeBytes + 1;
    if (!setShmAll(desiredNumPages)) {
      throw std::system_error(ENOMEM,
                              std::system_category(),
                              fmt::format("Cannot set shmall to {} from {}",
                                          desiredNumPages, curShmAll));
    }
  }
}

void* align(size_t alignment, size_t size, void*& ptr, size_t& space) {
  XDCHECK(folly::isPowTwo(alignment));
  const size_t alignmentMask = ~(alignment - 1);
  auto alignedPtr = reinterpret_cast<uint8_t*>(
      (reinterpret_cast<uintptr_t>(ptr) & alignmentMask));

  // not properly aligned so set it to the next aligned address.
  if (alignedPtr != ptr) {
    alignedPtr += alignment;
  }

  const ptrdiff_t diff = alignedPtr - reinterpret_cast<uint8_t*>(ptr);

  if ((diff + size) <= space) {
    ptr = reinterpret_cast<void*>(alignedPtr);
    space -= diff;
    return ptr;
  }
  return nullptr;
}

void* mmapAlignedZeroedMemory(size_t alignment,
                              size_t numBytes,
                              bool noAccess,
                              const PageSize& pageSize) {
  if (!folly::isPowTwo(alignment) || numBytes == 0 ||
      numBytes > std::numeric_limits<size_t>::max() - alignment) {
    throw std::invalid_argument("Invalid aligned mmap size or alignment");
  }
  size_t mappedBytes = numBytes + alignment;
  size_t keptBytes = numBytes;
  if (!pageSize.isHugePage()) {
    const auto basePageSize = getPageSize();
    if (mappedBytes > std::numeric_limits<size_t>::max() - (basePageSize - 1)) {
      throw std::invalid_argument("Aligned mmap size overflows page rounding");
    }
    mappedBytes =
        getAlignedSize(mappedBytes, static_cast<uint32_t>(basePageSize));
    keptBytes = getAlignedSize(numBytes, static_cast<uint32_t>(basePageSize));
  }
  const auto protFlag = noAccess ? PROT_NONE : PROT_READ | PROT_WRITE;
  const auto mapFlag = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                       pageSize.hugePageMmapFlags();
  void* base = mmap(nullptr, mappedBytes, protFlag, mapFlag, -1, 0);
  if (base == MAP_FAILED) {
    throw std::system_error(errno, std::system_category(), "Cannot mmap");
  }
  auto* ownedStart = static_cast<uint8_t*>(base);
  size_t ownedBytes = mappedBytes;
  auto rollback = folly::makeGuard([&] { munmap(ownedStart, ownedBytes); });

  void* memory = base;
  size_t space = mappedBytes;
  auto* alignedMemory = align(alignment, numBytes, memory, space);
  if (alignedMemory == nullptr) {
    throw std::invalid_argument("Cannot align mmap reservation");
  }
  auto* alignedBytes = static_cast<uint8_t*>(alignedMemory);
  const size_t head = mappedBytes - space;
  if (head != 0) {
    if (munmap(ownedStart, head) != 0) {
      throw std::system_error(errno, std::system_category(),
                              "Cannot trim mmap head");
    }
    ownedStart = alignedBytes;
    ownedBytes -= head;
  }
  const size_t tail = ownedBytes - keptBytes;
  if (tail != 0) {
    if (munmap(alignedBytes + keptBytes, tail) != 0) {
      throw std::system_error(errno, std::system_category(),
                              "Cannot trim mmap tail");
    }
    ownedBytes = keptBytes;
  }
  rollback.dismiss();
  return alignedMemory;
}

void* mmapAlignedZeroedMemory(size_t alignment,
                              size_t numBytes,
                              bool noAccess) {
  return mmapAlignedZeroedMemory(alignment, numBytes, noAccess, PageSize{});
}

void setMaxLockMemory(uint64_t bytes) {
  struct rlimit rlim{bytes, bytes};
  const int rv = setrlimit(RLIMIT_MEMLOCK, &rlim);
  if (rv == 0) {
    return;
  }
  throw std::system_error(
      errno, std::system_category(),
      fmt::format("Error setting rlimit to {} bytes. Errno = {}", bytes,
                  errno));
}

size_t getNumResidentPages(const void* memory, size_t len) {
  if (!isPageAlignedAddr(memory)) {
    throw std::invalid_argument(
        fmt::format("addr {} is not page aligned", memory));
  }
  XDCHECK(isPageAlignedAddr(memory));
  const size_t numPages = getNumPages(len);

  // TODO this could be a large allocation. may be break it up if it matters.
  std::vector<unsigned char> vec(numPages, 0);
  const int rv = mincore(const_cast<void*>(memory), len, vec.data());
  if (rv != 0) {
    throw std::system_error(
        errno, std::system_category(),
        fmt::format("Error in mincore addr = {}, len = {}. errno = {}", memory,
                    len, errno));
  }
  return std::count_if(vec.begin(), vec.end(),
                       [](unsigned char c) { return c != 0; });
}

size_t getPageSize() noexcept {
  static long pagesize = sysconf(_SC_PAGESIZE);
  XDCHECK_NE(pagesize, -1);
  XDCHECK_GT(pagesize, 0);
  return pagesize;
}

size_t getNumPages(size_t len) noexcept {
  return (len + getPageSize() - 1) / getPageSize();
}

bool isPageAlignedAddr(const void* addr) noexcept {
  return reinterpret_cast<uintptr_t>(addr) % getPageSize() == 0;
}

/* returns true with the file's mode. false if the file does not exist.
 * throws system_error for all other errors */
bool getStatIfExists(const std::string& name, mode_t* mode) {
  struct stat buf = {};
  const int ret = stat(name.c_str(), &buf);
  if (ret == 0) {
    if (mode != nullptr) {
      *mode = buf.st_mode;
    }
    return true;
  } else if (errno != ENOENT && errno != ENOTDIR) {
    // some system error, but it might exist
    throwSystemError(errno, fmt::format("Path: {}", name));
  }

  // does not exist;
  return false;
}

bool pathExists(const std::string& path) {
  return getStatIfExists(path, nullptr);
}

bool isDir(const std::string& name) {
  struct stat buf = {};
  auto err = stat(name.c_str(), &buf);
  if (err) {
    throwSystemError(errno, fmt::format("Path: {}", name));
  }
  return S_ISDIR(buf.st_mode) ? true : false;
}

bool isBlk(const std::string& name) {
  struct stat buf = {};
  auto err = stat(name.c_str(), &buf);
  if (err) {
    throwSystemError(errno, fmt::format("Path: {}", name));
  }
  return S_ISBLK(buf.st_mode) ? true : false;
}

/* throws error on any failure. */
void makeDir(const std::string& name) {
  auto mkdirs = [](const std::string& path, mode_t mode) {
    char tmp[256];
    char* p = nullptr;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path.c_str());
    len = strlen(tmp);
    if (len == 0) {
      throw std::invalid_argument(fmt::format("Error forming path {}", path));
    }
    if (tmp[len - 1] == '/') {
      tmp[len - 1] = 0;
    }
    for (p = tmp + 1; *p; p++) {
      if (*p == '/') {
        *p = 0;
        SCOPE_EXIT { *p = '/'; };
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
          throwSystemError(errno, fmt::format("failed to create {}", tmp));
        }
        *p = '/';
      }
    }
    // error checked by caller
    mkdir(tmp, mode);
  };

  mkdirs(name.c_str(), 0777);
  if (!getStatIfExists(name, nullptr)) {
    throwSystemError(errno, fmt::format("{} expected to be existing", name));
  }
}

/* throws error on any failure. */
void removePath(const std::string& name) {
  if (!pathExists(name)) {
    return;
  }

  if (isDir(name)) {
    auto dir = opendir(name.c_str());
    if (!dir) {
      throwSystemError(errno, fmt::format("Err removing path={}", name));
    }
    SCOPE_EXIT { closedir(dir); };
    struct dirent* entry;
    while ((entry = readdir(dir))) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
      }
      std::string path = name + "/" + std::string(entry->d_name);
      removePath(path);
    }
    auto err = rmdir(name.c_str());
    if (err) {
      throwSystemError(errno, fmt::format("Err removing path={}", name));
    }
  } else {
    auto err = unlink(name.c_str());
    if (err) {
      throwSystemError(errno, fmt::format("Err removing path={}", name));
    }
  }
}

std::string getUniqueTempDir(folly::StringPiece prefix) {
  const char* dir = getenv("TMPDIR");
  if (dir == nullptr) {
    dir = "/tmp";
  }
  return fmt::format("{}/{}_{}", dir, prefix, folly::Random::rand32());
}

std::string toString(std::chrono::nanoseconds d) {
  // bunch of constants to help us round up and convert with appropriate
  // suffix. For example say 90ns or 5.6us or 5.5ms or 55s etc
  constexpr uint64_t micros =
      std::chrono::nanoseconds(std::chrono::microseconds(1)).count();
  constexpr uint64_t millis =
      std::chrono::nanoseconds(std::chrono::milliseconds(1)).count();
  constexpr uint64_t secs =
      std::chrono::nanoseconds(std::chrono::seconds(1)).count();

  uint64_t count = d.count();
  if (count < micros) {
    return fmt::format("{}ns", count);
  } else if (count < millis) {
    return fmt::format("{:.2f}us", static_cast<double>(count) / micros);
  } else if (count < secs) {
    return fmt::format("{:.2f}ms", static_cast<double>(count) / millis);
  } else {
    return fmt::format("{:.2f}s", static_cast<double>(count) / secs);
  }
}

namespace {
// char to int  conversion
bool isDigit(char c) { return std::isdigit(c); }

// NOLINTNEXTLINE(facebook-avoid-non-const-global-variables)
std::atomic<size_t (*)()> getCgroupMemAvailableFn{nullptr};
// NOLINTNEXTLINE(facebook-avoid-non-const-global-variables)
std::atomic<size_t (*)()> getRSSBytesFn{nullptr};

} // namespace

size_t getRSSBytes() {
  auto fn = getRSSBytesFn.load();
  if (fn != nullptr) {
    size_t rss = fn();
    if (rss > 0) {
      return rss;
    }
  }

  // read field 2 from /proc/self/statm according to
  // http://man7.org/linux/man-pages/man5/proc.5.html
  std::string memInfoStr;
  if (!folly::readFile("/proc/self/statm", memInfoStr)) {
    return 0;
  }

  XDCHECK(!std::isdigit(memInfoStr.back()));
  memInfoStr.pop_back();

  std::vector<folly::StringPiece> tokens;
  folly::split(' ', memInfoStr, tokens);

  // 7 numeric fields followed by carriage return
  XDCHECK_GE(tokens.size(), 7ULL);
  if (tokens.size() < 7) {
    return 0;
  }

  for (auto t : tokens) {
    XDCHECK_EQ(std::find_if_not(t.begin(), t.end(), isDigit), t.end());
  }

  size_t pages = folly::to<size_t>(tokens[1]) + 3;
  return pages * getPageSize();
}

size_t getMemAvailable() {
  auto fn = getCgroupMemAvailableFn.load();
  if (fn != nullptr) {
    size_t cgroupMem = fn();
    if (cgroupMem > 0) {
      return cgroupMem;
    }
  }

  std::string memInfoStr;
  if (!folly::readFile("/proc/meminfo", memInfoStr)) {
    return 0;
  }

  std::vector<folly::StringPiece> lines;
  folly::split('\n', memInfoStr, lines);

  constexpr folly::StringPiece memAvailStr{"MemAvailable"};
  for (auto l : lines) {
    if (l.startsWith(memAvailStr)) {
      // format is MemAvailable:   172048584 kB
      auto startIt = std::find_if(l.begin(), l.end(), isDigit);
      XDCHECK_NE(startIt, l.end());
      // last 3 chars are ' kB'
      auto endIt = l.end() - 3;
      XDCHECK_EQ(std::find_if_not(startIt, l.end(), isDigit), endIt);
      return folly::to<size_t>(folly::StringPiece{startIt, endIt}) * 1024ULL;
    }
  }
  return 0;
}

void setCgroupMemoryAdvising(CgroupMemAvailableFn provider) {
  getCgroupMemAvailableFn.store(provider);
}

void setRSSMemoryAdvising(RSSMemoryFn provider) {
  getRSSBytesFn.store(provider);
}

void printExceptionStackTraces() {
  auto exceptions = folly::exception_tracer::getCurrentExceptions();
  for (auto& exc : exceptions) {
    std::cerr << exc << std::endl;
  }
}

} // namespace util
} // namespace cachelib
} // namespace facebook
