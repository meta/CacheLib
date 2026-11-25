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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include "cachelib/allocator/serialize/gen-cpp2/objects_types.h"
#pragma GCC diagnostic pop

#include "cachelib/common/CompilerUtils.h"

namespace facebook::cachelib {
// node information for the double linked list modelling the lru. It has the
// previous, next information with the last time the item was updated in the
// LRU.
template <typename T>
struct CACHELIB_PACKED_ATTR DListHook {
  using Time = uint32_t;
  using CompressedPtrType = typename T::CompressedPtrType;
  using PtrCompressor = typename T::PtrCompressor;

  void setNext(T* const n, const PtrCompressor& compressor) noexcept {
    next_ = compressor.compress(n);
  }

  void setNext(CompressedPtrType next) noexcept { next_ = next; }

  void setPrev(T* const p, const PtrCompressor& compressor) noexcept {
    prev_ = compressor.compress(p);
  }

  void setPrev(CompressedPtrType prev) noexcept { prev_ = prev; }

  CompressedPtrType getNext() const noexcept {
    return CompressedPtrType(next_);
  }

  T* getNext(const PtrCompressor& compressor) const noexcept {
    return compressor.unCompress(next_);
  }

  CompressedPtrType getPrev() const noexcept {
    return CompressedPtrType(prev_);
  }

  T* getPrev(const PtrCompressor& compressor) const noexcept {
    return compressor.unCompress(prev_);
  }

  // set and get the time when the node was updated in the lru.
  void setUpdateTime(Time time) noexcept { updateTime_ = time; }

  Time getUpdateTime() const noexcept {
    // Suppress TSAN here because we don't care if an item is promoted twice by
    // two get operations running concurrently. It should be very rarely and is
    // just a minor inefficiency if it happens.
    folly::annotate_ignore_thread_sanitizer_guard g(__FILE__, __LINE__);
    return updateTime_;
  }

 private:
  CompressedPtrType next_{}; // next node in the linked list
  CompressedPtrType prev_{}; // previous node in the linked list
  // timestamp when this was last updated to the head of the list
  Time updateTime_{0};
};

// uses a double linked list to implement an LRU. T must be have a public
// member of type Hook and HookPtr must point to that.
template <typename T, DListHook<T> T::* HookPtr>
class DList {
 public:
  using CompressedPtrType = typename T::CompressedPtrType;
  using PtrCompressor = typename T::PtrCompressor;
  using DListObject = serialization::DListObject;

  DList() = default;
  DList(const DList&) = delete;
  DList& operator=(const DList&) = delete;

  explicit DList(PtrCompressor compressor) noexcept
      : compressor_(std::move(compressor)) {}

  // Restore DList from saved state.
  //
  // @param object              Save DList object
  // @param compressor          PtrCompressor object
  DList(const DListObject& object, PtrCompressor compressor)
      : compressor_(std::move(compressor)),
        head_(compressor_.unCompress(
            CompressedPtrType{*object.compressedHead()})),
        tail_(compressor_.unCompress(
            CompressedPtrType{*object.compressedTail()})),
        size_(*object.size()) {}

  /**
   * Exports the current state as a thrift object for later restoration.
   */
  DListObject saveState() const {
    DListObject state;
    *state.compressedHead() = compressor_.compress(head_).saveState();
    *state.compressedTail() = compressor_.compress(tail_).saveState();
    *state.size() = size_;
    return state;
  }

  T* getNext(const T& node) const noexcept {
    return (node.*HookPtr).getNext(compressor_);
  }

  T* getPrev(const T& node) const noexcept {
    return (node.*HookPtr).getPrev(compressor_);
  }

  void setNext(T& node, T* next) noexcept {
    (node.*HookPtr).setNext(next, compressor_);
  }

  void setNextFrom(T& node, const T& other) noexcept {
    (node.*HookPtr).setNext((other.*HookPtr).getNext());
  }

  void setPrev(T& node, T* prev) noexcept {
    (node.*HookPtr).setPrev(prev, compressor_);
  }

  void setPrevFrom(T& node, const T& other) noexcept {
    (node.*HookPtr).setPrev((other.*HookPtr).getPrev());
  }

  // Links the passed node to the head of the double linked list
  // @param node node to be linked at the head
  void linkAtHead(T& node) noexcept;

  // Links the passed node to the tail of the double linked list
  // @param node node to be linked at the tail
  void linkAtTail(T& node) noexcept;

  // Add node before nextNode.
  //
  // @param nextNode    node before which to insert
  // @param node        node to insert
  // @note nextNode must be in the list and node must not be in the list
  void insertBefore(T& nextNode, T& node) noexcept;

  // removes the node completely from the linked list and cleans up the node
  // appropriately by setting its next and prev as nullptr.
  void remove(T& node) noexcept;

  // Unlinks the destination node and replaces it with the source node
  //
  // @param oldNode   destination node
  // @param newNode   source node
  void replace(T& oldNode, T& newNode) noexcept;

  // moves a node that belongs to the linked list to the head of the linked
  // list.
  void moveToHead(T& node) noexcept;

  // Splices the contiguous suffix [first, ..., tail_] to the head of the
  // list in O(1). After the call, `first` is the new head and the node
  // previously preceding `first` becomes the new tail.
  // @param first   the leftmost node of the suffix to move; must be in the list
  void moveSuffixToHead(T& first) noexcept;

  T* getHead() const noexcept { return head_; }
  T* getTail() const noexcept { return tail_; }

  size_t size() const noexcept { return size_; }

  // Iterator interface for the double linked list. Supports both iterating
  // from the tail and head.
  class Iterator {
   public:
    enum class Direction { FROM_HEAD, FROM_TAIL };

    Iterator(T* p, Direction d, const DList<T, HookPtr>& dlist) noexcept
        : curr_(p), dir_(d), dlist_(&dlist) {}
    virtual ~Iterator() = default;

    // copyable and movable
    Iterator(const Iterator&) = default;
    Iterator& operator=(const Iterator&) = default;
    Iterator(Iterator&&) noexcept = default;
    Iterator& operator=(Iterator&&) noexcept = default;

    // moves the iterator forward and backward. Calling ++ once the iterator
    // has reached the end is undefined.
    Iterator& operator++() noexcept;
    Iterator& operator--() noexcept;

    T* operator->() const noexcept { return curr_; }
    T& operator*() const noexcept { return *curr_; }

    bool operator==(const Iterator& other) const noexcept {
      return dlist_ == other.dlist_ && curr_ == other.curr_ &&
             dir_ == other.dir_;
    }

    bool operator!=(const Iterator& other) const noexcept {
      return !(*this == other);
    }

    explicit operator bool() const noexcept {
      return curr_ != nullptr && dlist_ != nullptr;
    }

    T* get() const noexcept { return curr_; }

    // Invalidates this iterator
    void reset() noexcept { curr_ = nullptr; }

    // Reset the iterator back to the beginning
    void resetToBegin() noexcept {
      curr_ = dir_ == Direction::FROM_HEAD ? dlist_->head_ : dlist_->tail_;
    }

   protected:
    void goForward() noexcept;
    void goBackward() noexcept;

    // the current position of the iterator in the list
    T* curr_{nullptr};
    // the direction we are iterating.
    Direction dir_{Direction::FROM_HEAD};
    const DList<T, HookPtr>* dlist_{nullptr};
  };

  // provides an iterator starting from the head of the linked list.
  Iterator begin() const noexcept;

  // provides an iterator starting from the tail of the linked list.
  Iterator rbegin() const noexcept;

  // Iterator to compare against for the end.
  Iterator end() const noexcept;
  Iterator rend() const noexcept;

 private:
  // unlinks the node from the linked list. Does not correct the next and
  // previous.
  void unlink(const T& node) noexcept;

  const PtrCompressor compressor_{};

  // head of the linked list
  T* head_{nullptr};

  // tail of the linked list
  T* tail_{nullptr};

  // size of the list
  size_t size_{0};
};

/* Linked list implemenation */
template <typename T, DListHook<T> T::* HookPtr>
void DList<T, HookPtr>::linkAtHead(T& node) noexcept {
  XDCHECK_NE(reinterpret_cast<uintptr_t>(&node),
             reinterpret_cast<uintptr_t>(head_));

  setNext(node, head_);
  setPrev(node, nullptr);
  // fix the prev ptr of head
  if (head_ != nullptr) {
    setPrev(*head_, &node);
  }
  head_ = &node;
  if (tail_ == nullptr) {
    tail_ = &node;
  }
  size_++;
}

template <typename T, DListHook<T> T::* HookPtr>
void DList<T, HookPtr>::linkAtTail(T& node) noexcept {
  XDCHECK_NE(reinterpret_cast<uintptr_t>(&node),
             reinterpret_cast<uintptr_t>(tail_));

  setNext(node, nullptr);
  setPrev(node, tail_);
  // Fix the next ptr for tail
  if (tail_ != nullptr) {
    setNext(*tail_, &node);
  }
  tail_ = &node;
  if (head_ == nullptr) {
    head_ = &node;
  }
  size_++;
}

template <typename T, DListHook<T> T::* HookPtr>
void DList<T, HookPtr>::insertBefore(T& nextNode, T& node) noexcept {
  XDCHECK_NE(reinterpret_cast<uintptr_t>(&nextNode),
             reinterpret_cast<uintptr_t>(&node));
  XDCHECK(getNext(node) == nullptr);
  XDCHECK(getPrev(node) == nullptr);

  auto* const prev = getPrev(nextNode);

  XDCHECK_NE(reinterpret_cast<uintptr_t>(prev),
             reinterpret_cast<uintptr_t>(&node));

  setPrev(node, prev);
  if (prev != nullptr) {
    setNext(*prev, &node);
  } else {
    head_ = &node;
  }

  setPrev(nextNode, &node);
  setNext(node, &nextNode);
  size_++;
}

template <typename T, DListHook<T> T::* HookPtr>
void DList<T, HookPtr>::unlink(const T& node) noexcept {
  XDCHECK_GT(size_, 0u);
  // fix head_ and tail_ if the node is either of that.
  auto* const prev = getPrev(node);
  auto* const next = getNext(node);

  if (&node == head_) {
    head_ = next;
  }
  if (&node == tail_) {
    tail_ = prev;
  }

  // fix the next and prev ptrs of the node before and after us.
  if (prev != nullptr) {
    setNextFrom(*prev, node);
  }
  if (next != nullptr) {
    setPrevFrom(*next, node);
  }
  size_--;
}

template <typename T, DListHook<T> T::* HookPtr>
void DList<T, HookPtr>::remove(T& node) noexcept {
  unlink(node);
  setNext(node, nullptr);
  setPrev(node, nullptr);
}

template <typename T, DListHook<T> T::* HookPtr>
void DList<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  // Update head and tail links if needed
  if (&oldNode == head_) {
    head_ = &newNode;
  }
  if (&oldNode == tail_) {
    tail_ = &newNode;
  }

  // Make the previous and next nodes point to the new node
  auto* const prev = getPrev(oldNode);
  auto* const next = getNext(oldNode);
  if (prev != nullptr) {
    setNext(*prev, &newNode);
  }
  if (next != nullptr) {
    setPrev(*next, &newNode);
  }

  // Make the new node point to the previous and next nodes
  setPrev(newNode, prev);
  setNext(newNode, next);

  // Cleanup the old node
  setPrev(oldNode, nullptr);
  setNext(oldNode, nullptr);
}

template <typename T, DListHook<T> T::* HookPtr>
void DList<T, HookPtr>::moveToHead(T& node) noexcept {
  if (&node == head_) {
    return;
  }
  unlink(node);
  linkAtHead(node);
}

template <typename T, DListHook<T> T::*HookPtr>
void DList<T, HookPtr>::moveSuffixToHead(T& first) noexcept {
  if (&first == head_) {
    return;
  }

  T* const newTail = getPrev(first);
  XDCHECK(newTail != nullptr);

  // Detach suffix [first, ..., tail_] from the prefix.
  setNext(*newTail, nullptr);

  // Splice the suffix in front of the old head.
  setNext(*tail_, head_);
  setPrev(*head_, tail_);
  setPrev(first, nullptr);

  head_ = &first;
  tail_ = newTail;
}

/* Iterator Implementation */
template <typename T, DListHook<T> T::* HookPtr>
void DList<T, HookPtr>::Iterator::goForward() noexcept {
  if (dir_ == Direction::FROM_TAIL) {
    curr_ = dlist_->getPrev(*curr_);
  } else {
    curr_ = dlist_->getNext(*curr_);
  }
}

template <typename T, DListHook<T> T::* HookPtr>
void DList<T, HookPtr>::Iterator::goBackward() noexcept {
  if (dir_ == Direction::FROM_TAIL) {
    curr_ = dlist_->getNext(*curr_);
  } else {
    curr_ = dlist_->getPrev(*curr_);
  }
}

template <typename T, DListHook<T> T::* HookPtr>
typename DList<T, HookPtr>::Iterator&
DList<T, HookPtr>::Iterator::operator++() noexcept {
  XDCHECK(curr_ != nullptr);
  if (curr_ != nullptr) {
    goForward();
  }
  return *this;
}

template <typename T, DListHook<T> T::* HookPtr>
typename DList<T, HookPtr>::Iterator&
DList<T, HookPtr>::Iterator::operator--() noexcept {
  XDCHECK(curr_ != nullptr);
  if (curr_ != nullptr) {
    goBackward();
  }
  return *this;
}

template <typename T, DListHook<T> T::* HookPtr>
typename DList<T, HookPtr>::Iterator DList<T, HookPtr>::begin() const noexcept {
  return DList<T, HookPtr>::Iterator(head_, Iterator::Direction::FROM_HEAD,
                                     *this);
}

template <typename T, DListHook<T> T::* HookPtr>
typename DList<T, HookPtr>::Iterator DList<T, HookPtr>::rbegin()
    const noexcept {
  return DList<T, HookPtr>::Iterator(tail_, Iterator::Direction::FROM_TAIL,
                                     *this);
}

template <typename T, DListHook<T> T::* HookPtr>
typename DList<T, HookPtr>::Iterator DList<T, HookPtr>::end() const noexcept {
  return DList<T, HookPtr>::Iterator(nullptr, Iterator::Direction::FROM_HEAD,
                                     *this);
}

template <typename T, DListHook<T> T::* HookPtr>
typename DList<T, HookPtr>::Iterator DList<T, HookPtr>::rend() const noexcept {
  return DList<T, HookPtr>::Iterator(nullptr, Iterator::Direction::FROM_TAIL,
                                     *this);
}
} // namespace facebook::cachelib
