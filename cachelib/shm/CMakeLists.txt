# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

add_thrift_file(SHM shm.thrift frozen2)

add_library (cachelib_shm
  ${SHM_THRIFT_FILES}
  PosixShmSegment.cpp
  ShmManager.cpp
  SysVShmSegment.cpp
  )
add_dependencies(cachelib_shm thrift_generated_files)
target_link_libraries(cachelib_shm PUBLIC
  cachelib_common
  NUMA::NUMA
  ${Xxhash_LIBRARY}
)

install(TARGETS cachelib_shm
        EXPORT cachelib-exports
        DESTINATION ${LIB_INSTALL_DIR} )

if (BUILD_TESTS)
  add_library (shm_test_support
    tests/common.cpp)
  target_link_libraries (shm_test_support PUBLIC
    cachelib_shm
    glog::glog
    gflags
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
  )

  function (add_source_test SOURCE_FILE)
     generic_add_source_test("shm-test" "${SOURCE_FILE}" shm_test_support "${ARGN}")
  endfunction()

  # fails from cmake: add_source_test (tests/test_page_size.cpp)
  add_source_test (tests/test_posix.cpp)
  add_source_test (tests/test_shm.cpp)
  add_source_test (tests/test_shm_death_style.cpp)
  add_source_test (tests/test_shm_manager.cpp)
  add_source_test (tests/test_sysv.cpp)
endif()
