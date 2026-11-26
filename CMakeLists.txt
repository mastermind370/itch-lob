cmake_minimum_required(VERSION 3.20)
project(itch_lob LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ITCH_NATIVE "Tune for the build machine's CPU" ON)
option(ITCH_BUILD_TESTS "Build unit tests (fetches GoogleTest if not installed)" ON)
set(ITCH_LADDER_WINDOW 2048 CACHE STRING "Fast-book price window in ticks (multiple of 64); see results/REPORT.md")

find_package(Threads REQUIRED)

add_library(itch INTERFACE)
target_include_directories(itch INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(itch INTERFACE Threads::Threads)
target_compile_definitions(itch INTERFACE ITCH_LADDER_WINDOW=${ITCH_LADDER_WINDOW})
target_compile_options(itch INTERFACE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)

if(ITCH_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-march=native HAS_MARCH_NATIVE)
  check_cxx_compiler_flag(-mcpu=native HAS_MCPU_NATIVE)
  if(HAS_MARCH_NATIVE)
    target_compile_options(itch INTERFACE -march=native)
  elseif(HAS_MCPU_NATIVE)  # Apple Silicon
    target_compile_options(itch INTERFACE -mcpu=native)
  endif()
endif()

foreach(tool itch_stats itch_slice gen_synthetic book_replay engine_bench pipeline_bench ablation)
  add_executable(${tool} tools/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE itch)
endforeach()

if(ITCH_BUILD_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(unit_tests tests/test_parser.cpp tests/test_book.cpp tests/test_fast_book.cpp tests/test_engine.cpp tests/test_pipeline.cpp)
  target_link_libraries(unit_tests PRIVATE itch GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(unit_tests)
endif()
