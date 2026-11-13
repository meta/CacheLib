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

#include <folly/File.h>
#include <folly/container/F14Map.h>
#include <xxhash.h>

#include <fstream>
#include <memory>
#include <string>

#include "cachelib/common/Utils.h"
#include "cachelib/shm/Shm.h"

namespace facebook {
namespace cachelib {
// Manages a list of shared memory segments associated with a given directory.
// If the directory is not present, one will be created and all segments will
// be created new from then on. If the directory exists and the metadata file
// exists in the directory, then segments can be attempted to be attached.
// Segments can be created new at any point of time as long as one by the same
// name is not already attached. Upon successful shutDown, the segments can be
// reattached by specifying the same directory for the next instance. If the
// directory is cleared after the segments are created, the segments are
// destroyed and cannot be reattached. The role of this class is also to take
// care of the key generation for the segments by using the segment's name.
// Once the segments are created, they can be mapped/attached by using
// the ShmSegment's api. Once attached successfully to a previous set of
// segments, new segments can also be created. This class is not thread safe.
// In general, the approach is to return an invalid value if there is an error
// that is possible to foresee and throw an exception if state of the instance
// could be corrupted
class ShmManager {
 public:
  ShmManager(const std::string& dirName,
             bool usePosix,
             const std::string& hugePageMountDir = "",
             const std::string& posixShmDir = "");

  ShmManager(const ShmManager&) = delete;
  ShmManager& operator=(const ShmManager&) = delete;

  ~ShmManager() {
    try {
      removeAllSegments();
    } catch (const std::system_error&) {
    }
  }

  // Create a new shared memory segment
  //
  // @param shmName   name of the segment. It will be appended after the prefix
  // @param size      size of the segment. It must be page-aligned.
  // @param addr      starting address that this segment should be mapped to
  //                  if nullptr, the segment will be mapped to a random
  //                  address chosen by the kernel. If an address collides with
  //                  an existing mapping, the mapping will fail.
  //
  // @return ShmAddr for the segment
  //
  // @throw   std::invalid_argument if unable to create or map shared memory
  ShmAddr createShm(const std::string& shmName,
                    size_t size,
                    void* addr = nullptr,
                    ShmSegmentOpts opts = {});

  // Attach to an existing shared memory segment
  // The segment is already mapped, its address will be returned.
  //
  // @param shmName   name of the segment. It will be appended after the prefix
  // @param addr      starting address that this segment should be mapped to
  //                  if nullptr, the segment will be mapped to a random
  //                  address chosen by the kernel
  // @return ShmAddr for shared memory segment
  //
  // @throw   std::invalid_argument if unable to attach or map shared memory
  ShmAddr attachShm(const std::string& shmName,
                    void* addr = nullptr,
                    ShmSegmentOpts opts = {});

  // Remove a shared memory segment. If the memory segment is currently mapped,
  // this will unmap the segment and then remove it. If the segment by name
  // exists, but is not attached, we remove it as well.
  //
  // @param shmName   name of the segment
  // @return  true if such a segment existed and we removed it.
  //          false if segment never existed
  bool removeShm(const std::string& segName);

  // gets a current segment by the name that is managed by this
  // instance. The lifetime of the returned object is same as the
  // lifetime of this instance.
  // @param name  Name of the segment
  // @return If a segment of that name, managed by this instance exists,
  //         it is returned. Otherwise, it throws std::invalid_argument
  ShmSegment& getShmByName(const std::string& shmName);

  enum class ShutDownRes { kSuccess = 0, kFileDeleted, kFailedWrite };

  // persists the metadata information for the current segments managed
  // by this instance. On a successful shutdown, the segments can be
  // reattached to on restart. There is no guarantee that they will be
  // since the segments could be deleted outside of this instance.
  // Doing anything with this instance after calling shutdown is
  // undefinied. Clearing the contents of the cache dir while the instance is
  // active will cause shutdown to fail and drop all the existing segments.
  // Only segments that were attached to this instance will be persisted on
  // ShutDown. The unattached segments will be destroyed on shutdown.
  //
  // @return returns ShutDownRes enum corresponding to result of shutdown
  ShutDownRes shutDown();

  // useful for removing segments by name associated with a given
  // cacheDir without instanciating.
  static void removeByName(const std::string& cacheDir,
                           const std::string& segName,
                           bool posix,
                           const std::string& hugePageMountDir = "",
                           const std::string& posixShmDir = "");

  // Useful for checking whether a segment exists by name associated with a
  // given cacheDir without instanciating. This should be ONLY used in tests.
  static bool segmentExists(const std::string& cacheDir,
                            const std::string& segName,
                            bool posix);

  // free up and remove all the segments related to the cache directory.
  static void cleanup(const std::string& cacheDir,
                      bool posix,
                      const std::string& hugePageMountDir = "",
                      const std::string& posixShmDir = "");

  // expose a read only instance of the shm for use where we want to peek at a
  // particular segment without owning its lifetime or any guarantees.
  //
  // @param cacheDir   the cache directory identifying the original shm
  // @param segName    the name of the original segment
  // @param addr       starting address that this segment should be mapped to
  //                   if nullptr, the segment will be mapped to a random
  //                   address chosen by the kernel
  // @param pageSize   page size backing the segment
  // @param hugePageMountDir hugetlbfs mount containing a POSIX huge-page
  //                         segment
  // @param posixShmDir      directory containing a POSIX normal-page segment;
  //                         empty means /dev/shm
  //
  // @return a unique_ptr to the shm if one is present with the name.
  static std::unique_ptr<ShmSegment> attachShmReadOnly(
      const std::string& cacheDir,
      const std::string& segName,
      bool posix,
      void* addr = nullptr,
      const PageSize& pageSize = PageSize(),
      const std::string& hugePageMountDir = "",
      const std::string& posixShmDir = "");

 private:
  enum class ShmVal : int8_t { SHM_SYS_V = 1, SHM_POSIX, SHM_INVALID };

  // if metadata file exists, truncates it. If it does not exist, creates an
  // empty one. Throws system_error if an error occurred.
  void createEmptyMetadataFile(const std::string& file) {
    if (metadataStream_) {
      metadataStream_.close();
    }
    metadataStream_.open(file, std::ios::trunc);
    if (metadataStream_.good() && metadataStream_.flush() &&
        metadataStream_.good()) {
      return;
    }
    util::throwSystemError(EIO, "Cannot create file");
  }

  // Open the metadata file in read-only mode and lock it for exclusive
  // access. This is used to make sure two processes do not accidentally
  // use the same cache directory
  void lockMetadataFile(const std::string& file) {
    metaDataLockFile_ = folly::File(file.c_str(), O_RDONLY);
    if (!metaDataLockFile_.try_lock()) {
      util::throwSystemError(EBUSY, "Can not lock shm metadata file");
    }
  }

  // initialize the state from a serialized state.
  // we first attempt to read from V2, if that fails, we try to read from V1.
  bool initFromFile();

  // write active segments into the metadata file so that we can reattach in
  // another instance.
  ShutDownRes writeActiveSegmentsToFile();

  // remove all the segments that this directory manages.
  void removeAllSegments();

  // remove all the segments we manage that were never attached or actively
  // used.
  void removeUnAttachedSegments();

  // removes a segment that has not been attached. If the segment does not
  // exist, does nothing.  propogates any other system_error.
  //
  // @return  true if the segment by this key was present, false if it never
  //          existed.
  bool removeUnattached(const std::string& shmName);

  folly::F14FastMap<std::string, std::unique_ptr<ShmSegment>>::iterator
  attachNewShm(const std::string& name, const ShmSegmentOpts& opts);

  std::string uniqueIdForName(const std::string& name) const {
    return name + dirHash_;
  }

  std::string controlDir_{};
  std::string dirHash_{};

  // current segment being managed by this instance
  folly::F14FastMap<std::string, std::unique_ptr<ShmSegment>> segments_{};

  // name to key mapping used for reattaching. This is persisted to a
  // file and used for attaching to the segment.
  folly::F14FastMap<std::string, std::string> nameToKey_{};

  // Single hugetlbfs mount backing this cache's huge-page POSIX segments (all
  // huge segments of a cache share one mount). Used to locate segments for
  // removal when no ShmSegmentOpts are available. Empty if none use huge pages.
  const std::string hugePageMountDir_{};

  // Directory of normal-page POSIX segments; empty means /dev/shm.
  const std::string posixShmDir_{};

  const std::string& segmentDir(const ShmSegmentOpts& opts) const {
    return opts.pageSize.isHugePage() ? hugePageMountDir_ : posixShmDir_;
  }

  // file handle for the metadata file. It remains open throughout the lifetime
  // of the object.
  std::ofstream metadataStream_;

  // folly::File for locking metadata file. The file is opened and locked
  // throughout the lifetime of the object.
  folly::File metaDataLockFile_;

  const bool usePosix_{false};

  // this file is used to persist the name to key mapping so that we can
  // reattach from another instance. The file exists inside controlDir_
  static constexpr const char* const kMetaDataFile = "metadata";
  static constexpr const char* const kDropFile = "ColdRoll";
};

} // namespace cachelib
} // namespace facebook
