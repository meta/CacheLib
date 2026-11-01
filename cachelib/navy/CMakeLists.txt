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

add_thrift_file(SERIALIZATION serialization/objects.thrift json)

add_library (cachelib_navy
  ${SERIALIZATION_THRIFT_FILES}
  admission_policy/DynamicRandomAP.cpp
  admission_policy/RejectRandomAP.cpp
  bighash/BigHash.cpp
  bighash/Bucket.cpp
  bighash/BucketStorage.cpp
  block_cache/Allocator.cpp
  block_cache/BlockCache.cpp
  block_cache/CombinedEntryBlock.cpp
  block_cache/FifoPolicy.cpp
  block_cache/FixedSizeIndex.cpp
  block_cache/HitsReinsertionPolicy.cpp
  block_cache/LruPolicy.cpp
  block_cache/Region.cpp
  block_cache/RegionManager.cpp
  block_cache/SparseMapIndex.cpp
  common/Buffer.cpp
  common/ChecksumOffload.cpp
  common/Device.cpp
  common/FdpNvme.cpp
  common/Hash.cpp
  common/NavyThread.cpp
  common/SizeDistribution.cpp
  common/Types.cpp
  driver/Driver.cpp
  engine/EnginePair.cpp
  Factory.cpp
  scheduler/NavyRequestDispatcher.cpp
  scheduler/NavyRequestScheduler.cpp
  scheduler/ThreadPoolJobScheduler.cpp
  scheduler/ThreadPoolJobQueue.cpp
  serialization/RecordIO.cpp
  testing/MockDevice.cpp
  )
add_dependencies(cachelib_navy thrift_generated_files)

if(CMAKE_SYSTEM_NAME STREQUAL Linux)
  set(MISSING_FALLOCATE FALSE)
else()
  set(MISSING_FALLOCATE TRUE)
  target_compile_definitions(cachelib_navy PRIVATE MISSING_FALLOCATE)
  target_compile_definitions(cachelib_navy PRIVATE MISSING_FADVISE)
endif()

target_link_libraries(cachelib_navy PUBLIC
  cachelib_common
  cachelib_shm
  cachelib_nvmitem
  GTest::gmock
  )

# DTO (DSA offload runtime) is resolved in the top-level CMakeLists.txt and
# linked PUBLIC into cachelib_common, so cachelib_navy inherits
# DTO::dto_explicit and the CACHELIB_BUILD_WITH_DTO definition transitively.

install(TARGETS cachelib_navy
        EXPORT cachelib-exports
        DESTINATION ${LIB_INSTALL_DIR} )

if (BUILD_TESTS)
  add_library(navy_test_support
    testing/BufferGen.cpp
    testing/MockDevice.cpp
    testing/MockJobScheduler.cpp
    testing/SeqPoints.cpp
    )
  add_dependencies(navy_test_support thrift_generated_files)
  target_link_libraries(navy_test_support PUBLIC
    cachelib_navy
    glog::glog
    gflags
    GTest::gtest
    GTest::gmock
  )


  function (add_source_test SOURCE_FILE)
     generic_add_source_test(
       "navy-test" "${SOURCE_FILE}" navy_test_support "${ARGN}"
     )
    get_filename_component(TEST_NAME "${SOURCE_FILE}" NAME_WE)
    target_sources("navy-test-${TEST_NAME}" PRIVATE
      "${CACHELIB_HOME}/common/tests/TestMain.cpp")
  endfunction()

  add_source_test (common/tests/BufferTest.cpp)
  add_source_test (common/tests/ChecksumOffloadTest.cpp)
  add_source_test (common/tests/HashTest.cpp)
  add_source_test (common/tests/UtilsTest.cpp)
  add_source_test (bighash/tests/BucketStorageTest.cpp)
  add_source_test (bighash/tests/BucketTest.cpp)
  add_source_test (admission_policy/tests/DynamicRandomAPTest.cpp)
  add_source_test (admission_policy/tests/RejectRandomAPTest.cpp)
  add_source_test (block_cache/tests/FifoPolicyTest.cpp)
  add_source_test (block_cache/tests/HitsReinsertionPolicyTest.cpp)
  add_source_test (block_cache/tests/LruPolicyTest.cpp)
  add_source_test (block_cache/tests/RegionTest.cpp)
  add_source_test (serialization/tests/RecordIOTest.cpp)
  add_source_test (serialization/tests/SerializationTest.cpp)
  add_source_test (scheduler/tests/OrderedThreadPoolJobSchedulerTest.cpp)
  add_source_test (scheduler/tests/ThreadPoolJobSchedulerTest.cpp)
  add_source_test (driver/tests/DriverTest.cpp)
  # Failes from cmake
  # if (NOT MISSING_FALLOCATE)
  #  add_source_test (common/tests/DeviceTest.cpp)
  # endif()
  add_source_test (block_cache/tests/AllocatorTest.cpp)
  add_source_test (block_cache/tests/RegionManagerTest.cpp)
  add_source_test (testing/tests/BufferGenTest.cpp)
  add_source_test (testing/tests/MockJobSchedulerTest.cpp)
  add_source_test (testing/tests/SeqPointsTest.cpp)
  add_source_test (block_cache/tests/BlockCacheTest.cpp)
  set_tests_properties(
    "navy-test-BlockCacheTest.BlockCache.ExpiredItemDestructorCallback"
    PROPERTIES DISABLED TRUE
  )
  add_source_test (block_cache/tests/CombinedEntryBlockTest.cpp)
  add_source_test (bighash/tests/BigHashTest.cpp)
endif()
