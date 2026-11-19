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

#include "cachelib/allocator/memory/SlabAllocator.h"

#include <fmt/core.h>
#include <folly/Likely.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>
#include <folly/synchronization/SanitizeThread.h>
#include <sys/mman.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "cachelib/common/PageSize.h"
#include "cachelib/common/Utils.h"

/* Missing madvise(2) flags on MacOS */
#ifndef MADV_REMOVE
#define MADV_REMOVE 0
#endif
#ifndef MADV_DONTDUMP
#define MADV_DONTDUMP 0
#endif

using namespace facebook::cachelib;

namespace {
static inline size_t roundDownToSlabSize(size_t size) {
  return size - (size % sizeof(Slab));
}

FOLLY_DISABLE_ADDRESS_SANITIZER void touchAddr(const uint8_t* addr) {
  // Use volatile to fool the compiler to not optimize this away in opt mode.
  volatile const uint8_t val = *addr; // NOLINT(facebook-hte-Volatile)
  (void)val;
}
} // namespace

void SlabAllocator::MappingDeleter::operator()(void* memory) const noexcept {
  munmap(memory, length);
}

SlabAllocator::OwnedMemory SlabAllocator::allocateOwnedMemory(
    size_t size, const PageSize& pageSize) {
  const size_t mapSize = pageSize.getPageAlignedSize(size);
  const size_t alignment = std::max(sizeof(Slab), pageSize.getPageSize());
  return OwnedMemory{util::mmapAlignedZeroedMemory(
                         alignment, mapSize, /*noAccess=*/false, pageSize),
                     MappingDeleter{mapSize}};
}

void SlabAllocator::checkState() const {
  if (memoryStart_ == nullptr || memorySize_ <= Slab::kSize) {
    throw std::invalid_argument(
        fmt::format("Invalid memory spec. memoryStart = {}, size = {}",
                    memoryStart_,
                    memorySize_));
  }

  if (slabMemoryStart_ == nullptr || nextSlabAllocation_ == nullptr) {
    throw std::invalid_argument(
        fmt::format("Invalid slabMemoryStart_ {} of nextSlabAllocation_ {}",
                    fmt::ptr(slabMemoryStart_),
                    fmt::ptr(nextSlabAllocation_)));
  }

  // nextSlabAllocation_ should be valid.
  if (nextSlabAllocation_ > getSlabMemoryEnd()) {
    throw std::invalid_argument(
        fmt::format("Invalid nextSlabAllocation_ {}, with SlabMemoryEnd {}",
                    fmt::ptr(nextSlabAllocation_),
                    fmt::ptr(getSlabMemoryEnd())));
  }

  for (const auto slab : freeSlabs_) {
    if (!isValidSlab(slab)) {
      throw std::invalid_argument(
          fmt::format("Invalid free slab {}", fmt::ptr(slab)));
    }
  }
}

void SlabAllocator::logAsanPoisoningStatus() const {
#if FOLLY_SANITIZE_ADDRESS
  if (asanPoisoningEnabled_) {
    XLOG(INFO, "CacheLib slab ASAN poisoning is ENABLED");
  } else {
    XLOG(INFO, "CacheLib slab ASAN poisoning is DISABLED");
  }
#endif
}

SlabAllocator::~SlabAllocator() {
  stopMemoryLocker();

  if (ownsMemory_) {
    munmap(memoryStart_, mmapLength_);
  }
}

void SlabAllocator::stopMemoryLocker() {
  if (memoryLocker_.joinable()) {
    stopLocking_ = true;
    memoryLocker_.join();
  }
}

SlabAllocator::SlabAllocator(size_t size, const Config& config)
    : SlabAllocator(
          allocateOwnedMemory(size, config.hugePageSize), size, config) {
  XDCHECK(!isRestorable());
}

SlabAllocator::SlabAllocator(OwnedMemory memory,
                             size_t size,
                             const Config& config)
    : SlabAllocator(memory.get(), size, true, config) {
  mmapLength_ = memory.get_deleter().length;
  auto* released = memory.release();
  XDCHECK_EQ(released, memoryStart_);
}

SlabAllocator::SlabAllocator(void* memoryStart,
                             size_t memorySize,
                             const Config& config)
    : SlabAllocator(memoryStart, memorySize, false, config) {
  XDCHECK(isRestorable());
}

SlabAllocator::SlabAllocator(void* memoryStart,
                             size_t memorySize,
                             bool ownsMemory,
                             const Config& config)
    : memoryStart_(memoryStart),
      memorySize_(roundDownToSlabSize(memorySize)),
      slabMemoryStart_(computeSlabMemoryStart(memoryStart_, memorySize_)),
      nextSlabAllocation_(slabMemoryStart_),
      ownsMemory_(ownsMemory)
#if FOLLY_SANITIZE_ADDRESS
      ,
      asanPoisoningEnabled_(config.enableAsanPoisoning)
#endif
{
  auto stopLockerOnFailure = folly::makeGuard([this] { stopMemoryLocker(); });
  checkState();

  static_assert(!(sizeof(Slab) & (sizeof(Slab) - 1)),
                "slab size must be power of two");

  if (config.excludeFromCoredump) {
    excludeMemoryFromCoredump(config.hugePageSize);
  }

  if (config.lockMemory) {
    memoryLocker_ = std::thread{[this]() { lockMemoryAsync(); }};
  }

  asanPoisonMemoryRegion(
      slabMemoryStart_,
      reinterpret_cast<const uint8_t*>(getSlabMemoryEnd()) -
          reinterpret_cast<const uint8_t*>(slabMemoryStart_));

  // Poison the slab header array - headers are kept poisoned at rest
  asanPoisonMemoryRegion(memoryStart_,
                         reinterpret_cast<const uint8_t*>(slabMemoryStart_) -
                             reinterpret_cast<const uint8_t*>(memoryStart_));

  XDCHECK_EQ(0u, reinterpret_cast<uintptr_t>(memoryStart_) % sizeof(Slab));
  XDCHECK_EQ(0u, memorySize_ % sizeof(Slab));
  XDCHECK(nextSlabAllocation_ != nullptr);
  XDCHECK_EQ(reinterpret_cast<uintptr_t>(nextSlabAllocation_),
             reinterpret_cast<uintptr_t>(slabMemoryStart_));

  logAsanPoisoningStatus();
  stopLockerOnFailure.dismiss();
}

SlabAllocator::SlabAllocator(const serialization::SlabAllocatorObject& object,
                             void* memoryStart,
                             size_t memSize,
                             const Config& config)
    : memoryStart_(memoryStart),
      memorySize_(*object.memorySize()),
      slabMemoryStart_(computeSlabMemoryStart(memoryStart_, memorySize_)),
      nextSlabAllocation_(getSlabForIdx(*object.nextSlabIdx())),
      canAllocate_(*object.canAllocate()),
      ownsMemory_(false)
#if FOLLY_SANITIZE_ADDRESS
      ,
      asanPoisoningEnabled_(config.enableAsanPoisoning)
#endif
{
  if (Slab::kSize != *object.slabSize()) {
    throw std::invalid_argument(
        fmt::format("current slab size {} does not match the previous one {}",
                    Slab::kSize,
                    *object.slabSize()));
  }

  if (getMinAllocSize() != *object.minAllocSize()) {
    throw std::invalid_argument(fmt::format(
        "current min alloc size {} does not match the previous one {}",
        getMinAllocSize(),
        *object.minAllocSize()));
  }

  XDCHECK(isRestorable());

  const size_t currSize = roundDownToSlabSize(memSize);
  if (memorySize_ != currSize) {
    throw std::invalid_argument(
        fmt::format("Memory size {} does not match the saved state's size {}",
                    currSize,
                    memorySize_));
  }

  if (config.excludeFromCoredump) {
    excludeMemoryFromCoredump(config.hugePageSize);
  }

  for (const auto& pair : *object.memoryPoolSize()) {
    const PoolId id = pair.first;
    if (id >= static_cast<PoolId>(memoryPoolSize_.size())) {
      throw std::invalid_argument(
          fmt::format("Invalid class id {}. Max Class Id {}",
                      id,
                      memoryPoolSize_.size() - 1));
    }
    memoryPoolSize_[id] = pair.second;
  }

  for (auto freeSlabIdx : *object.freeSlabIdxs()) {
    freeSlabs_.push_back(getSlabForIdx(freeSlabIdx));
  }

  for (auto advisedSlabIdx : *object.advisedSlabIdxs()) {
    // The slab headers in previous release did not have advised flag
    // set in the slab header. To avoid memory locking from touching
    // advised slab pages, we'd have to cold roll. To avoid cold roll
    // explicitly set the advised bit here.
    auto header = getSlabHeader(advisedSlabIdx);
    XDCHECK(header != nullptr);
    header->setAdvised(true);
    advisedSlabs_.push_back(getSlabForIdx(advisedSlabIdx));
  }

  if (config.lockMemory) {
    memoryLocker_ = std::thread{[this]() { lockMemoryAsync(); }};
  }

  checkState();

  // Poison slabs that are not allocated. Tests depend on this running after
  // checkState().

  asanPoisonMemoryRegion(
      nextSlabAllocation_,
      reinterpret_cast<const uint8_t*>(getSlabMemoryEnd()) -
          reinterpret_cast<const uint8_t*>(nextSlabAllocation_));

  for (const auto* slab : freeSlabs_) {
    asanPoisonMemoryRegion(slab, Slab::kSize);
  }

  for (const auto* slab : advisedSlabs_) {
    asanPoisonMemoryRegion(slab, Slab::kSize);
  }

  // Poison the slab header array last, after the restore-time header reads
  // (checkState) and writes (advised flag) above have run on live headers.
  asanPoisonMemoryRegion(memoryStart_,
                         reinterpret_cast<const uint8_t*>(slabMemoryStart_) -
                             reinterpret_cast<const uint8_t*>(memoryStart_));

  logAsanPoisoningStatus();
}

void SlabAllocator::lockMemoryAsync() noexcept {
  try {
    // memory start is always page aligned since it is aligned to slab size.
    auto* mem = reinterpret_cast<const uint8_t* const>(memoryStart_);
    XDCHECK(util::isPageAlignedAddr(mem));

    const size_t numPages = util::getNumPages(memorySize_);
    const size_t pageSize = util::getPageSize();

    size_t pageOffset = 0;
    size_t numAdvisedAwayPages = 0;

    while (pageOffset < numPages) {
      if (stopLocking_) {
        return;
      }

      auto pageAddr = mem + pageOffset * pageSize;
      // Avoid touching advised away pages.
      const auto header = getSlabHeader(pageAddr);
      if (header && header->isAdvised()) {
        ++numAdvisedAwayPages;
      } else {
        // this relies on the fact that the pages used with the allocator are
        // shared memory pages. For memory that is not shared, touching the
        // memory won't page them in until the page gets written to. We default
        // to mlock for that and require the caller to set the appropriate
        // rlimits.
        touchAddr(pageAddr);
      }

      ++pageOffset;

      if (pageOffset % kPagesPerStep == 0) {
        /* sleep override */
        std::this_thread::sleep_for(std::chrono::milliseconds(kLockSleepMS));
      }
    }

    // verify everything got paged in. If it doesn't, then we'll end up locking
    // advised away pages, which means we'll start off with some unusable
    // cache memory.
    const auto numInCore = util::getNumResidentPages(memoryStart_, memorySize_);
    if (numInCore != numPages - numAdvisedAwayPages) {
      XLOGF(ERR,
            "could not page in all memory. numPages = {}, numInCore = {}. "
            "Trying to mlock.",
            numPages - numAdvisedAwayPages, numInCore);
      // try mlock to see if that helps.
      const int rv = mlock(memoryStart_, memorySize_);
      if (rv != 0) {
        XLOGF(ERR, "could not mlock. errno = {}", errno);
      }
    }
  } catch (const std::exception& e) {
    XLOGF(ERR, "Exception during locking memory {}", e.what());
  }
}

namespace {
unsigned int numSlabs(size_t memorySize) noexcept {
  return static_cast<unsigned int>(memorySize / sizeof(Slab));
}
unsigned int numSlabsForHeaders(size_t memorySize) noexcept {
  const size_t headerSpace = sizeof(SlabHeader) * numSlabs(memorySize);
  return static_cast<unsigned int>((headerSpace + sizeof(Slab) - 1) /
                                   sizeof(Slab));
}
} // namespace

unsigned int SlabAllocator::getNumUsableSlabs(size_t memorySize) noexcept {
  return numSlabs(memorySize) - numSlabsForHeaders(memorySize);
}

unsigned int SlabAllocator::getNumUsableSlabs() const noexcept {
  return getNumUsableAndAdvisedSlabs() -
         static_cast<unsigned int>(numSlabsReclaimable());
}

unsigned int SlabAllocator::getNumUsableAndAdvisedSlabs() const noexcept {
  return static_cast<unsigned int>(getSlabMemoryEnd() - slabMemoryStart_);
}

Slab* SlabAllocator::computeSlabMemoryStart(void* memoryStart,
                                            size_t memorySize) {
  // compute the number of slabs we can have.
  const auto numHeaderSlabs = numSlabsForHeaders(memorySize);
  if (numSlabs(memorySize) <= numHeaderSlabs) {
    throw std::invalid_argument("not enough memory for slabs");
  }

  if (memoryStart == nullptr ||
      reinterpret_cast<uintptr_t>(memoryStart) % sizeof(Slab)) {
    throw std::invalid_argument(
        fmt::format("Invalid memory start {}", memoryStart));
  }

  // reserve the first numHeaderSlabs for storing the header info for all the
  // slabs.
  return reinterpret_cast<Slab*>(memoryStart) + numHeaderSlabs;
}

Slab* SlabAllocator::makeNewSlabImpl() {
  // early return without any locks.
  if (!canAllocate_) {
    return nullptr;
  }

  LockHolder l(lock_);
  // grab a free slab if it exists.
  if (!freeSlabs_.empty()) {
    auto slab = freeSlabs_.back();
    freeSlabs_.pop_back();
    return slab;
  }

  XDCHECK_EQ(0u,
             reinterpret_cast<uintptr_t>(nextSlabAllocation_) % sizeof(Slab));

  // check if we have any more memory left.
  if (allMemorySlabbed()) {
    // free list is empty and we have slabbed all the memory.
    canAllocate_ = false;
    return nullptr;
  }

  // allocate a new slab.
  return nextSlabAllocation_++;
}

// This does not hold the lock since the expectation is that its used with
// new/free/advised away slabs which are not in active use.
void SlabAllocator::initializeHeader(Slab* slab, PoolId id) {
  auto* header = getSlabHeader(slab);
  XDCHECK(header != nullptr);
  header->initialize(id);
}

Slab* SlabAllocator::makeNewSlab(PoolId id) {
  Slab* slab = makeNewSlabImpl();
  if (slab == nullptr) {
    return nullptr;
  }

  memoryPoolSize_[id] += sizeof(Slab);
  // initialize the header for the slab.
  initializeHeader(slab, id);
  return slab;
}

void SlabAllocator::freeSlab(Slab* slab) {
  // find the header for the slab.
  auto* header = getSlabHeader(slab);
  XDCHECK(header != nullptr);
  if (header == nullptr) {
    throw std::runtime_error(fmt::format("Invalid Slab {}", fmt::ptr(slab)));
  }

  memoryPoolSize_[header->getPoolId()] -= sizeof(Slab);
  // grab the lock
  LockHolder l(lock_);
  freeSlabs_.push_back(slab);
  canAllocate_ = true;
  header->resetAllocInfo();
}

bool SlabAllocator::adviseSlab(Slab* slab) {
  // find the header for the slab.
  auto* header = getSlabHeader(slab);
  if (header == nullptr) {
    throw std::runtime_error(fmt::format("Invalid Slab {}", fmt::ptr(slab)));
  }
  // Mark slab as advised in header prior to advising to avoid it from being
  // touched during memory locking.
  header->setAdvised(true);
  // madvise kernel to release this slab. Do this while not holding the
  // lock since the MADV_REMOVE happens inline.
  auto ret = madvise((void*)slab->memoryAtOffset(0), Slab::kSize, MADV_REMOVE);
  if (!ret || pretendMadvise_) {
    LockHolder l(lock_);
    advisedSlabs_.push_back(slab);
    // This doesn't reset flags
    header->resetAllocInfo();
    return true;
  }
  // Unset the flag since we failed to advise this slab away
  header->setAdvised(false);
  return false;
}

Slab* FOLLY_NULLABLE SlabAllocator::reclaimSlab(PoolId id) {
  Slab* slab = nullptr;
  {
    LockHolder l(lock_);
    if (!advisedSlabs_.empty()) {
      slab = advisedSlabs_.back();
      advisedSlabs_.pop_back();
    }
  }

  if (!slab) {
    return nullptr;
  }

  const size_t numPages = util::getNumPages(sizeof(Slab));
  const size_t pageSize = util::getPageSize();
  auto* mem = reinterpret_cast<const uint8_t* const>(slab->memoryAtOffset(0));
  XDCHECK(util::isPageAlignedAddr(mem));

  for (size_t pageOffset = 0; pageOffset < numPages; pageOffset++) {
    touchAddr(mem + pageOffset * pageSize);
  }
  memoryPoolSize_[id] += sizeof(Slab);
  // initialize the header for the slab.
  initializeHeader(slab, id);
  return slab;
}

SlabHeader* SlabAllocator::getSlabHeader(
    const Slab* const slab) const noexcept {
  if ([&] {
        // TODO(T79149875): Fix data race exposed by TSAN.
        folly::annotate_ignore_thread_sanitizer_guard g(__FILE__, __LINE__);
        return isValidSlab(slab);
      }()) {
    return [&] {
      // TODO(T79149875): Fix data race exposed by TSAN.
      folly::annotate_ignore_thread_sanitizer_guard g(__FILE__, __LINE__);
      return getSlabHeader(slabIdx(slab));
    }();
  }
  return nullptr;
}

bool SlabAllocator::isMemoryInSlab(const void* ptr,
                                   const Slab* slab) const noexcept {
  if (!isValidSlab(slab)) {
    return false;
  }
  return getSlabForMemory(ptr) == slab;
}

std::tuple<uint32_t, const void*> SlabAllocator::getRandomAlloc()
    const noexcept {
  // disregard the space we use for slab header.
  const auto validMaxOffset =
      memorySize_ - (reinterpret_cast<uintptr_t>(slabMemoryStart_) -
                     reinterpret_cast<uintptr_t>(memoryStart_));

  // pick a random location in the memory.
  const auto offset = folly::Random::rand64(0, validMaxOffset);
  const auto* memory =
      reinterpret_cast<const uint8_t*>(slabMemoryStart_) + offset;

  const auto* slab = getSlabForMemory(memory);
  const auto* header = getSlabHeader(slab);
  if (header == nullptr) {
    return std::make_tuple(0, nullptr);
  }

  XDCHECK_GE(reinterpret_cast<uintptr_t>(memory),
             reinterpret_cast<uintptr_t>(slab));

  // getRandomAlloc() is best-effort sampling. Slab headers can concurrently
  // transition between allocation classes while slab release/rebalance is in
  // progress; callers validate the sampled item before using it. A racing
  // allocSize read may at worst produce an invalid sample, which is reported as
  // nullptr below or rejected by the caller's lookup validation.
  folly::annotate_ignore_thread_sanitizer_guard g(__FILE__, __LINE__);
  const auto allocSize = header->getAllocSize();
  if (allocSize == 0) {
    return std::make_tuple(0, nullptr);
  }

  const auto maxAllocIdx = Slab::kSize / allocSize - 1;
  auto allocIdx = (reinterpret_cast<uintptr_t>(memory) -
                   reinterpret_cast<uintptr_t>(slab)) /
                  allocSize;
  allocIdx = allocIdx > maxAllocIdx ? maxAllocIdx : allocIdx;
  return std::make_tuple(
      allocSize, reinterpret_cast<const uint8_t*>(slab) + allocSize * allocIdx);
}

serialization::SlabAllocatorObject SlabAllocator::saveState() {
  if (!isRestorable()) {
    throw std::logic_error("Can not save state when memory is mmaped");
  }

  // stop async thread that is paging in memory if it is still running.
  stopMemoryLocker();

  serialization::SlabAllocatorObject object;
  *object.memorySize() = memorySize_;
  *object.nextSlabIdx() = slabIdx(nextSlabAllocation_);
  *object.canAllocate() = canAllocate_;

  for (PoolId id = 0; id < static_cast<PoolId>(memoryPoolSize_.size()); ++id) {
    object.memoryPoolSize()[id] = memoryPoolSize_[id];
  }

  for (auto slab : freeSlabs_) {
    object.freeSlabIdxs()->push_back(slabIdx(slab));
  }
  for (auto slab : advisedSlabs_) {
    object.advisedSlabIdxs()->push_back(slabIdx(slab));
  }

  *object.slabSize() = Slab::kSize;
  *object.minAllocSize() = getMinAllocSize();
  return object;
}

// for benchmarking purposes.
const unsigned int kMarkerBits = 6;
CompressedPtr4B SlabAllocator::compressAlt(const void* ptr) const {
  if (ptr == nullptr) {
    return CompressedPtr4B{};
  }

  ptrdiff_t delta = reinterpret_cast<const uint8_t*>(ptr) -
                    reinterpret_cast<const uint8_t*>(slabMemoryStart_);
  return CompressedPtr4B{
      static_cast<CompressedPtr4B::PtrType>(delta >> kMarkerBits)};
}

void* SlabAllocator::unCompressAlt(const CompressedPtr4B cPtr) const {
  if (cPtr.isNull()) {
    return nullptr;
  }

  const auto markerOffset = cPtr.getRaw() << kMarkerBits;
  const void* markerPtr =
      reinterpret_cast<const uint8_t*>(slabMemoryStart_) + markerOffset;

  const auto* header = getSlabHeader(markerPtr);
  const auto allocSize = header->getAllocSize();

  XDCHECK_GE(allocSize, 1u << kMarkerBits);

  auto slab = getSlabForMemory(markerPtr);

  auto slabOffset = reinterpret_cast<uintptr_t>(markerPtr) -
                    reinterpret_cast<uintptr_t>(slab);
  XDCHECK_LT(slabOffset, Slab::kSize);
  /*
   * Since the marker is to the left of the desired allocation, now
   * we want to find the alloc boundary to the right of this marker.
   * But we start off by finding the distance to the alloc
   * boundary on our left, which we call delta.
   * Then the distance to the right is allocSize - delta:
   *
   *      I                   M                       I
   *      <-- delta ---------><-- allocSize - delta -->
   *
   * Since allocs start at the beginning of the slab, and are all allocSize
   * bytes big, delta is just slabOffset % allocSize.  If delta is 0, then the
   * marker is already at an alloc boundary.
   */
  const auto delta = slabOffset % allocSize;
  if (delta) {
    slabOffset += (allocSize - delta);
  }
  return slab->memoryAtOffset(slabOffset);
}

void SlabAllocator::excludeMemoryFromCoredump(const PageSize& pageSize) const {
  // dump the headers always. Very useful for debugging when we have
  // pointers and need to find information. slab headers are only few slabs
  // and in the order of 4-8MB.
  const auto* memStartPtr = reinterpret_cast<const uint8_t*>(memoryStart_);
  void* slabMemStartPtr = slabMemoryStart_;
  const size_t headerBytes =
      reinterpret_cast<const uint8_t*>(slabMemoryStart_) - memStartPtr;
  size_t slabBytes = memorySize_ - headerBytes;

  if (pageSize.isHugePage()) {
    // MADV_DONTDUMP changes VMA flags, and HugeTLB VMAs can only be split on
    // huge-page boundaries. Keep only the complete huge pages in the range.
    const size_t hugePageSize = pageSize.getPageSize();
    if (!util::align(hugePageSize, 0, slabMemStartPtr, slabBytes)) {
      return;
    }
    slabBytes = util::getAlignedSizeDown(slabBytes, hugePageSize);
  } else {
    // Linux requires an aligned address but rounds the length up internally.
    XDCHECK(pageSize.isPageAlignedAddr(slabMemStartPtr));
  }

  if (slabBytes == 0) {
    return;
  }

  if (madvise(slabMemStartPtr, slabBytes, MADV_DONTDUMP)) {
    throw std::system_error(errno, std::system_category(),
                            "madvise failed to exclude memory from coredump");
  }
}
