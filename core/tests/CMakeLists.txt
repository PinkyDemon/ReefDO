# Catch2 v3 is vendored as the two amalgamated files (third_party/catch2) so the build
# never touches the network. Tests are built *with* exceptions (Catch2 needs them);
# the core they link against is not.

add_library(catch2 STATIC ${CMAKE_SOURCE_DIR}/third_party/catch2/catch_amalgamated.cpp)
target_include_directories(catch2 PUBLIC ${CMAKE_SOURCE_DIR}/third_party/catch2)
if(MSVC)
  target_compile_options(catch2 PRIVATE /EHsc /W3)
endif()

add_executable(reefdo_tests
  proptest.hpp
  fake_store.hpp
  scenario.hpp
  test_fixed_vector.cpp
  test_modbus.cpp
  test_solubility.cpp
  test_correction.cpp
  test_filter.cpp
  test_probe.cpp
  test_ladder.cpp
  test_ladder_properties.cpp
  test_config.cpp
  test_selftest.cpp
  test_boost.cpp
  test_log.cpp
  test_sim.cpp
  test_app.cpp
  test_tuya.cpp
  test_slot.cpp
)
target_include_directories(reefdo_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}) # the helpers, for gateway/tests too
target_link_libraries(reefdo_tests PRIVATE reefdo_core catch2)
if(MSVC)
  target_compile_options(reefdo_tests PRIVATE /W4 /WX /EHsc /external:W0)
else()
  target_compile_options(reefdo_tests PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME reefdo_tests COMMAND reefdo_tests)

if(REEFDO_COVERAGE)
  get_filename_component(_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  if(WIN32)
    # clang-cl does not pull the profile runtime in by itself
    file(GLOB _profile_rt "${_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
    if(NOT _profile_rt)
      message(FATAL_ERROR "clang_rt.profile-x86_64.lib not found next to ${CMAKE_CXX_COMPILER}")
    endif()
    target_link_libraries(reefdo_tests PRIVATE ${_profile_rt})
  else()
    target_link_options(reefdo_tests PRIVATE -fprofile-instr-generate)
  endif()
  # the coverage target itself is in the top-level CMakeLists.txt: it spans core/ and gateway/
endif()
