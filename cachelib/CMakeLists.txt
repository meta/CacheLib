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

# https://cmake.org/examples/
# CMakeLists files in this project can
# refer to the root source directory of the project as ${HELLO_SOURCE_DIR} and
# to the root binary directory of the project as ${HELLO_BINARY_DIR}.

cmake_minimum_required (VERSION 3.14)

## TODO: get version from variable
project (CacheLib VERSION 0.1)

#configure_file(cachelib/cachelib_config.h.in cachelib_config.h)

if (NOT DEFINED CACHELIB_MAJOR_VERSION)
  set(CACHELIB_MAJOR_VERSION 0)
endif ()
set(CACHELIB_MINOR_VERSION 1)
set(CACHELIB_PATCH_VERSION 0)
set(CACHELIB_VERSION
 ${CACHELIB_MAJOR_VERSION}.${CACHELIB_MINOR_VERSION}.${CACHELIB_PATCH_VERSION})

set(PACKAGE_NAME      "cachelib")
if (NOT DEFINED PACKAGE_VERSION)
  set(PACKAGE_VERSION   "${CACHELIB_VERSION}")
endif ()
set(PACKAGE_STRING    "${PACKAGE_NAME} ${PACKAGE_VERSION}")
set(PACKAGE_TARNAME   "${PACKAGE_NAME}-${PACKAGE_VERSION}")
set(PACKAGE_BUGREPORT "https://github.com/facebook/TBD")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BUILD_TESTS "If enabled, compile the tests." ON)

option(BUILD_WITH_DTO
  "If enabled, build with the DTO library for Intel DSA offload support." OFF)


set(BIN_INSTALL_DIR bin CACHE STRING
    "The subdirectory where binaries should be installed")
set(TESTS_INSTALL_DIR tests CACHE STRING
    "The subdirectory where test binaries should be installed")
set(INCLUDE_INSTALL_DIR include/cachelib CACHE STRING
    "The subdirectory where header files should be installed")
set(LIB_INSTALL_DIR lib CACHE STRING
    "The subdirectory where libraries should be installed")
set(CMAKE_INSTALL_DIR lib/cmake/cachelib CACHE STRING
    "The subdirectory where CMake package config files should be installed")
set(CONFIGS_INSTALL_DIR test_configs CACHE STRING
    "The subdirectory where sample test configurations should be installed")

# CMake include directories
set(CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/CMake"
  # For shipit-transformed builds
  "${CMAKE_CURRENT_SOURCE_DIR}/../build/fbcode_builder/CMake"
  ${CMAKE_MODULE_PATH})

include(FBThriftLibrary)

# When installing the library (and dependencies like folly) in a non-default
# prefix, this will let projects linking against *.so to find libfolly.so
# automatically.
#set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${LIB_INSTALL_DIR}")
##https://gitlab.kitware.com/cmake/community/-/wikis/doc/cmake/RPATH-handling
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib:$ORIGIN/../lib64:$ORIGIN/../lib-os")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)

set(CACHELIB_HOME ${CMAKE_CURRENT_SOURCE_DIR})
set(CACHELIB_BUILD ${CMAKE_CURRENT_BINARY_DIR})

# Add root dir so qualified includes work.
# E.g. #include "cachelib/allocator/foobar.h"
include_directories(${CACHELIB_HOME}/..)
include_directories(${CACHELIB_BUILD}/..)
include_directories(${CACHELIB_BUILD})

# Set directory of the Find$x.cmake files to properly include dependencies
set(CMAKE_MODULE_PATH
  "${CACHELIB_HOME}/cmake"
  ${CMAKE_MODULE_PATH})

# specify the C++ standard
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  message(STATUS "setting C++ standard to C++${CMAKE_CXX_STANDARD}")
endif()
set(CMAKE_CXX_EXTENSIONS OFF)

# include(fb_cxx_flags)
message(STATUS "Update CXXFLAGS: ${CMAKE_CXX_FLAGS}")


set(CMAKE_THREAD_PREFER_PTHREAD ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

find_package(Boost REQUIRED COMPONENTS
             system filesystem regex context program_options thread)
find_package(Gflags REQUIRED)
find_package(Glog REQUIRED)
find_package(GTest REQUIRED)
include(GoogleTest)
enable_testing()
find_package(folly CONFIG REQUIRED)
find_package(fizz CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(wangle CONFIG REQUIRED)
find_package(Zlib REQUIRED)
find_package(Zstd REQUIRED)
find_package(FBThrift REQUIRED) # must come after wangle
find_package(NUMA REQUIRED)
find_package(Sparsemap REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(Xxhash REQUIRED)

find_package(uring)
if (NOT uring_FOUND)
  add_definitions(-DCACHELIB_IOURING_DISABLE)
endif()

# FBThrift - generates h/cpp files from .*thrift files
find_program(FBTHRIFT1 thrift1
  DOC "Facebook's fork of apache thrift")
if(NOT FBTHRIFT1)
    message(FATAL_ERROR "thrift1 not found!")
endif()


add_custom_target(thrift_generated_files)


set_property(GLOBAL PROPERTY TEST_BINARIES)

function (generic_add_source_test TEST_PREFIX SOURCE_FILE)
  # Get the basename of the first source file
  get_filename_component(TEST_NAME "${SOURCE_FILE}" NAME_WE)

  #message(STATUS "Adding test: ${TEST_PREFIX}-${TEST_NAME}"
  #" source: ${SOURCE_FILE} additional libraries: ${ARGN}")

  add_executable("${TEST_PREFIX}-${TEST_NAME}" "${SOURCE_FILE}")
  target_link_libraries("${TEST_PREFIX}-${TEST_NAME}" "${ARGN}")

  install(
    TARGETS "${TEST_PREFIX}-${TEST_NAME}"
    DESTINATION ${TESTS_INSTALL_DIR}
  )

  get_property(tmp GLOBAL PROPERTY TEST_BINARIES)
  set(tmp "${tmp} \\\n\t${TEST_PREFIX}-${TEST_NAME}")
  set_property(GLOBAL PROPERTY TEST_BINARIES "${tmp}")

  # make tests visible to ctest
  gtest_add_tests(TARGET "${TEST_PREFIX}-${TEST_NAME}"
                        TEST_PREFIX "${TEST_PREFIX}-${TEST_NAME}."
                        TEST_LIST "test_cases")
  # use same timeout as folly
  set_tests_properties(${test_cases} PROPERTIES TIMEOUT 300)
endfunction()


#add_custom_command(
#  OUTPUT
#    gen-cpp2/BloomFilter_constants.cpp
#    gen-cpp2/BloomFilter_constants.h
#    gen-cpp2/BloomFilter_data.cpp
#    gen-cpp2/BloomFilter_data.h
#    gen-cpp2/BloomFilter_metadata.cpp
#    gen-cpp2/BloomFilter_metadata.h
#    gen-cpp2/BloomFilter_types.cpp
#    gen-cpp2/BloomFilter_types_custom_protocol.h
#    gen-cpp2/BloomFilter_types.h
#    gen-cpp2/BloomFilter_types.tcc
#    gen-cpp2/BloomFilter_layouts.cpp
#    gen-cpp2/BloomFilter_layouts.h
#  COMMAND
#    ${FBTHRIFT1} -o ${CMAKE_CURRENT_BINARY_DIR} --gen mstch_cpp2:frozen2
#                 -I ${CACHELIB_HOME} BloomFilter.thrift
#  DEPENDS
#    BloomFilter.thrift
#  WORKING_DIRECTORY
#    ${CMAKE_CURRENT_SOURCE_DIR}
#)
#add_custom_target(common_thrift_files
#  DEPENDS
#     gen-cpp2/BloomFilter_constants.cpp
#     #${XBLOOM_THRIFT_FILES}
#  )
#add_dependencies(thrift_generated_files common_thrift_files)
function(add_thrift_file PREFIX THRIFT_RELATIVE_PATH CPP_OPTION)

  # Get the basename of the thrift file
  get_filename_component(THRIFT_BASENAME "${THRIFT_RELATIVE_PATH}" NAME)

  # Get the basename of the thrift file (without extension)
  get_filename_component(THRIFT_BASENAME_NO_EXT
                         "${THRIFT_RELATIVE_PATH}" NAME_WE)

  # Get the relative-directory of the thrift file (if any)
  get_filename_component(THRIFT_REL_DIR "${THRIFT_RELATIVE_PATH}" DIRECTORY)

  #message(STATUS "add-thrift:  PREFIX: ${PREFIX} REL_PATH:"
  #" ${THRIFT_RELATIVE_PATH}   BASENAME: ${THRIFT_BASENAME}"
  #"  REL_DIR: ${THRIFT_REL_DIR} current_source_dir: "
  # "${CMAKE_CURRENT_SOURCE_DIR}   cur-bin-dir: ${CMAKE_CURRENT_BINARY_DIR}")


  if (THRIFT_REL_DIR)
    # Create the sub-directory in the binary directory
    # (e.g. for "allocator/serialize/objects.thrift", create the "serizlize"
    # under "build/allocator/serialize" , where the "gen-cpp2" directory will
    # be created). Reldir might be empty.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${THRIFT_REL_DIR}")

    # HACK ALERT, see below
    string(APPEND THRIFT_REL_DIR "/")
  endif()

  # Generate a list of expected output files.
  # NOTE: the list is tightly-coupled with the cpp flavor
  # generated by 'thrift1'.
  set(${PREFIX}_THRIFT_FILES "constants.cpp"
    "constants.h"
    "data.cpp"
    "data.h"
    "metadata.cpp"
    "metadata.h"
    "types.cpp"
    "types_custom_protocol.h"
    "types.h"
    "types.tcc"
    "types_binary.cpp"
    "types_compact.cpp"
    "types_serialization.cpp"
    )

  # The thrift cpp option "frozen2"
  # (e.g. "thrift1 --gen mstch_cpp2:frozen2 ...")
  # generates two additional files. The "json" option does not.
  if (CPP_OPTION STREQUAL "frozen2")
    list(APPEND ${PREFIX}_THRIFT_FILES
      "layouts.h"
      "layouts.cpp")
  endif()


  string(LENGTH "${CMAKE_BINARY_DIR}" "FOOJ")
  #message(STATUS "*** FOOJ: ${FOOJ}")
  string(SUBSTRING "${CMAKE_CURRENT_BINARY_DIR}" "${FOOJ}" -1 "FOOP")
  #message(STATUS "*** FOOP: ${FOOP}")
  string(CONCAT FOOM "${CMAKE_BINARY_DIR}"
                     "/cachelib" "${FOOP}/${THRIFT_REL_DIR}")
  #message(STATUS "*** FOOM: ${FOOM}")
  string(CONCAT FOOX "${FOOM}" "gen-cpp2")
  #message(STATUS "*** FOOX: ${FOOX}")
  message(STATUS "*** OUTDIR: ${CMAKE_CURRENT_BINARY_DIR}/${THRIFT_REL_DIR} ")
  file(MAKE_DIRECTORY "${FOOM}")

  #message(STATUS "thrift suffixes: ${${PREFIX}_THRIFT_FILES}")

  #HACK ALERT: there is NO slash after "${THRIFT_REL_DIR}" as it might
  #  be empty. if it's not empty, the slash was added above.
  #list(TRANSFORM ${PREFIX}_THRIFT_FILES PREPEND
  #             "${THRIFT_REL_DIR}gen-cpp2/${THRIFT_BASENAME_NO_EXT}_")

  message(STATUS "before: ${${PREFIX}_THRIFT_FILES}")
  #string(REGEX REPLACE "([^;]+)"
  #       "${THRIFT_REL_DIR}gen-cpp2/${THRIFT_BASENAME_NO_EXT}_\\1"
  #       TMP "${${PREFIX}_THRIFT_FILES}")
  string(REGEX REPLACE "([^;]+)" "${FOOX}/${THRIFT_BASENAME_NO_EXT}_\\1"
                       TMP "${${PREFIX}_THRIFT_FILES}")

  message(STATUS "after: ${TMP}")
  set(${PREFIX}_THRIFT_FILES "${TMP}")

  message(STATUS "thrift files: ${${PREFIX}_THRIFT_FILES}")

  add_custom_command(
    OUTPUT
       ${${PREFIX}_THRIFT_FILES}
    COMMAND
       ${FBTHRIFT1} -o ${FOOM} --gen mstch_cpp2:${CPP_OPTION}
                    -I "${FBTHRIFT_INCLUDE_DIR}" -I ${CACHELIB_HOME}/..
                    ${THRIFT_BASENAME}
    DEPENDS
       ${THRIFT_RELATIVE_PATH}
   WORKING_DIRECTORY
       ${CMAKE_CURRENT_SOURCE_DIR}/${THRIFT_REL_DIR}
   )

 add_custom_target(${PREFIX}_thrift_target
   DEPENDS ${${PREFIX}_THRIFT_FILES})

 add_dependencies(thrift_generated_files ${PREFIX}_thrift_target)

 # HACK ALERT: transfer the list of files to the parent's (caller's) scope
 set(${PREFIX}_THRIFT_FILES "${${PREFIX}_THRIFT_FILES}" PARENT_SCOPE)
endfunction()

add_thrift_file(OBJECT_CACHE_PERSISTENCE
                object_cache/persistence/persistent_data.thrift json)

if (BUILD_WITH_DTO)
  # DSA offload runtime (DTO, https://github.com/intel/DTO). Resolved here so
  # every cachelib library can use it: DTO::dto_explicit is linked PUBLIC into
  # cachelib_common (see common/CMakeLists.txt), the base library of every
  # other cachelib target, which propagates the headers, the link dependency,
  # and the CACHELIB_BUILD_WITH_DTO compile definition to the whole tree and
  # to consumers of the installed package.
  #
  # The DSA checksum offload code uses DTO's explicit asynchronous API
  # (caller-allocated submit/poll operations, batches, raw CRC32C), which
  # exists from DTO 1.2 on. An installed DTO package of at least that version
  # is used when present; otherwise the pinned release is fetched and built
  # in-tree rather than linking whatever libdto happens to be on the system.
  set(CACHELIB_DTO_GIT_REPOSITORY "https://github.com/intel/DTO.git"
      CACHE STRING "Git repository for DTO when no installed copy is found")
  # Tag v1.2.
  set(CACHELIB_DTO_GIT_TAG "6ef831d9204f9156b1e816c7b603e7f0718193ce"
      CACHE STRING "DTO revision to fetch when no installed copy is found")

  find_package(DTO 1.2 CONFIG QUIET)
  if (DTO_FOUND)
    message(STATUS "Using installed DTO package: ${DTO_DIR} "
      "(version ${DTO_VERSION})")
  else()
    if (CMAKE_VERSION VERSION_LESS 3.14)
      message(FATAL_ERROR
        "BUILD_WITH_DTO needs an installed DTO cmake package or "
        "CMake >= 3.14 to fetch one. Install DTO from "
        "${CACHELIB_DTO_GIT_REPOSITORY} at ${CACHELIB_DTO_GIT_TAG}, "
        "or upgrade CMake.")
    endif()
    # DTO links against libaccel-config and libnuma; check for them here so
    # a missing system package fails at configure time with a clear message
    # instead of at link time inside the fetched project.
    find_library(ACCEL_CONFIG_LIBRARY accel-config)
    find_library(NUMA_LIBRARY numa)
    if (NOT ACCEL_CONFIG_LIBRARY OR NOT NUMA_LIBRARY)
      message(FATAL_ERROR
        "BUILD_WITH_DTO requires the libaccel-config and libnuma "
        "development packages (e.g. apt install libaccel-config-dev "
        "libnuma-dev)")
    endif()
    message(STATUS "DTO not installed; fetching "
      "${CACHELIB_DTO_GIT_REPOSITORY} @ ${CACHELIB_DTO_GIT_TAG}")
    include(FetchContent)
    FetchContent_Declare(dto
      GIT_REPOSITORY "${CACHELIB_DTO_GIT_REPOSITORY}"
      GIT_TAG "${CACHELIB_DTO_GIT_TAG}")
    FetchContent_MakeAvailable(dto)
  endif()
endif()

add_subdirectory (common)
add_subdirectory (shm)
add_subdirectory (navy)
add_subdirectory (allocator)
add_subdirectory (interface)
add_subdirectory (datatype)
add_subdirectory (compact_cache)
add_subdirectory (benchmarks)
add_subdirectory (cachebench)

# Install the source header files
install(
  DIRECTORY
    allocator
    common
    compact_cache
    datatype
    interface
    navy
    shm
  DESTINATION ${INCLUDE_INSTALL_DIR}
  FILES_MATCHING PATTERN "*.h"
  PATTERN "external" EXCLUDE
  PATTERN "tests" EXCLUDE
  PATTERN "benchmarks" EXCLUDE
  PATTERN "cachebench" EXCLUDE
  PATTERN "scripts" EXCLUDE
  PATTERN "cmake" EXCLUDE
)

# Install the thrift-generated header files
install(
  DIRECTORY
    ${CACHELIB_BUILD}/cachelib/
  DESTINATION
    ${INCLUDE_INSTALL_DIR}
  FILES_MATCHING
    PATTERN "gen-cpp2/*.h"
    PATTERN "gen-cpp2/*.tcc"
    PATTERN "tests" EXCLUDE
    PATTERN "*.dir" EXCLUDE
    PATTERN "CMakeFiles" EXCLUDE
)


# Install CMake package configuration files for cachelib
# TODO: @ONLY?
include(CMakePackageConfigHelpers)
configure_package_config_file(
  cmake/cachelib-config.cmake.in
  cachelib-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_DIR}
  PATH_VARS
    INCLUDE_INSTALL_DIR
    CMAKE_INSTALL_DIR
)
install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/cachelib-config.cmake
  DESTINATION ${CMAKE_INSTALL_DIR}
)

install(
  DIRECTORY ${CACHELIB_HOME}/cachebench/test_configs/
  DESTINATION ${CONFIGS_INSTALL_DIR}
)

add_library(cachelib INTERFACE)
target_link_libraries(cachelib INTERFACE
     cachelib_common
     cachelib_shm
     cachelib_navy
     cachelib_allocator
     cachelib_interface
     )
target_include_directories(
  cachelib
  INTERFACE
    $<INSTALL_INTERFACE:${INCLUDE_INSTALL_DIR}>
    )
target_compile_features(cachelib_common INTERFACE cxx_std_20)

install(TARGETS
     cachelib
     EXPORT cachelib-exports
     DESTINATION ${LIB_INSTALL_DIR}
  )

install(EXPORT cachelib-exports
        FILE cachelib-targets.cmake
        #NAMESPACE cachelib::
        DESTINATION ${CMAKE_INSTALL_DIR})

if (BUILD_SHARED_LIBS)
  set_target_properties(
    cachelib_allocator
    cachelib_cachebench
    cachelib_common
    cachelib_datatype
    cachelib_interface
    cachelib_navy
    cachelib_nvmitem
    cachelib_shm
    PROPERTIES
    SOVERSION ${CACHELIB_MAJOR_VERSION}
    VERSION ${PACKAGE_VERSION}
  )
endif ()

if (BUILD_TESTS)
  get_property(TEST_BINARIES GLOBAL PROPERTY TEST_BINARIES)
  #message(STATUS "=== Test binaries : ${TEST_BINARIES} ===")
  configure_file(cmake/Makefile.tests.in Makefile.tests @ONLY)
  install(
    FILES
      ${CACHELIB_BUILD}/Makefile.tests
    DESTINATION
      ${TESTS_INSTALL_DIR}
    RENAME
      Makefile
  )
endif()
