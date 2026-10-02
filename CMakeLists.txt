cmake_minimum_required(VERSION 3.24)
project(ReefDO VERSION 0.1.0 LANGUAGES CXX)

# Host build: the hardware-independent core and gateway, their tests and (later) the simulator.
# The firmware is a separate ESP-IDF project under firmware/ that consumes core/ and gateway/ as components.

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
add_subdirectory(gateway)
if(NOT REEFDO_COVERAGE)
  add_subdirectory(sim) # reefdo_sim: the App on the virtual tank, the page on localhost
endif()

if(REEFDO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/tests)    # defines reefdo_tests
  add_subdirectory(gateway/tests) # adds its sources to it

  if(REEFDO_COVERAGE)
    get_filename_component(_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    # `cmake --build --preset coverage` runs the tests instrumented, merges the profile,
    # writes coverage/report.txt + coverage/html/, and fails unless core/ and gateway/ are at
    # 100 % lines AND 100 % branches. Pure CMake script; no shell, no Python.
    add_custom_target(coverage
      COMMAND ${CMAKE_COMMAND}
        -DTEST_EXE=$<TARGET_FILE:reefdo_tests>
        -DLLVM_BIN=${_llvm_bin}
        -DOUT_DIR=${CMAKE_BINARY_DIR}/coverage
        -DSOURCE_ROOTS=${CMAKE_SOURCE_DIR}/core,${CMAKE_SOURCE_DIR}/gateway
        -P ${CMAKE_SOURCE_DIR}/cmake/coverage.cmake
      DEPENDS reefdo_tests
      USES_TERMINAL
      COMMENT "Running tests under llvm-cov and gating at 100 % line + branch coverage")
  endif()
endif()
