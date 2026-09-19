cmake_minimum_required(VERSION 3.24)
project(ReefDO VERSION 0.1.0 LANGUAGES CXX)

# Host build: the hardware-independent core, its tests and (later) the simulator.
# The firmware is a separate ESP-IDF project under firmware/ that consumes core/ as a component.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REEFDO_COVERAGE "Instrument with clang-cl for llvm-cov and add the 'coverage' target (100 % gate)" OFF)
option(REEFDO_BUILD_TESTS "Build the Catch2 test binary" ON)

if(MSVC)
  # CMake adds /EHsc by default for MSVC-like compilers; core is built without exceptions,
  # so strip the global default and let each target choose.
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  add_compile_options(/utf-8 /permissive-)
endif()

if(REEFDO_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "REEFDO_COVERAGE needs clang (presets clang-coverage on Windows, linux-coverage elsewhere)")
  endif()
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
endif()

add_subdirectory(core)

if(REEFDO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/tests)
endif()
