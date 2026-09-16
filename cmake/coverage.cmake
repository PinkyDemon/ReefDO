# Invoked by the `coverage` target:
#   cmake -DTEST_EXE=... -DLLVM_BIN=... -DOUT_DIR=... -DSOURCE_ROOT=... -P coverage.cmake
# Runs the instrumented test binary, merges the raw profile, produces a text report and an
# HTML tree, and fails unless core/src (+ headers) is at 100 % lines and 100 % branches.

foreach(v TEST_EXE LLVM_BIN OUT_DIR SOURCE_ROOT)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "coverage.cmake: ${v} not set")
  endif()
endforeach()

file(MAKE_DIRECTORY "${OUT_DIR}")
set(raw "${OUT_DIR}/reefdo.profraw")
set(profdata "${OUT_DIR}/reefdo.profdata")
file(REMOVE "${raw}" "${profdata}")

set(ENV{LLVM_PROFILE_FILE} "${raw}")
execute_process(COMMAND "${TEST_EXE}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "coverage: tests failed (exit ${rc}) — fix the tests before measuring coverage")
endif()

execute_process(COMMAND "${LLVM_BIN}/llvm-profdata" merge -sparse "${raw}" -o "${profdata}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "coverage: llvm-profdata merge failed (${rc})")
endif()

file(GLOB_RECURSE sources "${SOURCE_ROOT}/src/*.cpp" "${SOURCE_ROOT}/include/*.hpp")

execute_process(
  COMMAND "${LLVM_BIN}/llvm-cov" report "${TEST_EXE}" "-instr-profile=${profdata}" ${sources}
  OUTPUT_VARIABLE report RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "coverage: llvm-cov report failed (${rc})")
endif()
file(WRITE "${OUT_DIR}/report.txt" "${report}")

execute_process(
  COMMAND "${LLVM_BIN}/llvm-cov" show "${TEST_EXE}" "-instr-profile=${profdata}"
          -format=html "-output-dir=${OUT_DIR}/html" -show-branches=count ${sources}
  RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(WARNING "coverage: llvm-cov show (html) failed (${rc}); report.txt is still valid")
endif()

message("${report}")

string(REGEX MATCH "TOTAL[^\n]*" total "${report}")
if(NOT total)
  message(FATAL_ERROR "coverage: no TOTAL line in llvm-cov output")
endif()
# TOTAL columns: Regions Missed Cover | Functions Missed Executed | Lines Missed Cover | Branches Missed Cover
string(REGEX MATCHALL "[0-9]+\\.[0-9]+%" pcts "${total}")
list(LENGTH pcts n)
if(n LESS 4)
  message(FATAL_ERROR "coverage: unexpected TOTAL line: ${total}")
endif()
list(GET pcts 2 lines)
list(GET pcts 3 branches)

if(NOT lines STREQUAL "100.00%" OR NOT branches STREQUAL "100.00%")
  message(FATAL_ERROR
    "coverage gate FAILED: lines ${lines}, branches ${branches} (required 100.00% / 100.00%)\n"
    "  details: ${OUT_DIR}/report.txt and ${OUT_DIR}/html/index.html")
endif()
message(STATUS "coverage gate passed: lines ${lines}, branches ${branches}")
