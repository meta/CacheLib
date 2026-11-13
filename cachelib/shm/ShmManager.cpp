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

#include "cachelib/shm/ShmManager.h"

#include <fmt/core.h>
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>
#include <sys/stat.h>

#include <exception>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <thrift/lib/cpp2/protocol/Serializer.h>
#pragma GCC diagnostic pop

#include "cachelib/common/Utils.h"
#include "cachelib/shm/gen-cpp2/shm_types.h"

namespace facebook {
namespace cachelib {

namespace {
inline std::string pathName(const std::string& dir, const std::string& file) {
  return dir + '/' + file;
}

bool removeSegByName(bool posix,
                     const std::string& uniqueName,
                     const std::string& hugePageMountDir,
                     const std::string& posixShmDir) {
  if (!posix) {
    return SysVShmSegment::removeByName(uniqueName);
  }
  bool removed = PosixShmSegment::removeByName(uniqueName, hugePageMountDir);
  if (!posixShmDir.empty()) {
    removed |= PosixShmSegment::removeByName(uniqueName, posixShmDir);
  }
  return removed;
}

std::string uniqueIdForNameAndDir(const std::string& name,
                                  const std::string& cacheDir) {
  return name + std::to_string(XXH3_64bits(cacheDir.data(), cacheDir.size()));
}
} // namespace

ShmManager::ShmManager(const std::string& dir,
                       bool usePosix,
                       const std::string& hugePageMountDir,
                       const std::string& posixShmDir)
    : controlDir_(dir),
      dirHash_(
          std::to_string(XXH3_64bits(controlDir_.data(), controlDir_.size()))),
      hugePageMountDir_(hugePageMountDir),
      posixShmDir_(posixShmDir),
      usePosix_(usePosix) {
  // check that the directory exists. If it does not exist, create
  // one. If it exists, extract the name to key mapping from the metadata
  // file.

  const auto metaFile = pathName(controlDir_, kMetaDataFile);
  mode_t controlMode = 0;
  if (!util::getStatIfExists(controlDir_, &controlMode)) {
    // create directory
    util::makeDir(controlDir_);
    // create an empty meta file. If during shutdown, this does not
    // exist, we dont do a clean shutdown
    createEmptyMetadataFile(metaFile);
    // Lock file for exclusive access
    lockMetadataFile(metaFile);
    return;
  } else if (!S_ISDIR(controlMode)) {
    // exists and is a file
    util::throwSystemError(ENOTDIR);
  }

  // someone indicated by creating our special file that we need to drop the
  // segments.
  const auto dropFileName = pathName(controlDir_, kDropFile);
  const bool dropSegments = util::getStatIfExists(dropFileName, nullptr);

  SCOPE_EXIT {
    if (dropSegments) {
      // now that we have decided to drop the segments, remove the file. If file
      // gets removed for some reason, that is okay since we have already
      // reinitialized the metadata file.
      try {
        util::removePath(dropFileName);
      } catch (...) {
      }
    }
  };

  mode_t metaFileMode = 0;
  if (!util::getStatIfExists(metaFile, &metaFileMode)) {
    // does not exist. create an empty one. If during shutdown, this does not
    // exist, we dont do a clean shutdown
    createEmptyMetadataFile(metaFile);
    // Lock file for exclusive access
    lockMetadataFile(metaFile);
    return;
  } else if (!S_ISREG(metaFileMode)) {
    // exists, but is a directory
    util::throwSystemError(EISDIR);
  }

  // if file exists, init from it if needed.
  const bool reattach = dropSegments ? false : initFromFile();
  if (!reattach) {
    DCHECK(nameToKey_.empty());
  }
  // Lock file for exclusive access
  lockMetadataFile(metaFile);

  // truncate the file so that unless we shutdown clean, we dont re-attach
  // from it.
  createEmptyMetadataFile(metaFile);
}

bool ShmManager::initFromFile() {
  // restore the nameToKey_ map and destroy the contents of the file.
  const std::string fileName = pathName(controlDir_, kMetaDataFile);
  std::ifstream f(fileName);
  SCOPE_EXIT { f.close(); };

  if (!f) {
    util::throwSystemError(ENOENT, "File is invalid");
  }

  std::string buf{std::istreambuf_iterator<char>(f),
                  std::istreambuf_iterator<char>()};
  if (buf.empty()) {
    return false;
  }

  serialization::ShmManagerObject object;
  try {
    apache::thrift::BinarySerializer::deserialize(buf, object);
  } catch (const std::exception& ex) {
    throw std::invalid_argument(
        fmt::format("Dersialization has failed. Reason: {}", ex.what()));
  }

  if (static_cast<bool>(usePosix_) ^
      (*object.shmVal() == static_cast<int8_t>(ShmVal::SHM_POSIX))) {
    throw std::invalid_argument(
        fmt::format("Invalid value for attach. ShmVal: {}", *object.shmVal()));
  }

  for (const auto& kv : *object.nameToKeyMap()) {
    nameToKey_.insert({kv.first, kv.second});
  }

  const std::string persistedHugePageMountDir =
      object.hugePageMountDir().value_or("");
  if (usePosix_ && persistedHugePageMountDir != hugePageMountDir_) {
    XLOGF(INFO,
          "POSIX shared memory mount changed from '{}' to '{}'; removing "
          "incompatible segments and starting fresh",
          persistedHugePageMountDir.empty() ? "(n/a)"
                                            : persistedHugePageMountDir.c_str(),
          hugePageMountDir_.empty() ? "(n/a)" : hugePageMountDir_.c_str());

    std::exception_ptr firstError;
    const auto removeFromMount = [&](const std::string& mount) {
      for (const auto& entry : nameToKey_) {
        try {
          removeSegByName(usePosix_, uniqueIdForName(entry.first), mount,
                          posixShmDir_);
        } catch (...) {
          if (!firstError) {
            firstError = std::current_exception();
          }
        }
      }
    };

    removeFromMount(persistedHugePageMountDir);
    if (firstError) {
      std::rethrow_exception(firstError);
    }
    nameToKey_.clear();
    return false;
  }

  return true;
}

typename ShmManager::ShutDownRes ShmManager::writeActiveSegmentsToFile() {
  const std::string fileName = pathName(controlDir_, kMetaDataFile);
  const bool exists = util::getStatIfExists(fileName, nullptr);
  // if file was deleted in the process, then the user intended to drop all
  // the segments.
  if (!exists) {
    // delete all the segments  that we know exist for this control directory
    // and exit
    removeAllSegments();
    return ShutDownRes::kFileDeleted;
  }

  // write the shmtype, nameToKey_ map to the file.
  DCHECK(metadataStream_);

  serialization::ShmManagerObject object;

  object.shmVal() = usePosix_ ? static_cast<int8_t>(ShmVal::SHM_POSIX)
                              : static_cast<int8_t>(ShmVal::SHM_SYS_V);

  if (usePosix_ && !hugePageMountDir_.empty()) {
    object.hugePageMountDir() = hugePageMountDir_;
  }

  for (const auto& kv : nameToKey_) {
    const auto& name = kv.first;
    const auto& key = kv.second;
    const auto it = segments_.find(name);
    // segment exists and is active.
    if (it != segments_.end() && it->second->isActive()) {
      object.nameToKeyMap()[name] = key;
    }
  }

  // write to file
  std::string buf;
  apache::thrift::BinarySerializer::serialize(object, &buf);
  metadataStream_.seekp(0);
  metadataStream_ << buf;

  metadataStream_.flush();
  if (!metadataStream_) {
    return ShutDownRes::kFailedWrite;
  }
  return ShutDownRes::kSuccess;
}

typename ShmManager::ShutDownRes ShmManager::shutDown() {
  const ShutDownRes ret = writeActiveSegmentsToFile();
  metadataStream_.close();
  metaDataLockFile_.unlock();

  // segments that we did not attach to, but discovered it in the metadata
  // file, clean them up since we will not have written them to file above.
  removeUnAttachedSegments();

  // clear our data.
  segments_.clear();
  nameToKey_.clear();
  return ret;
}

void ShmManager::removeByName(const std::string& dir,
                              const std::string& name,
                              bool posix,
                              const std::string& hugePageMountDir,
                              const std::string& posixShmDir) {
  removeSegByName(posix, uniqueIdForNameAndDir(name, dir), hugePageMountDir,
                  posixShmDir);
}

bool ShmManager::segmentExists(const std::string& cacheDir,
                               const std::string& shmName,
                               bool posix) {
  try {
    ShmSegment(ShmAttach, uniqueIdForNameAndDir(shmName, cacheDir), posix);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::unique_ptr<ShmSegment> ShmManager::attachShmReadOnly(
    const std::string& dir,
    const std::string& name,
    bool posix,
    void* addr,
    const PageSize& pageSize,
    const std::string& hugePageMountDir,
    const std::string& posixShmDir) {
  ShmSegmentOpts opts{pageSize, true /* read only */};
  auto shm = std::make_unique<ShmSegment>(
      ShmAttach, uniqueIdForNameAndDir(name, dir), posix, opts,
      pageSize.isHugePage() ? hugePageMountDir : posixShmDir);
  if (!shm->mapAddress(addr)) {
    throw std::invalid_argument(fmt::format(
        "Error mapping shm {} under {}, addr: {}", name, dir, addr));
  }
  return shm;
}

void ShmManager::cleanup(const std::string& dir,
                         bool posix,
                         const std::string& hugePageMountDir,
                         const std::string& posixShmDir) {
  // instantiate a shm manager and destroy it to clear all the segments
  // associated with the directory.
  ShmManager s(dir, posix, hugePageMountDir, posixShmDir);
}

void ShmManager::removeAllSegments() {
  for (const auto& kv : nameToKey_) {
    removeSegByName(usePosix_, uniqueIdForName(kv.first), hugePageMountDir_,
                    posixShmDir_);
  }
  nameToKey_.clear();
}

void ShmManager::removeUnAttachedSegments() {
  auto it = nameToKey_.begin();
  while (it != nameToKey_.end()) {
    const auto name = it->first;
    // check if the segment is attached.
    if (!segments_.contains(name)) { // not attached
      removeSegByName(usePosix_, uniqueIdForName(name), hugePageMountDir_,
                      posixShmDir_);
      it = nameToKey_.erase(it);
    } else {
      ++it;
    }
  }
}

ShmAddr ShmManager::createShm(const std::string& shmName,
                              size_t size,
                              void* addr,
                              ShmSegmentOpts opts) {
  // we are going to create a new segment most likely after trying to attach
  // to an old one. detach and remove any old ones if they have already been
  // attached or mapped
  removeShm(shmName);

  DCHECK(!segments_.contains(shmName));
  DCHECK(!nameToKey_.contains(shmName));

  std::unique_ptr<ShmSegment> newSeg;
  try {
    newSeg =
        std::make_unique<ShmSegment>(ShmNew, uniqueIdForName(shmName), size,
                                     usePosix_, opts, segmentDir(opts));
  } catch (const std::system_error& e) {
    // if segment already exists by this key and we dont know about
    // it(EEXIST), its an invalid state.
    if (e.code().value() == EEXIST) {
      throw;
    }
    throw std::invalid_argument(
        fmt::format("Unable to create shared memory segment: name: {}, "
                    "size: {}, addr: {}. msg: {}",
                    shmName, size, addr, e.what()));
  }

  DCHECK(newSeg);
  if (!newSeg->mapAddress(addr, opts.alignment)) {
    throw std::invalid_argument(
        fmt::format("Unable to map shared memory segment after create: "
                    "name: {}, size: {}, addr: {}",
                    shmName, size, addr));
  }

  auto ret = newSeg->getCurrentMapping();
  nameToKey_.emplace(shmName, newSeg->getKeyStr());
  segments_.emplace(shmName, std::move(newSeg));
  return ret;
}

folly::F14FastMap<std::string, std::unique_ptr<ShmSegment>>::iterator
ShmManager::attachNewShm(const std::string& shmName,
                         const ShmSegmentOpts& opts) {
  const auto keyIt = nameToKey_.find(shmName);
  // if key is not known already, there is not much we can do to attach.
  if (keyIt == nameToKey_.end()) {
    throw std::invalid_argument(
        fmt::format("Unable to find any segment with name {}", shmName));
  }

  // This means the segment exists and we can try to attach it.
  try {
    auto seg = std::make_unique<ShmSegment>(ShmAttach, uniqueIdForName(shmName),
                                            usePosix_, opts, segmentDir(opts));
    auto [it, _] = segments_.emplace(shmName, std::move(seg));
    return it;
  } catch (const std::system_error& e) {
    throw std::invalid_argument(fmt::format(
        "Unable to attach to shared memory segment: name: {}, error: {}",
        shmName, e.what()));
  }
}

ShmAddr ShmManager::attachShm(const std::string& shmName,
                              void* addr,
                              ShmSegmentOpts opts) {
  // check if segment already exists.
  auto shmIt = segments_.find(shmName);
  if (shmIt == segments_.end()) {
    shmIt = attachNewShm(shmName, opts);
  }
  DCHECK(shmIt != segments_.end());

  auto& shm = *shmIt->second;
  if (shm.isMapped() || !shm.mapAddress(addr, opts.alignment)) {
    throw std::invalid_argument(
        fmt::format("Unable to map shared memory segment after attach:"
                    " name: {}, addr: {}, mapped: {}",
                    shmName, addr, shm.isMapped()));
  }

  return shm.getCurrentMapping();
}

bool ShmManager::removeShm(const std::string& shmName) {
  try {
    auto& shm = getShmByName(shmName);
    shm.detachCurrentMapping();

    // first mark it to be removed if it is active
    if (shm.isActive()) {
      shm.markForRemoval();
    }

    DCHECK(shm.isMarkedForRemoval());
    DCHECK(shm.isInvalid());
  } catch (const std::invalid_argument&) {
    // shm by this name is not attached.
    if (!removeSegByName(usePosix_, uniqueIdForName(shmName), hugePageMountDir_,
                         posixShmDir_)) {
      DCHECK(!segments_.contains(shmName));
      DCHECK(!nameToKey_.contains(shmName));
      return false;
    }
  }
  // not mapped and already removed.
  segments_.erase(shmName);
  nameToKey_.erase(shmName);
  return true;
}

ShmSegment& ShmManager::getShmByName(const std::string& shmName) {
  const auto it = segments_.find(shmName);
  if (it != segments_.end()) {
    DCHECK(it->second != nullptr);
    return *it->second.get();
  } else {
    throw std::invalid_argument(
        fmt::format("shared memory segment does not exist: name: {}", shmName));
  }
}

} // namespace cachelib
} // namespace facebook
