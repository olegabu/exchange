cmake_minimum_required(VERSION 3.24)

project(exchange LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(EXCHANGE_BUILD_TESTS "Build exchange tests" ON)
option(EXCHANGE_BUILD_BENCHMARKS "Build exchange micro-benchmarks" ON)

# docs/spec.md §3.3. OFF: sequencer is a sibling checkout compiled inside
# this build tree. ON: a packaged sequencer found with find_package.
# The sequencer::* target names are identical either way, so nothing
# below this block knows which one it got.
option(EXCHANGE_USE_SEQUENCER_PACKAGE "Consume sequencer via find_package instead of add_subdirectory" OFF)
set(SEQUENCER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sequencer" CACHE PATH
  "Path to the sequencer checkout (add_subdirectory mode)")

# Same flags as sequencer's root, and for the same reasons: -Werror for
# our own code (vendored and generated headers are SYSTEM includes, so
# it does not reach them); LTO + strip in Release, because a plain
# Release binary against vcpkg's brpc is ~300 MB of DWARF otherwise.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()
include(CheckIPOSupported)
check_ipo_supported(RESULT _exchange_ipo_supported OUTPUT _exchange_ipo_error)
if(_exchange_ipo_supported AND CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_link_options(-s)
endif()

if(EXCHANGE_USE_SEQUENCER_PACKAGE)
  find_package(sequencer CONFIG REQUIRED)
else()
  if(NOT EXISTS "${SEQUENCER_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SEQUENCER_DIR=${SEQUENCER_DIR} has no CMakeLists.txt; "
      "point it at a sequencer checkout (CLAUDE.md: siblings under total-order/)")
  endif()
  # Its tests and benchmarks are its own business; EXCLUDE_FROM_ALL so
  # only what we link gets built.
  set(SEQUENCER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(SEQUENCER_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${SEQUENCER_DIR}" "${CMAKE_BINARY_DIR}/sequencer" EXCLUDE_FROM_ALL)
endif()

# find_package results are directory-scoped, so what our own targets
# link directly is found here regardless of how sequencer was obtained.
find_package(gflags CONFIG REQUIRED)
find_package(glog CONFIG REQUIRED)
find_package(Threads REQUIRED)
if(EXCHANGE_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)
endif()
if(EXCHANGE_BUILD_BENCHMARKS)
  find_package(benchmark CONFIG REQUIRED)
endif()

if(EXCHANGE_BUILD_TESTS OR EXCHANGE_BUILD_BENCHMARKS)
  enable_testing()
endif()

add_subdirectory(vendor/liquibook)

# The checked-in SBE codecs (docs/spec.md §3.2, §4). SYSTEM: generated
# code is not held to this repository's warning flags.
add_library(exchange_sbe INTERFACE)
add_library(exchange::sbe ALIAS exchange_sbe)
target_include_directories(exchange_sbe SYSTEM INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated>"
)
target_compile_features(exchange_sbe INTERFACE cxx_std_20)
add_subdirectory(src)
if(EXCHANGE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(EXCHANGE_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
