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

add_library (cachelib_interface
  Cache.cpp
  CacheItem.cpp
  Handle.cpp
  InFlightTransfers.cpp
  MemcpyConnector.cpp
  TransferExecutor.cpp
  Stats.cpp
  components/FlashCacheComponent.cpp
  components/RAMCacheComponent.cpp
  utils/CoroFiberAdapter.cpp
)

# GCC 11's C++20 coroutine support has bugs that cause internal compiler errors
# (segfaults) when compiling coroutine templates at -O2. Reduce optimization
# for affected files to work around this.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "12")
  set_source_files_properties(
    components/FlashCacheComponent.cpp
    PROPERTIES COMPILE_OPTIONS "-O1"
  )
endif()

target_link_libraries(cachelib_interface PUBLIC
  cachelib_allocator
)

install(TARGETS cachelib_interface
        EXPORT cachelib-exports
        DESTINATION ${LIB_INSTALL_DIR})

if (BUILD_TESTS)
  add_library (interface_test_support STATIC
    components/tests/CacheComponentFactory.cpp
  )
  target_link_libraries (interface_test_support PUBLIC
    cachelib_interface
    navy_test_support
    glog::glog
    gflags
    GTest::gtest
    GTest::gmock
  )

  function (add_source_test SOURCE_FILE)
    generic_add_source_test("interface-test" "${SOURCE_FILE}"
                     interface_test_support "${ARGN}")
    get_filename_component(TEST_NAME "${SOURCE_FILE}" NAME_WE)
    target_sources("interface-test-${TEST_NAME}" PRIVATE
      "${CACHELIB_HOME}/common/tests/TestMain.cpp")
  endfunction()

  # GCC ICE in gimplify_var_or_parm_decl with coroutine test macros
  if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_source_test (tests/CacheTest.cpp)
    add_source_test (tests/InterfaceTest.cpp)
    add_source_test (tests/DescriptorTest.cpp)
    add_source_test (tests/MemcpyConnectorTest.cpp)
    add_source_test (tests/TransferExecutorTest.cpp)
    add_source_test (components/tests/CacheComponentTest.cpp)
    add_source_test (components/tests/FlashCacheComponentTest.cpp)
  endif()
  add_source_test (tests/ResultTest.cpp)
  add_source_test (tests/InFlightLookupsTest.cpp)
  add_source_test (tests/InFlightTransfersTest.cpp)
  add_source_test (utils/tests/CoroFiberAdapterTest.cpp)
  add_source_test (utils/tests/ShardedSerializerTest.cpp)
endif()
