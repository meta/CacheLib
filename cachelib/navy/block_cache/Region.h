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

#include <folly/fibers/TimedMutex.h>

#include <vector>

#include "cachelib/common/ConditionVariable.h"
#include "cachelib/common/Profiled.h"
#include "cachelib/navy/block_cache/Types.h"
#include "cachelib/navy/common/Types.h"
#include "cachelib/navy/serialization/Serialization.h"

namespace facebook {
namespace cachelib {
namespace navy {
using folly::fibers::TimedMutex;

enum class OpenMode : uint8_t {
  // Not opened
  None,

  // Write Mode, caller always uses this mode for writing
  Write,

  // Read Mode, caller always uses this mode for reading
  Read,
};

enum class OpenStatus : uint8_t {
  // Caller can proceed
  Ready,

  // Sequence number mismatch
  Retry,

  // Error (like too large, out of regions)
  Error,
};

class RegionDescriptor;

// Region. Responsible for open/lock synchronization and keeps count of active
// readers/writers.
class Region {
 public:
  // @param id         unique id for the region
  // @param regionSize region size
  Region(RegionId id, uint64_t regionSize)
      : regionId_{id}, regionSize_{regionSize} {}

  // @param d          previously serialized state of Region.
  // @param regionSize size of the region.
  Region(const serialization::Region& d, uint64_t regionSize)
      : regionId_{static_cast<uint32_t>(*d.regionId())},
        regionSize_{regionSize},
        priority_{static_cast<uint16_t>(*d.priority())},
        lastEntryEndOffset_{static_cast<uint32_t>(*d.lastEntryEndOffset())},
        numItems_{static_cast<uint32_t>(*d.numItems())} {}

  // Disable copy constructor to avoid mistakes like below:
  //   auto r = RegionManager.getRegion(rid);
  // Did you notice that we forgot reference "&"?
  Region(const Region&) = delete;
  Region& operator=(const Region&) = delete;

  // Immediately block future accesses to this region and (optionally)
  // wait on condition variable for all active readers and writers to be
  // released. Return true if there are no pending operation to this region,
  // false otherwise.
  // @param wait  whether to wait or not (only for test cases)
  bool readyForReclaim(bool wait, bool allowRead);

  // Wait for all the active readers finished.
  // When we want to reset the region after reclaim, we need to make sure no
  // active readers are reading from the region
  void waitForActiveReaders();

  // Opens this region for write and allocate a slot of @size.
  // Fail if there's insufficient space.
  // Returns a MutableBufferView into the attached buffer covering the
  // allocated range.  The view is empty when allocation fails.
  // A buffer must be attached before calling this method.
  std::tuple<RegionDescriptor, RelAddress, MutableBufferView> openAndAllocate(
      uint32_t size);

  // Opens this region for reading. Fail if region is blocked.
  RegionDescriptor openForRead();

  // Resets the region's internal state. This is used to reset state
  // after a region has been reclaimed.
  void reset();

  // Closes the region and consume the region descriptor.
  void close(RegionDescriptor&& desc);

  // Assigns this region a priority. The meaning of priority
  // is dependent on the eviction policy we choose.
  void setPriority(uint16_t priority) {
    std::lock_guard l{lock_};
    priority_ = priority;
  }

  // Gets the priority this region is assigned.
  uint16_t getPriority() const {
    std::lock_guard l{lock_};
    return priority_;
  }

  // Gets the end offset of last slot added to this region.
  uint32_t getLastEntryEndOffset() const {
    std::lock_guard l{lock_};
    return lastEntryEndOffset_;
  }

  // Gets the number of items in this region.
  uint32_t getNumItems() const {
    std::lock_guard l{lock_};
    return numItems_;
  }

  // If this region is actively used, then the fragmentation
  // is the bytes at the end of the region that's not used.
  uint32_t getFragmentationSize() const {
    std::lock_guard l{lock_};
    if (numItems_) {
      return regionSize_ - lastEntryEndOffset_;
    }
    return 0;
  }

  // Reads from attached buffer from 'fromOffset' into 'outBuf'.
  void readFromBuffer(uint32_t fromOffset, MutableBufferView outBuf) const;

  // Attaches buffer 'buf' to the region.
  void attachBuffer(std::unique_ptr<Buffer>&& buf) {
    std::lock_guard l{lock_};
    XDCHECK_EQ(buffer_, nullptr);
    buffer_ = std::move(buf);
  }

  // Checks if the region has buffer attached.
  bool hasBuffer() const {
    std::lock_guard l{lock_};
    return buffer_.get() != nullptr;
  }

  // Detaches the attached buffer and returns it.
  // This could block if there are active readers
  std::unique_ptr<Buffer> detachBuffer();

  // A value whose checksum BlockCache deferred to flush time (small values
  // under the DSA per-op gate are batched there). Offsets are relative to
  // the region buffer; csOffset is the EntryDesc::cs field to patch.
  struct PendingChecksum {
    uint32_t valueOffset;
    uint32_t valueSize;
    uint32_t csOffset;
  };
  void addPendingChecksum(PendingChecksum p);
  std::vector<PendingChecksum> takePendingChecksums();
  // Flush-path access to the attached buffer: a read-only view, and a 32-bit
  // store used to patch deferred checksums (serialized with readFromBuffer).
  BufferView bufferView() const;
  void storeU32(uint32_t offset, uint32_t value);

  // Flushes the attached buffer by calling the callBack function.
  // The callBack function is expected to write to the underlying device.
  // The callback function should return true if successfully flushed the
  // buffer, otherwise it should return false.
  enum FlushRes {
    kSuccess,
    kRetryDeviceFailure,
    kRetryPendingWrites,
  };
  FlushRes flushBuffer(std::function<bool(RelAddress, BufferView)> callBack);

  // Cleans up the attached buffer by calling the callBack function.
  void cleanupBuffer(std::function<void(RegionId, BufferView)> callBack);

  // Marks the bit to indicate pending flush status.
  void setPendingFlush() {
    std::lock_guard l{lock_};
    XDCHECK_NE(buffer_, nullptr);
    XDCHECK((flags_ & (kFlushPending | kFlushed)) == 0);

    flags_ |= kFlushPending;
  }

  // Checks if the region's buffer is flushed.
  bool isFlushedLocked() const { return (flags_ & kFlushed) != 0; }

  // Checks whether the region's buffer is cleaned up.
  bool isCleanedupLocked() const { return (flags_ & kCleanedup) != 0; }

  // Check if this region is being reclaimed
  bool isBeingReclaimed() const {
    std::lock_guard l{lock_};
    return (flags_ & kBeingReclaimed) != 0;
  }

  // Returns the number of active writers using the region.
  uint32_t getActiveWriters() const {
    std::lock_guard l{lock_};
    return activeWriters_;
  }

  // Returns the number of active readers using the region.
  uint32_t getActiveInMemReaders() const {
    std::lock_guard l{lock_};
    return activeInMemReaders_;
  }

  // Returns the region id.
  RegionId id() const { return regionId_; }

 private:
  uint32_t activeOpenLocked(bool writersOnly) const;

  // Checks to see if there is enough space in the region for a new write of
  // size 'size'.
  bool canAllocateLocked(uint32_t size) const {
    // assert that buffer is not flushed and flush is not pending
    XDCHECK((flags_ & (kFlushPending | kFlushed)) == 0);
    return (lastEntryEndOffset_ + size <= regionSize_);
  }

  RelAddress allocateLocked(uint32_t size);

  static constexpr uint32_t kBlockAccess{1u << 0};
  static constexpr uint16_t kPinned{1u << 1};
  static constexpr uint16_t kFlushPending{1u << 2};
  static constexpr uint16_t kFlushed{1u << 3};
  static constexpr uint16_t kCleanedup{1u << 4};
  static constexpr uint16_t kBeingReclaimed{1u << 5};

  const RegionId regionId_{};
  const uint64_t regionSize_{0};

  uint16_t priority_{0};
  uint16_t flags_{0};
  uint32_t activePhysReaders_{0};
  uint32_t activeInMemReaders_{0};
  uint32_t activeWriters_{0};
  // End offset of last slot added to region
  uint32_t lastEntryEndOffset_{0};
  uint32_t numItems_{0};
  std::unique_ptr<Buffer> buffer_{nullptr};
  std::vector<PendingChecksum> pendingChecksums_;

  mutable trace::Profiled<TimedMutex, "cachelib:navy:bc_region"> lock_{
      TimedMutex::Options(false)};
  mutable util::ConditionVariable cond_;
};

// RegionDescriptor. Contains status of the open, region id and the open mode.
// This is returned when a open is done in read/write mode. close() uses the
// descriptor to properly close and update the internal counters.
class RegionDescriptor {
 public:
  RegionDescriptor() = default;
  // @param status  status of the open
  RegionDescriptor(OpenStatus status) : status_(status) {}
  static RegionDescriptor makeWriteDescriptor(OpenStatus status,
                                              RegionId regionId) {
    return RegionDescriptor{status, regionId, OpenMode::Write};
  }
  static RegionDescriptor makeReadDescriptor(OpenStatus status,
                                             RegionId regionId,
                                             bool physReadMode) {
    return RegionDescriptor{status, regionId, OpenMode::Read, physReadMode};
  }
  RegionDescriptor(const RegionDescriptor&) = delete;
  RegionDescriptor& operator=(const RegionDescriptor&) = delete;

  RegionDescriptor(RegionDescriptor&& o) noexcept
      : status_{o.status_},
        mode_{o.mode_},
        physReadMode_{o.physReadMode_},
        regionId_{o.regionId_} {
    o.mode_ = OpenMode::None;
    o.regionId_ = RegionId{};
    o.status_ = OpenStatus::Retry;
    o.physReadMode_ = false;
  }

  RegionDescriptor& operator=(RegionDescriptor&& o) noexcept {
    if (this != &o) {
      this->~RegionDescriptor();
      new (this) RegionDescriptor(std::move(o));
    }
    return *this;
  }

  // Checks whether the current open mode is Physical Read Mode.
  bool isPhysReadMode() const {
    return (mode_ == OpenMode::Read) && physReadMode_;
  }

  // Checks whether status of the open is Ready.
  bool isReady() const { return status_ == OpenStatus::Ready; }

  // Returns the current open mode.
  OpenMode mode() const { return mode_; }

  // Returns the current open status.
  OpenStatus status() const { return status_; }

  // Returns the unique region ID.
  RegionId id() const { return regionId_; }

 private:
  RegionDescriptor(OpenStatus status,
                   RegionId regionId,
                   OpenMode mode,
                   bool physReadMode = false)
      : status_(status),
        mode_(mode),
        physReadMode_(physReadMode),
        regionId_(regionId) {}

  OpenStatus status_{OpenStatus::Error};
  OpenMode mode_{OpenMode::None};
  // physReadMode_ is applicable only in read mode
  bool physReadMode_{false};
  RegionId regionId_{};
};
} // namespace navy
} // namespace cachelib
} // namespace facebook
