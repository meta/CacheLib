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
#include <string>

#include "cachelib/shm/ShmCommon.h"

namespace facebook {
namespace cachelib {

constexpr int kInvalidFD = -1;

/* This class lets you manage a posix shared memory segment identified by
 * name. This is very similar to the System V shared memory segment, except
 * that it allows for resizing of the segments on the fly. This can let the
 * application logic to grow/shrink the shared memory segment at its end.
 * Accessing the pages truncated on shrinking will result in SIGBUS.
 *
 * Segments can be created and attached to the process's address space.
 * Segments can be marked for removal, even while they are currently attached
 * to some process's address space. Upon which, any subsequent attach fails
 * until a new segment of the same name is created. Once the last process
 * attached to the segment unmaps the memory from its address space, the
 * physical memory associated with this segment is freed.
 *
 * At any given point of time, there is only ONE unique attachable segment by
 * name, but there could exist several unattachable segments which were once
 * referenced by the same name living in process address space while all of
 * them are marked for removal.
 */

class PosixShmSegment : public ShmBase {
 public:
  // attach to an existing posix segment with the given name
  //
  // @param name  Name of the segment
  // @param opts  the options for attaching to the segment.
  // @param hugePageMountDir  directory of the segment: the hugetlbfs mount for
  //        huge pages; for normal pages "" means /dev/shm.
  PosixShmSegment(ShmAttachT,
                  const std::string& name,
                  ShmSegmentOpts opts = {},
                  const std::string& hugePageMountDir = "");

  // create a new segment
  // @param name  The name of the segment
  // @param size  The size of the segment. This will be rounded up to the
  //              nearest page size.
  // @param hugePageMountDir  directory of the segment: the hugetlbfs mount for
  //        huge pages; for normal pages "" means /dev/shm.
  PosixShmSegment(ShmNewT,
                  const std::string& name,
                  size_t size,
                  ShmSegmentOpts opts = {},
                  const std::string& hugePageMountDir = "");

  // destructor
  ~PosixShmSegment() override;

  std::string getKeyStr() const noexcept override { return getName(); }

  // marks the current segment to be removed once it is no longer mapped
  // by any process in the kernel.
  void markForRemoval() override;

  // return the current size of the segment. throws std::system_error
  // with EINVAL if the segment is invalid or  appropriate errno if the
  // segment exists but we have a bad fd or kernel is out of memory.
  size_t getSize() const override;

  // attaches the segment from the start to the address space of the
  // caller. the address must be page aligned.
  // @param addr   the start of the address for attaching.
  //
  // @return  the address where  the segment was mapped to. This will be same
  // as addr if addr is not nullptr
  // @throw std::system_error with EINVAL if the segment is not valid or
  //        address/length are not page aligned.
  void* mapAddress(void* addr) const override;

  // unmaps the memory from addr up to the given length from the
  // address space.
  void unMap(void* addr) const override;

  // useful for removing without attaching
  // @param name  Name of the segment
  // @param hugePageMountDir  directory to remove the segment from in addition
  //        to /dev/shm, if not empty.
  // @return true if the segment existed. false otherwise
  static bool removeByName(const std::string& name,
                           const std::string& hugePageMountDir = "");

 private:
  static int createNewSegment(const std::string& name,
                              const ShmSegmentOpts& opts,
                              const std::string& hugePageMountDir);
  static int getExisting(const std::string& name,
                         const ShmSegmentOpts& opts,
                         const std::string& hugePageMountDir);

  // returns the key type corresponding to the given name.
  static std::string createKeyForName(const std::string& name) noexcept;

  // resize the segment
  // @param size  the new size
  // @return none
  // @throw  Throws std::system_error with appropriate errno
  void resize(size_t size) const;

  void createReferenceMapping();
  void deleteReferenceMapping() const;

  void memBind(void* addr) const;

  // Directory of the segment file ("" for /dev/shm). Needed to locate the
  // file for removal.
  const std::string hugePageMountDir_;

  // file descriptor associated with the shm. This has FD_CLOEXEC set
  // and once opened, we close this only on destruction of this object
  int fd_{kInvalidFD};
};
} // namespace cachelib
} // namespace facebook
