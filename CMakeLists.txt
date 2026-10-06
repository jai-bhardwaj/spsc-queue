cmake_minimum_required(VERSION 3.20)
project(spsc_queue LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Header-only: consumers just need the include directory.
add_library(spsc_queue INTERFACE)
target_include_directories(spsc_queue INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(spsc_queue INTERFACE cxx_std_20)

find_package(Threads REQUIRED)

option(SPSC_BUILD_TESTS "Build the test suite" ON)
option(SPSC_BUILD_BENCH "Build the benchmarks" ON)
option(SPSC_TSAN "Build the tests with ThreadSanitizer" OFF)

if(SPSC_BUILD_TESTS)
  add_executable(spsc_tests tests/test_queue.cpp)
  target_link_libraries(spsc_tests PRIVATE spsc_queue Threads::Threads)
  target_compile_options(spsc_tests PRIVATE -Wall -Wextra -Wpedantic)

  if(SPSC_TSAN)
    # TSan needs -O1 and frame pointers to report anything readable.
    target_compile_options(spsc_tests PRIVATE -fsanitize=thread -O1 -g -fno-omit-frame-pointer)
    target_link_options(spsc_tests PRIVATE -fsanitize=thread)
  endif()

  enable_testing()
  add_test(NAME spsc_tests COMMAND spsc_tests)
endif()

if(SPSC_BUILD_BENCH)
  add_executable(spsc_bench bench/bench.cpp)
  target_link_libraries(spsc_bench PRIVATE spsc_queue Threads::Threads)
  # Benchmarks built without -O2 produce meaningless numbers, so force it here
  # regardless of the build type the caller picked.
  target_compile_options(spsc_bench PRIVATE -O2 -Wall -Wextra)
endif()
