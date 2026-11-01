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

#include "cachelib/navy/bighash/Bucket.h"

#include <folly/Random.h>

#include "cachelib/navy/common/ChecksumOffload.h"
#include "cachelib/navy/common/Hash.h"

namespace facebook::cachelib::navy {
static_assert(sizeof(Bucket) == 24,
              "Bucket overhead. If this changes, you may have to adjust the "
              "sizes used in unit tests.");

namespace {
const details::BucketEntry* getIteratorEntry(BucketStorage::Allocation itr) {
  return reinterpret_cast<const details::BucketEntry*>(itr.view().data());
}
} // namespace

BufferView Bucket::Iterator::key() const {
  return getIteratorEntry(itr_)->key();
}

uint64_t Bucket::Iterator::keyHash() const {
  return getIteratorEntry(itr_)->keyHash();
}

BufferView Bucket::Iterator::value() const {
  return getIteratorEntry(itr_)->value();
}

bool Bucket::Iterator::keyEqualsTo(HashedKey hk) const {
  return getIteratorEntry(itr_)->keyEqualsTo(hk);
}

uint32_t Bucket::computeChecksum(BufferView view) {
  constexpr auto kChecksumStart = sizeof(checksum_);
  auto data = view.slice(kChecksumStart, view.size() - kChecksumStart);
  return navy::checksum(data);
}

uint32_t Bucket::computeChecksumOffloaded(BufferView view,
                                          folly::FunctionRef<void()> overlap) {
  constexpr auto kChecksumStart = sizeof(checksum_);
  auto data = view.slice(kChecksumStart, view.size() - kChecksumStart);
  return checksumWithOverlap(data, overlap);
}

Bucket& Bucket::initNew(MutableBufferView view, uint64_t generationTime) {
  return *new (view.data())
      Bucket(generationTime, view.size() - sizeof(Bucket));
}

BufferView Bucket::find(HashedKey hk) const {
  auto itr = storage_.getFirst();
  while (!itr.done()) {
    auto* entry = getIteratorEntry(itr);
    if (entry->keyEqualsTo(hk)) {
      return entry->value();
    }
    itr = storage_.getNext(itr);
  }
  return {};
}

std::pair<uint32_t, uint32_t> Bucket::insert(
    HashedKey hk,
    BufferView value,
    const ExpiredCheck& checkExpired,
    const DestructorCallback& destructorCb) {
  const auto size =
      details::BucketEntry::computeSize(hk.key().size(), value.size());
  XDCHECK_LE(size, storage_.capacity());

  auto ret = makeSpace(size, checkExpired, destructorCb);
  auto alloc = storage_.allocate(size);
  XDCHECK(!alloc.done());
  details::BucketEntry::create(alloc.view(), hk, value);

  return ret;
}

std::pair<uint32_t, uint32_t> Bucket::makeSpace(
    uint32_t size,
    const ExpiredCheck& checkExpired,
    const DestructorCallback& destructorCb) {
  const auto requiredSize = BucketStorage::slotSize(size);
  XDCHECK_LE(requiredSize, storage_.capacity());

  if (storage_.remainingCapacity() >= requiredSize) {
    return {};
  }

  uint32_t evictionExpired =
      removeExpired(storage_.getFirst(), checkExpired, destructorCb);
  uint32_t evictions = evictionExpired;
  // Check available space again after evictions
  auto curFreeSpace = storage_.remainingCapacity();
  if (evictionExpired > 0 && curFreeSpace >= requiredSize) {
    return std::make_pair(evictions, evictionExpired);
  }

  auto itr = storage_.getFirst();
  while (true) {
    evictions++;

    if (destructorCb) {
      auto* entry = getIteratorEntry(itr);
      destructorCb(entry->hashedKey(), entry->value(),
                   DestructorEvent::Recycled);
    }

    curFreeSpace += BucketStorage::slotSize(itr.view().size());
    if (curFreeSpace >= requiredSize) {
      storage_.removeUntil(itr);
      break;
    }
    itr = storage_.getNext(itr);
    XDCHECK(!itr.done());
  }
  return std::make_pair(evictions, evictionExpired);
}

uint32_t Bucket::removeExpired(BucketStorage::Allocation itr,
                               const ExpiredCheck& checkExpired,
                               const DestructorCallback& destructorCb) {
  if (!checkExpired) {
    return 0;
  }

  uint32_t evictions = 0;
  std::vector<BucketStorage::Allocation> removed;
  while (!itr.done()) {
    auto* entry = getIteratorEntry(itr);
    if (!checkExpired(entry->value())) {
      itr = storage_.getNext(itr);
      continue;
    }

    // Remove expired entry
    if (destructorCb) {
      destructorCb(entry->hashedKey(), entry->value(),
                   DestructorEvent::Recycled);
    }
    removed.emplace_back(itr);
    itr = storage_.getNext(itr);
    evictions++;
  }
  storage_.remove(removed);
  return evictions;
}

uint32_t Bucket::remove(HashedKey hk, const DestructorCallback& destructorCb) {
  auto itr = storage_.getFirst();
  while (!itr.done()) {
    auto* entry = getIteratorEntry(itr);
    if (entry->keyEqualsTo(hk)) {
      if (destructorCb) {
        destructorCb(entry->hashedKey(), entry->value(),
                     DestructorEvent::Removed);
      }
      storage_.remove(itr);
      return 1;
    }
    itr = storage_.getNext(itr);
  }
  return 0;
}

std::pair<std::string, BufferView> Bucket::getRandomAlloc() {
  const auto randOffset = folly::Random::rand64(0, storage_.capacity());

  auto itr = storage_.getFirst();
  while (!itr.done()) {
    auto offset = storage_.getOffset(itr);
    auto size = itr.view().size();
    if (randOffset < offset + size) {
      auto* entry = getIteratorEntry(itr);
      return std::make_pair(reinterpret_cast<const char*>(entry->key().data()),
                            entry->value());
    }
    itr = storage_.getNext(itr);
  }
  return {};
}

Bucket::Iterator Bucket::getFirst() const {
  return Iterator{storage_.getFirst()};
}

Bucket::Iterator Bucket::getNext(Iterator itr) const {
  return Iterator{storage_.getNext(itr.itr_)};
}
} // namespace facebook::cachelib::navy
