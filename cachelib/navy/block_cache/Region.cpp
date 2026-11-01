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

#include "cachelib/navy/block_cache/Region.h"

#include <utility>

#include "cachelib/navy/common/NavyThread.h"

namespace facebook::cachelib::navy {

bool Region::readyForReclaim(bool wait, bool allowRead) {
  std::unique_lock l{lock_};
  flags_ |= allowRead ? kBeingReclaimed : kBlockAccess;
  bool ready = false;
  // If we allow read during reclaim, we only need to block on active writers,
  // not readers, hence we give allowRead as a parameter to activeOpenLocked.
  while (!(ready = (activeOpenLocked(allowRead) == 0UL)) && wait) {
    cond_.wait(l);
  }

  return ready;
}

void Region::waitForActiveReaders() {
  std::unique_lock l{lock_};
  // No more reader should be allowed.
  flags_ |= kBlockAccess;
  while (activeInMemReaders_ != 0 || activePhysReaders_ != 0) {
    cond_.wait(l);
  }
}

uint32_t Region::activeOpenLocked(bool writersOnly) const {
  return writersOnly
             ? activeWriters_
             : (activePhysReaders_ + activeInMemReaders_ + activeWriters_);
}

std::tuple<RegionDescriptor, RelAddress, MutableBufferView>
Region::openAndAllocate(uint32_t size) {
  std::lock_guard l{lock_};
  XDCHECK(!((flags_ & kBlockAccess) || (flags_ & kBeingReclaimed)));
  if (!canAllocateLocked(size)) {
    return std::make_tuple(RegionDescriptor{OpenStatus::Error}, RelAddress{},
                           MutableBufferView{});
  }
  activeWriters_++;
  auto addr = allocateLocked(size);
  XDCHECK(buffer_);
  MutableBufferView view(size, buffer_->data() + addr.offset());
  return std::make_tuple(
      RegionDescriptor::makeWriteDescriptor(OpenStatus::Ready, regionId_), addr,
      view);
}

RegionDescriptor Region::openForRead() {
  std::unique_lock l{lock_};
  if (flags_ & kBlockAccess) {
    // Region is currently in reclaim, retry later
    if (isOnNavyThread()) {
      // If we are on fiber, we can just sleep here
      cond_.wait(l);
    }
    return RegionDescriptor{OpenStatus::Retry};
  }
  bool physReadMode = false;
  if (isFlushedLocked() || !buffer_) {
    physReadMode = true;
    activePhysReaders_++;
  } else {
    activeInMemReaders_++;
  }
  return RegionDescriptor::makeReadDescriptor(OpenStatus::Ready, regionId_,
                                              physReadMode);
}

std::unique_ptr<Buffer> Region::detachBuffer() {
  std::unique_lock l{lock_};
  XDCHECK_NE(buffer_, nullptr);
  while (activeInMemReaders_ != 0) {
    cond_.wait(l);
  }

  XDCHECK_EQ(activeWriters_, 0UL);
  auto retBuf = std::move(buffer_);
  buffer_ = nullptr;
  return retBuf;
}

// This function flushes the attached buffer if there are no active writers
// by calling the callBack function that is expected to write the buffer to
// underlying device. If there are active writers, the caller is expected
// to call this function again.
Region::FlushRes Region::flushBuffer(
    std::function<bool(RelAddress, BufferView)> callBack) {
  std::unique_lock lock{lock_};
  if (activeWriters_ != 0) {
    return FlushRes::kRetryPendingWrites;
  }
  if (!isFlushedLocked()) {
    lock.unlock();
    if (callBack(RelAddress{regionId_, 0}, buffer_->view())) {
      lock.lock();
      flags_ |= kFlushed;
      return FlushRes::kSuccess;
    }
    return FlushRes::kRetryDeviceFailure;
  }
  return FlushRes::kSuccess;
}

void Region::cleanupBuffer(std::function<void(RegionId, BufferView)> callBack) {
  std::unique_lock lock{lock_};
  while (activeWriters_ != 0) {
    cond_.wait(lock);
  }
  if (!isCleanedupLocked()) {
    lock.unlock();
    callBack(regionId_, buffer_->view());
    lock.lock();
    flags_ |= kCleanedup;
  }
}

void Region::reset() {
  std::lock_guard l{lock_};
  XDCHECK_EQ(activeOpenLocked(false), 0U);
  priority_ = 0;
  flags_ = 0;
  activeWriters_ = 0;
  activePhysReaders_ = 0;
  activeInMemReaders_ = 0;
  lastEntryEndOffset_ = 0;
  numItems_ = 0;
  pendingChecksums_.clear();
  cond_.notifyAll();
}

void Region::addPendingChecksum(PendingChecksum p) {
  std::lock_guard l{lock_};
  pendingChecksums_.push_back(p);
}

std::vector<Region::PendingChecksum> Region::takePendingChecksums() {
  std::lock_guard l{lock_};
  return std::exchange(pendingChecksums_, {});
}

BufferView Region::bufferView() const {
  std::lock_guard l{lock_};
  XDCHECK_NE(buffer_, nullptr);
  return buffer_->view();
}

void Region::storeU32(uint32_t offset, uint32_t value) {
  std::lock_guard l{lock_};
  XDCHECK_NE(buffer_, nullptr);
  XDCHECK_LE(offset + sizeof(value), buffer_->size());
  memcpy(buffer_->data() + offset, &value, sizeof(value));
}

void Region::close(RegionDescriptor&& desc) {
  std::lock_guard l{lock_};
  switch (desc.mode()) {
  case OpenMode::Write:
    XDCHECK_GT(activeWriters_, 0u);
    if (--activeWriters_ == 0) {
      cond_.notifyAll();
    }
    break;
  case OpenMode::Read:
    if (desc.isPhysReadMode()) {
      XDCHECK_GT(activePhysReaders_, 0u);
      if (--activePhysReaders_ == 0) {
        cond_.notifyAll();
      }
    } else {
      XDCHECK_GT(activeInMemReaders_, 0u);
      if (--activeInMemReaders_ == 0) {
        cond_.notifyAll();
      }
    }
    break;
  default:
    XDCHECK(false);
  }
}

RelAddress Region::allocateLocked(uint32_t size) {
  XDCHECK(canAllocateLocked(size));
  auto offset = lastEntryEndOffset_;
  lastEntryEndOffset_ += size;
  numItems_++;
  return RelAddress{regionId_, offset};
}

void Region::readFromBuffer(uint32_t fromOffset,
                            MutableBufferView outBuf) const {
  std::lock_guard l{lock_};
  XDCHECK_NE(buffer_, nullptr);
  XDCHECK_LE(fromOffset + outBuf.size(), buffer_->size());
  memcpy(outBuf.data(), buffer_->data() + fromOffset, outBuf.size());
}

} // namespace facebook::cachelib::navy
