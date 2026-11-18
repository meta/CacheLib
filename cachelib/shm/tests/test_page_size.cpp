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

#include <folly/Conv.h>
#include <folly/FileUtil.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/testing/TestUtil.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>

#include "cachelib/allocator/ReadOnlySharedCacheView.h"
#include "cachelib/shm/HugePageTestUtils.h"
#include "cachelib/shm/Shm.h"
#include "cachelib/shm/ShmCommon.h"
#include "cachelib/shm/ShmManager.h"
#include "cachelib/shm/tests/common.h"

namespace facebook {
namespace cachelib {
namespace tests {

void ShmTest::testPageSize(size_t p, bool posix) {
  ShmSegmentOpts opts{PageSize(p)};
  const auto& ps = opts.pageSize;
  size_t size = ps.getPageAlignedSize(4096);
  ASSERT_TRUE(ps.isPageAlignedSize(size));
  // POSIX huge-page segments need the hugetlbfs mount passed to the segment.
  const std::string mountDir = (posix && ps.isHugePage())
                                   ? detail::hugetlbfsMountFromEnv()
                                   : std::string{};

  // create with unaligned size
  ASSERT_NO_THROW({
    ShmSegment s(ShmNew, segmentName, size, posix, opts, mountDir);
    ASSERT_TRUE(s.mapAddress(nullptr));
    ASSERT_EQ(ps.getPageSize(),
              ps.getPageSizeInSMap(s.getCurrentMapping().addr));
  });

  ASSERT_NO_THROW({
    ShmSegment s2(ShmAttach, segmentName, posix, opts, mountDir);
    ASSERT_TRUE(s2.mapAddress(nullptr));
    ASSERT_EQ(ps.getPageSize(),
              ps.getPageSizeInSMap(s2.getCurrentMapping().addr));
  });
}

TEST(PageSize, MappingWithColonInPath) {
  folly::test::TemporaryFile file("cachelib:page-size");
  const size_t size = PageSize::systemPageSize();
  ASSERT_EQ(0, ftruncate(file.fd(), size));
  void* addr =
      mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd(), 0);
  ASSERT_NE(MAP_FAILED, addr);
  SCOPE_EXIT { munmap(addr, size); };

  EXPECT_EQ(size, PageSize::getPageSizeInSMap(addr));
}

TEST_F(ShmTestPosix, HugePageRequiresMount) {
  ShmSegmentOpts opts{PageSize(PageSize::kHugePageSize2MB)};
  const size_t size = opts.pageSize.getPageAlignedSize(4096);
  EXPECT_THROW(ShmSegment(ShmNew, segmentName, size, /*usePosix=*/true, opts,
                          /*hugePageMountDir=*/""),
               std::system_error);
}

// The HugeTLB end-to-end tests require a provisioned huge-page pool (kernel
// cmdline / sysctl) and, for POSIX, a hugetlbfs mount via kHugetlbfsMountEnv.
// They skip when those are absent (e.g. sandcastle) rather than fail.

TEST_F(ShmTestPosix, PageSizesNormal) {
  testPageSize(PageSize::kNormalPageSize, true);
}

TEST_F(ShmTestPosix, PageSizesTwoMB) {
  if (!hugePagesUsable(PageSize::kHugePageSize2MB, true)) {
    GTEST_SKIP() << "2MB HugeTLB pool / hugetlbfs mount not available";
  }
  testPageSize(PageSize::kHugePageSize2MB, true);
}

TEST_F(ShmTestSysV, PageSizesNormal) {
  testPageSize(PageSize::kNormalPageSize, false);
}

TEST_F(ShmTestSysV, PageSizesTwoMB) {
  if (!hugePagesUsable(PageSize::kHugePageSize2MB, false)) {
    GTEST_SKIP() << "2MB HugeTLB pool not available";
  }
  testPageSize(PageSize::kHugePageSize2MB, false);
}

TEST(ReadOnlySharedCacheView, PosixTwoMBPages) {
  if (!hugePagesUsable(PageSize::kHugePageSize2MB, true)) {
    GTEST_SKIP() << "2MB HugeTLB pool / hugetlbfs mount not available";
  }

  folly::test::TemporaryDirectory tmpDir;
  const auto cacheDir = tmpDir.path().string();

  const auto pageSize = PageSize(PageSize::kHugePageSize2MB);
  ShmManager manager(cacheDir, true, detail::hugetlbfsMountFromEnv());
  const auto mapping =
      manager.createShm(cachelib::detail::kShmCacheName, pageSize.getPageSize(),
                        nullptr, ShmSegmentOpts(pageSize));
  *static_cast<char*>(mapping.addr) = 'x';

  ReadOnlySharedCacheView view(cacheDir, true, nullptr, pageSize,
                               detail::hugetlbfsMountFromEnv());
  auto* viewAddr = reinterpret_cast<void*>(view.getShmMappingAddress());
  EXPECT_EQ(pageSize.getPageSize(), PageSize::getPageSizeInSMap(viewAddr));
  EXPECT_EQ('x', *static_cast<const char*>(view.getItemPtrFromOffset(0)));
}

} // namespace tests
} // namespace cachelib
} // namespace facebook
