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

add_thrift_file(BLOOM BloomFilter.thrift frozen2)

add_library (cachelib_common
  BloomFilter.cpp
  Cohort.cpp
  FurcHash.cpp
  CountDownLatch.cpp
  EventTracker.cpp
  ${BLOOM_THRIFT_FILES}
  hothash/HotHashDetector.cpp
  inject_pause.cpp
  PercentileStats.cpp
  PeriodicWorker.cpp
  piecewise/GenericPieces.cpp
  piecewise/GenericPiecesBase.cpp
  piecewise/RequestRange.cpp
  Serialization.cpp
  Utils.cpp
)
add_dependencies(cachelib_common thrift_generated_files)

# Find xxhash library
find_library(XXHASH_LIBRARY xxhash)

target_link_libraries(cachelib_common PUBLIC
  Threads::Threads
  thrift::rpc
  thrift::protocol
  thrift::dynamic_value
  Folly::folly
  Folly::follybenchmark
  Folly::folly_debugging_exception_tracer_exception_tracer
  Folly::folly_debugging_exception_tracer_exception_tracer_base
  Folly::folly_debugging_exception_tracer_exception_counter
  GTest::gtest
  magic_enum::magic_enum
  ${XXHASH_LIBRARY}
)

if (BUILD_WITH_DTO)
  if (NOT TARGET DTO::dto_explicit)
    message(FATAL_ERROR
      "BUILD_WITH_DTO requires DTO::dto_explicit. "
      "Build DTO with DTO_BUILD_EXPLICIT=ON.")
  endif()
  target_link_libraries(cachelib_common PUBLIC DTO::dto_explicit)
  target_compile_definitions(cachelib_common PUBLIC CACHELIB_BUILD_WITH_DTO)
endif()

install(TARGETS cachelib_common
        EXPORT cachelib-exports
        DESTINATION ${LIB_INSTALL_DIR} )


if (BUILD_TESTS)
  add_library (common_test_utils STATIC
    TestUtils.cpp
  )
  target_link_libraries (common_test_utils PUBLIC
    cachelib_common
    gflags
    GTest::gtest
    GTest::gtest_main
  )
  add_library (common_test_support OBJECT
    hothash/HotHashDetectorTest.cpp
    piecewise/GenericPiecesTest.cpp
    piecewise/RequestRangeTest.cpp
  )
  target_link_libraries (common_test_support PUBLIC
    cachelib_common
    common_test_utils
    gflags
    GTest::gtest
    GTest::gtest_main
  )

  function (add_source_test SOURCE_FILE)
     # Add any additional libraries BEFORE the "common_test_support" to ensure
     # that the linking ordering is correct (e.g. if any of them requires
     # "gflags", gflags should appear AFTER it).
     generic_add_source_test("common-test" "${SOURCE_FILE}"
                      "${ARGN}" common_test_support)
  endfunction()

  add_source_test (tests/AccessTrackerTest.cpp)
  # need allocator/memory/tests/TestBase.cpp:
  #add_source_test (tests/ApproxSplitSetTest.cpp allocator_test_support)
  add_source_test (tests/BloomFilterTest.cpp)
  add_source_test (tests/BytesEqualTest.cpp)
  add_source_test (tests/CohortTests.cpp)
  add_source_test (tests/CounterTests.cpp)
  add_source_test (tests/CountMinSketchTest.cpp)
  add_source_test (tests/EventInterfaceTest.cpp allocator_test_support)
  add_source_test (tests/HashTests.cpp)
  add_source_test (tests/IteratorsTests.cpp)
  add_source_test (tests/MutexTests.cpp)
  add_source_test (tests/PeriodicWorkerTest.cpp)
  add_source_test (tests/SerializationTest.cpp allocator_test_support)
  add_source_test (tests/UtilTests.cpp cachelib_shm)
  add_source_test (tests/CountDownLatchTest.cpp)
  # fails from cmake: add_source_test (tests/UtilTestsRSS.cpp)
endif()
