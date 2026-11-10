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

#include "cachelib/shm/PosixShmSegment.h"

#include <fcntl.h>
#include <fmt/core.h>
#include <folly/Expected.h>
#include <folly/String.h>
#include <folly/logging/xlog.h>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>

#include "cachelib/common/Utils.h"

/* On Mac OS / FreeBSD, mmap(2) syscall does not support these flags */
#ifndef MAP_LOCKED
#warning "MAP_LOCKED not defined"
#define MAP_LOCKED 0
#endif

namespace facebook {
namespace cachelib {

constexpr static mode_t kRWMode = 0666;
using stat_t = struct stat;

namespace detail {

int shmOpenImpl(const char* name, int flags) {
  const int fd = shm_open(name, flags, kRWMode);

  if (fd != -1) {
    return fd;
  }

  switch (errno) {
  case EEXIST:
  case EMFILE:
  case ENFILE:
  case EACCES:
    util::throwSystemError(errno);
    break;
  case ENAMETOOLONG:
  case EINVAL:
    util::throwSystemError(errno, "Invalid segment name");
    break;
  case ENOENT:
    if (!(flags & O_CREAT)) {
      util::throwSystemError(errno);
    } else {
      XDCHECK(false);
      // FIXME: posix says that ENOENT is thrown only when O_CREAT
      // is not set. However, it seems to be set even when O_CREAT
      // was set and the parent of path name does not exist.
      util::throwSystemError(errno, "Invalid errno");
    }
    break;
  default:
    XDCHECK(false);
    util::throwSystemError(errno, "Invalid errno");
  }
  return kInvalidFD;
}

int openFileImpl(const char* path, int flags, mode_t mode) {
  const int fd = open(path, flags, mode);
  if (fd == -1) {
    util::throwSystemError(errno,
                           fmt::format("open() failed for path {}", path));
  }
  return fd;
}

template <bool isHugePage = false>
folly::Expected<bool, std::system_error> unlinkImpl(const char* const name) {
  if constexpr (isHugePage) {
    if (::unlink(name) == 0) {
      return true;
    }
  } else {
    if (shm_unlink(name) == 0) {
      return true;
    }
  }

  switch (errno) {
  case ENOENT:
    return false;
  case ENAMETOOLONG:
    [[fallthrough]];
  case EINVAL:
    return folly::makeUnexpected(std::system_error(
        errno, std::system_category(), "Invalid segment name"));
  default:
    return folly::makeUnexpected(
        std::system_error(errno, std::system_category()));
  }
}

void ftruncateImpl(int fd, size_t size) {
  const int ret = ftruncate(fd, size);
  if (ret == 0) {
    return;
  }
  switch (errno) {
  case EBADF:
  case EINVAL:
    util::throwSystemError(errno);
    break;
  default:
    XDCHECK(false);
    util::throwSystemError(errno, "Invalid errno");
  }
}

void fstatImpl(int fd, stat_t* buf) {
  const int ret = fstat(fd, buf);
  if (ret == 0) {
    return;
  }
  switch (errno) {
  case EBADF:
  case ENOMEM:
  case EOVERFLOW:
    util::throwSystemError(errno);
    break;
  default:
    XDCHECK(false);
    util::throwSystemError(errno, "Invalid errno");
  }
}

void* mmapImpl(
    void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
  void* ret = mmap(addr, length, prot, flags, fd, offset);
  if (ret != MAP_FAILED) {
    return ret;
  }

  switch (errno) {
  case EACCES:
  case EAGAIN:
    if (flags & MAP_LOCKED) {
      util::throwSystemError(ENOMEM);
      break;
    }
    [[fallthrough]];
  case EBADF:
  case EINVAL:
  case ENFILE:
  case ENODEV:
  case ENOMEM:
  case EPERM:
  case ETXTBSY:
  case EOVERFLOW:
    util::throwSystemError(errno);
    break;
  default:
    XDCHECK(false);
    util::throwSystemError(errno, "Invalid errno");
  }
  return nullptr;
}

void munmapImpl(void* addr, size_t length) {
  const int ret = munmap(addr, length);

  if (ret == 0) {
    return;
  } else if (errno == EINVAL) {
    util::throwSystemError(errno);
  } else {
    XDCHECK(false);
    util::throwSystemError(EINVAL, "Invalid errno");
  }
}

void getMempolicyImpl(int& oldMode, NumaBitMask& memBindNumaNodes) {
  auto nodeMask = memBindNumaNodes.getNativeBitmask();

  long ret =
      get_mempolicy(&oldMode, nodeMask->maskp, nodeMask->size, nullptr, 0);

  if (ret != 0) {
    util::throwSystemError(
        errno, fmt::format("get_mempolicy() failed: {}", std::strerror(errno)));
  }
}

void setMempolicyImpl(int oldMode, const NumaBitMask& memBindNumaNodes) {
  auto nodeMask = memBindNumaNodes.getNativeBitmask();

  long ret = set_mempolicy(oldMode, nodeMask->maskp, nodeMask->size);

  if (ret != 0) {
    util::throwSystemError(
        errno, fmt::format("set_mempolicy() failed: {}", std::strerror(errno)));
  }
}

int shmOpenForPageSize(const std::string& name,
                       int flags,
                       const PageSize& pageSize,
                       const std::string& hugePageMountDir) {
  if (!pageSize.isHugePage() && hugePageMountDir.empty()) {
    return detail::shmOpenImpl(name.c_str(), flags);
  }
  // HugeTLB segments must use a different mount from /dev/shm (which is
  // hardcoded by shm_open). Normal-page segments get a directory only from
  // usePosixShmDirectory().
  if (hugePageMountDir.empty()) {
    util::throwSystemError(
        EINVAL,
        "A hugetlbfs mount dir is required for huge-page POSIX shm segments");
  }
  auto path = std::filesystem::path(hugePageMountDir) /
              std::filesystem::path(name).relative_path();
  return detail::openFileImpl(path.lexically_normal().c_str(),
                              flags | O_CLOEXEC, kRWMode);
}

} // namespace detail

PosixShmSegment::PosixShmSegment(ShmAttachT,
                                 const std::string& name,
                                 ShmSegmentOpts opts,
                                 const std::string& hugePageMountDir)
    : ShmBase(std::move(opts), createKeyForName(name)),
      hugePageMountDir_(hugePageMountDir),
      fd_(getExisting(getName(), opts_, hugePageMountDir_)) {
  XDCHECK_NE(fd_, kInvalidFD);
  markActive();
  createReferenceMapping();
}

PosixShmSegment::PosixShmSegment(ShmNewT,
                                 const std::string& name,
                                 size_t size,
                                 ShmSegmentOpts opts,
                                 const std::string& hugePageMountDir)
    : ShmBase(std::move(opts), createKeyForName(name)),
      hugePageMountDir_(hugePageMountDir),
      fd_(createNewSegment(getName(), opts_, hugePageMountDir_)) {
  markActive();
  resize(size);
  XDCHECK(isActive());
  XDCHECK_NE(fd_, kInvalidFD);
  // this ensures that the segment lives while the object lives.
  createReferenceMapping();
}

PosixShmSegment::~PosixShmSegment() {
  try {
    // delete the reference mapping so the segment can be deleted if its
    // marked to be.
    deleteReferenceMapping();
  } catch (const std::system_error& e) {
    XLOG(ERR) << "Error deleting reference mapping: " << e.what();
  }

  // need to close the fd without throwing any exceptions. so we call close
  // directly.
  if (fd_ != kInvalidFD) {
    const int ret = close(fd_);
    if (ret != 0) {
      XDCHECK_NE(errno, EIO);
      XDCHECK_NE(errno, EINTR);
      XDCHECK_EQ(errno, EBADF);
      XDCHECK(!errno);
    }
  }
}

int PosixShmSegment::createNewSegment(const std::string& name,
                                      const ShmSegmentOpts& opts,
                                      const std::string& hugePageMountDir) {
  constexpr static int createFlags = O_RDWR | O_CREAT | O_EXCL;
  return detail::shmOpenForPageSize(name, createFlags, opts.pageSize,
                                    hugePageMountDir);
}

int PosixShmSegment::getExisting(const std::string& name,
                                 const ShmSegmentOpts& opts,
                                 const std::string& hugePageMountDir) {
  int flags = opts.readOnly ? O_RDONLY : O_RDWR;
  return detail::shmOpenForPageSize(name, flags, opts.pageSize,
                                    hugePageMountDir);
}

void PosixShmSegment::markForRemoval() {
  if (isActive()) {
    // we still have the fd open. so we can use it to perform ftruncate
    // even after marking for removal through unlink. The fd does not get
    // recycled until we actually destroy this object.
    removeByName(getName(), hugePageMountDir_);
    markForRemove();
  } else {
    XDCHECK(false);
  }
}

bool PosixShmSegment::removeByName(const std::string& segmentName,
                                   const std::string& hugePageMountDir) {
  const auto key = createKeyForName(segmentName);
  bool removed = false;

  auto result = detail::unlinkImpl(key.c_str());
  if (result.hasValue()) {
    removed |= result.value();
  }

  if (!hugePageMountDir.empty()) {
    const auto path = std::filesystem::path(hugePageMountDir) /
                      std::filesystem::path(key).relative_path();
    auto secondResult = detail::unlinkImpl</*isHugePage=*/true>(
        path.lexically_normal().c_str());
    if (secondResult.hasValue()) {
      removed |= secondResult.value();
    } else if (result.hasValue()) {
      result = std::move(secondResult);
    }
  }

  if (result.hasError()) {
    throw std::move(result).error();
  }
  return removed;
}

size_t PosixShmSegment::getSize() const {
  if (isActive() || isMarkedForRemoval()) {
    stat_t buf = {};
    detail::fstatImpl(fd_, &buf);
    return buf.st_size;
  } else {
    throw std::runtime_error(fmt::format(
        "Trying to get size of  segment with name {} in an invalid state",
        getName()));
  }
}

void PosixShmSegment::resize(size_t size) const {
  size = opts_.pageSize.getPageAlignedSize(size);
  XDCHECK(isActive() || isMarkedForRemoval());
  if (isActive() || isMarkedForRemoval()) {
    XDCHECK_NE(fd_, kInvalidFD);
    detail::ftruncateImpl(fd_, size);
  } else {
    throw std::runtime_error(
        fmt::format("Trying to resize segment with name {} in an invalid state",
                    getName()));
  }
}

void* PosixShmSegment::mapAddress(void* addr) const {
  size_t size = getSize();
  if (!opts_.pageSize.isPageAlignedSize(size) ||
      !opts_.pageSize.isPageAlignedAddr(addr)) {
    util::throwSystemError(EINVAL, "Address/size not aligned");
  }

  int flags = MAP_SHARED | opts_.pageSize.hugePageMmapFlags();
  // If users pass in an address, they must make sure that address is unused.
  if (addr != nullptr) {
    flags |= MAP_FIXED;
  }

  const int prot = opts_.readOnly ? PROT_READ : PROT_WRITE | PROT_READ;

  void* retAddr = detail::mmapImpl(addr, size, prot, flags, fd_, 0);
  // if there was hint for mapping, then fail if we cannot respect this
  // because we want to be specific about mapping to exactly that address.
  if (retAddr != nullptr && addr != nullptr && retAddr != addr) {
    util::throwSystemError(EINVAL, "Address already mapped");
  }
  XDCHECK(retAddr == addr || addr == nullptr);
  memBind(retAddr);
  return retAddr;
}

void PosixShmSegment::unMap(void* addr) const {
  detail::munmapImpl(addr, getSize());
}

static void forcePageAllocation(void* addr, size_t size, size_t pageSize) {
  char* startAddr = reinterpret_cast<char*>(addr);
  char* endAddr = startAddr + size;
  for (volatile char* curAddr = startAddr; curAddr < endAddr;
       curAddr += pageSize) {
    *curAddr = *curAddr;
  }
}

void PosixShmSegment::memBind(void* addr) const {
  if (opts_.memBindNumaNodes.empty()) {
    return;
  }

  NumaBitMask oldMemBindNumaNodes;
  int oldMode = 0;

  // mbind() cannot be used because mmap was called with MAP_SHARED flag
  // But we can set memory policy for current thread and force page allocation.
  // The following logic is used:
  // 1. Remember current memory policy for the current thread
  // 2. Set new memory policy as specified by config
  // 3. Force page allocation by touching every page in the segment
  // 4. Restore memory policy

  // Remember current memory policy
  detail::getMempolicyImpl(oldMode, oldMemBindNumaNodes);

  // Set memory bindings
  detail::setMempolicyImpl(MPOL_BIND, opts_.memBindNumaNodes);

  forcePageAllocation(addr, getSize(), opts_.pageSize.getPageSize());

  // Restore memory policy for the thread
  detail::setMempolicyImpl(oldMode, oldMemBindNumaNodes);
}

std::string PosixShmSegment::createKeyForName(
    const std::string& name) noexcept {
  // ensure that the slash is always there in the head. repetitive
  // slash is fine.
  if (name.empty() || name[0] != '/') {
    return "/" + name;
  } else {
    return name;
  }
}

void PosixShmSegment::createReferenceMapping() {
  // create a mapping that lasts the life of this object. mprotect it to
  // ensure there are no actual accesses.
  referenceMapping_ = detail::mmapImpl(nullptr, opts_.pageSize.getPageSize(),
                                       PROT_NONE, MAP_SHARED, fd_, 0);

  XDCHECK(referenceMapping_ != nullptr);
  if (madvise(referenceMapping_, opts_.pageSize.getPageSize(), MADV_DONTDUMP) !=
      0) {
    XLOG(WARN) << "Failure to exclude reference mapping from coredump: "
               << folly::errnoStr(errno);
  }
}

void PosixShmSegment::deleteReferenceMapping() const {
  if (referenceMapping_ != nullptr) {
    detail::munmapImpl(referenceMapping_, opts_.pageSize.getPageSize());
  }
}
} // namespace cachelib
} // namespace facebook
