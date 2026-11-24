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

#include <fmt/core.h>
#include <folly/File.h>
#include <gtest/gtest.h>

#include <system_error>

#include "cachelib/allocator/nvmcache/NavyConfig.h"
#include "cachelib/allocator/nvmcache/NavySetup.h"
#include "cachelib/allocator/tests/NvmTestUtils.h"
#include "cachelib/common/Utils.h"

namespace facebook {
namespace cachelib {
namespace tests {

namespace {
// chmod on a file.
// Throw if the file does not exist.
void changeMode(const std::string& name, mode_t mode) {
  if (!util::pathExists(name)) {
    throw std::invalid_argument(
        fmt::format("Trying to chmod on file {} that does not exist!", name));
  }
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s", name.c_str());

  chmod(tmp, mode);
}

size_t getFileSize(std::string& fileName) {
  auto f = folly::File(fileName.c_str());
  struct stat fileStat;
  auto ret = fstat(f.fd(), &fileStat);
  XDCHECK(ret == 0);
  return fileStat.st_size;
}

} // namespace

TEST(NavySetupTest, ChecksumOffloadWithoutCrc32cIsDisabled) {
  for (bool bigHashOffload : {false, true}) {
    SCOPED_TRACE(bigHashOffload);
    navy::NavyConfig config;
    config.setMemoryFile(64 * 1024 * 1024);
    config.setReaderAndWriterThreads(1, 1);
    config.blockCache().setRegionSize(4 * 1024 * 1024);
    config.bigHash().setSizePctAndMaxItemSize(10, 1024);
    if (bigHashOffload) {
      config.bigHash().setChecksumDsaOffload(true);
    } else {
      config.blockCache().setCopyAndChecksumDsaOffload(true);
    }

    // The engines reject offload with legacy CRC32, so setup must disable it
    // before constructing them.
    EXPECT_NO_THROW(createNavyCache(config, {}, {}, true, nullptr, false));

    config.setChecksumType(navy::ChecksumType::Crc32c);
    EXPECT_NO_THROW(createNavyCache(config, {}, {}, true, nullptr, false));
  }
}

TEST(NavySetupTest, CacheSizes) {
  constexpr uint64_t kMB = 1024 * 1024;
  constexpr uint64_t kBlockSize = 4096;

  navy::NavyConfig config;
  config.setMemoryFile(100 * kMB);
  config.setBlockSize(kBlockSize);

  auto sizes = getNavyCacheSizes(config);
  EXPECT_EQ(100 * kMB, sizes.deviceSize);
  EXPECT_EQ(512 * 1024, sizes.metadataSize);

  config.setDeviceMetadataSize(1);
  sizes = getNavyCacheSizes(config);
  EXPECT_EQ(kBlockSize, sizes.metadataSize);

  config.setDeviceMetadataSize(0);
  config.blockCache().setRegionSize(8 * kMB);
  config.setRaidFiles({"one", "two", "three"}, 9 * kMB);
  sizes = getNavyCacheSizes(config);
  EXPECT_EQ(24 * kMB, sizes.deviceSize);
  EXPECT_EQ(30 * kBlockSize, sizes.metadataSize);
}

TEST(NavySetupTest, RAID0DeviceSize) {
  // Verify size is reduced when we pass in a size that's not aligned to
  // stripeSize for RAID0Device

  auto filePath = fmt::format("/tmp/navy_device_raid0io_test-{}", ::getpid());
  util::makeDir(filePath);
  SCOPE_EXIT { util::removePath(filePath); };

  std::vector<std::string> files = {filePath + "/CACHE0", filePath + "/CACHE1",
                                    filePath + "/CACHE2", filePath + "/CACHE3"};

  int size = 9 * 1024 * 1024;
  int ioAlignSize = 4096;
  int stripeSize = 8 * 1024 * 1024;

  std::vector<std::string> navyFileArray;
  for (const auto& file : files) {
    navyFileArray.push_back(file);
  }

  navy::NavyConfig cfg{};
  cfg.setRaidFiles(navyFileArray, size, true);
  cfg.blockCache().setRegionSize(stripeSize);
  cfg.setBlockSize(ioAlignSize);

  auto device = createDevice(cfg, nullptr);
  EXPECT_GT(size * files.size(), device->getSize());
  EXPECT_EQ(files.size() * 8 * 1024 * 1024, device->getSize());
}

// Make sure that we throw when the device failed to create.
TEST(NavySetupTest, FileCreationFailure) {
  auto filePath = fmt::format("/tmp/navy_device_raid0io_test-{}", ::getpid());
  util::makeDir(filePath);
  // Change the directory permission so that cachelib can't create file.
  changeMode(filePath, 0111);
  SCOPE_EXIT {
    changeMode(filePath, 0777);
    util::removePath(filePath);
  };
  std::vector<std::string> files = {filePath + "/CACHE0", filePath + "/CACHE1",
                                    filePath + "/CACHE2", filePath + "/CACHE3"};

  int size = 9 * 1024 * 1024;
  int ioAlignSize = 4096;
  int stripeSize = 8 * 1024 * 1024;

  std::vector<std::string> navyFileArray;
  for (const auto& file : files) {
    navyFileArray.push_back(file);
  }

  navy::NavyConfig cfg{};
  cfg.setRaidFiles(navyFileArray, size, true);
  cfg.blockCache().setRegionSize(stripeSize);
  cfg.setBlockSize(ioAlignSize);

  // Expect to throw.
  EXPECT_THROW({ createDevice(cfg, nullptr); }, std::system_error);

  // Change the permission back and expect to initialize normally.
  changeMode(filePath, 0777);
  auto device = createDevice(cfg, nullptr);
  EXPECT_GT(size * files.size(), device->getSize());
  EXPECT_EQ(files.size() * 8 * 1024 * 1024, device->getSize());
}

// Test the file size will change when the requested file size is different from
// the existing file size for FileDevice
TEST(NavySetupTest, FileDeviceResizeFile) {
  auto filePath = fmt::format("/tmp/navy_device_test-{}", ::getpid());
  util::makeDir(filePath);
  SCOPE_EXIT { util::removePath(filePath); };
  std::string file = filePath + "/CACHE0";

  uint64_t fileSize = 1024 * 1024 * 1024;
  int ioAlignSize = 4096;
  int stripeSize = 8 * 1024 * 1024;

  std::vector<std::string> navyFileArray;

  navy::NavyConfig cfg{};
  cfg.setSimpleFile(file, fileSize, true /*truncateFile*/);
  cfg.blockCache().setRegionSize(stripeSize);
  cfg.setBlockSize(ioAlignSize);

  createDevice(cfg, nullptr);

  EXPECT_EQ(getFileSize(file), fileSize);

  // reduce the file size ("truncate" = false)
  cfg.setSimpleFile(file, fileSize / 2, false /*truncateFile*/);
  createDevice(cfg, nullptr);
  EXPECT_EQ(getFileSize(file), fileSize); // file size won't change

  // increase the file size ("truncate" = false)
  cfg.setSimpleFile(file, fileSize * 2, false /*truncateFile*/);
  createDevice(cfg, nullptr);
  EXPECT_EQ(getFileSize(file), fileSize); // file size won't change

  // reduce the file size ("truncate" = true)
  cfg.setSimpleFile(file, fileSize / 2, true /*truncateFile*/);
  createDevice(cfg, nullptr);
  EXPECT_EQ(getFileSize(file), fileSize / 2);

  // increase the file size ("truncate" = true)
  cfg.setSimpleFile(file, fileSize * 2, true /*truncateFile*/);
  createDevice(cfg, nullptr);
  EXPECT_EQ(getFileSize(file), fileSize * 2);
}

// Test the file size will change when the requested file size is different from
// the existing file size for RAID0Device
TEST(NavySetupTest, RAID0DeviceResizeFile) {
  auto filePath = fmt::format("/tmp/navy_device_raid0io_test-{}", ::getpid());
  util::makeDir(filePath);
  SCOPE_EXIT { util::removePath(filePath); };

  std::vector<std::string> files = {filePath + "/CACHE0", filePath + "/CACHE1",
                                    filePath + "/CACHE2", filePath + "/CACHE3"};

  uint64_t fileSize = 1024 * 1024 * 1024;
  int ioAlignSize = 4096;
  int stripeSize = 8 * 1024 * 1024;

  std::vector<std::string> navyFileArray;
  for (const auto& file : files) {
    navyFileArray.push_back(file);
  }

  navy::NavyConfig cfg{};
  cfg.setRaidFiles(navyFileArray, fileSize, true /*truncateFile*/);
  cfg.blockCache().setRegionSize(stripeSize);
  cfg.setBlockSize(ioAlignSize);

  createDevice(cfg, nullptr);
  for (auto& file : files) {
    EXPECT_EQ(getFileSize(file), fileSize);
  }

  // reduce the file size ("truncate" = false)
  cfg.setRaidFiles(navyFileArray, fileSize / 2, false /*truncateFile*/);
  createDevice(cfg, nullptr);
  for (auto& file : files) {
    EXPECT_EQ(getFileSize(file), fileSize); // file size won't change
  }

  // increase the file size ("truncate" = false)
  cfg.setRaidFiles(navyFileArray, fileSize * 2, false /*truncateFile*/);
  createDevice(cfg, nullptr);
  for (auto& file : files) {
    EXPECT_EQ(getFileSize(file), fileSize); // file size won't change
  }

  // reduce the file size ("truncate" = true)
  cfg.setRaidFiles(navyFileArray, fileSize / 2, true /*truncateFile*/);
  createDevice(cfg, nullptr);
  for (auto& file : files) {
    EXPECT_EQ(getFileSize(file), fileSize / 2);
  }

  // increase the file size ("truncate" = true)
  cfg.setRaidFiles(navyFileArray, fileSize * 2, true /*truncateFile*/);
  createDevice(cfg, nullptr);
  for (auto& file : files) {
    EXPECT_EQ(getFileSize(file), fileSize * 2);
  }
}

TEST(NavySetupTest, EnginesSetup) {
  {
    // Valid NavyConfig setup

    navy::NavyConfig cfg = utils::getNvmTestConfig("/tmp");
    // cfg.setSimpleFile("test", 1024 * 1024);

    // 20MB block cache 0.
    cfg.blockCache().setSize(20 * 1024 * 1024);
    navy::EnginesConfig pair1;
    pair1.blockCache().setRegionSize(4 * 1024 * 1024);
    // Second bighash: 5 pct, 640 threshold.
    pair1.bigHash().setSizePctAndMaxItemSize(5, 640);
    cfg.addEnginePair(std::move(pair1));

    // Send the key by length
    cfg.setEnginesSelector([](HashedKey hk) { return hk.key().size() % 2; });

    EXPECT_NE(nullptr, createNavyCache(cfg, {}, {}, true, nullptr, false));
  }

  {
    navy::NavyConfig cfg = utils::getNvmTestConfig("/tmp");

    // Will throw. Small item max size too large.
    cfg.bigHash().setSizePctAndMaxItemSize(1, 40960);

    // 20MB block cache 0.
    cfg.blockCache().setSize(20 * 1024 * 1024);
    navy::EnginesConfig pair1;
    pair1.blockCache().setRegionSize(4 * 1024 * 1024);
    // Second bighash: 5 pct, 640 threshold.
    pair1.bigHash().setSizePctAndMaxItemSize(5, 640);
    cfg.addEnginePair(std::move(pair1));

    // Send the key by length
    cfg.setEnginesSelector([](HashedKey hk) { return hk.key().size() % 2; });

    EXPECT_THROW(
        { createNavyCache(cfg, {}, {}, true, nullptr, false); },
        std::invalid_argument);
  }

  {
    navy::NavyConfig cfg = utils::getNvmTestConfig("/tmp");

    // 20MB block cache 0.
    cfg.blockCache().setSize(20 * 1024 * 1024);
    navy::EnginesConfig pair1;
    pair1.blockCache().setRegionSize(4 * 1024 * 1024);
    // Second bighash: 5 pct, 640 threshold.
    pair1.bigHash().setSizePctAndMaxItemSize(5, 640);
    cfg.addEnginePair(std::move(pair1));

    // Exception. No engine selector.
    EXPECT_THROW(
        { createNavyCache(cfg, {}, {}, true, nullptr, false); },
        std::invalid_argument);
  }

  {
    navy::NavyConfig cfg = utils::getNvmTestConfig("/tmp");

    // 20MB block cache 0.
    cfg.blockCache().setSize(20 * 1024 * 1024);
    navy::EnginesConfig pair1;
    pair1.blockCache().setRegionSize(4 * 1024 * 1024);
    // Second bighash: 5 pct, 640 threshold.
    pair1.bigHash().setSizePctAndMaxItemSize(5, 640);
    // Will throw. Set size for the second block cache.
    pair1.blockCache().setSize(20 * 1024 * 1024);
    cfg.addEnginePair(std::move(pair1));
    // Send the key by length
    cfg.setEnginesSelector([](HashedKey hk) { return hk.key().size() % 2; });

    // Exception. The last block cache does not take the full psace.
    EXPECT_THROW(
        { createNavyCache(cfg, {}, {}, true, nullptr, false); },
        std::invalid_argument);
  }
}
} // namespace tests
} // namespace cachelib
} // namespace facebook
