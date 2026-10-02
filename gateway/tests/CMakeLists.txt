# The gateway's tests join the one test binary (core/tests/CMakeLists.txt): one run, one coverage profile.
target_sources(reefdo_tests PRIVATE
  test_views.cpp
  test_router.cpp
  test_cloud.cpp
  test_platform.hpp
)
target_link_libraries(reefdo_tests PRIVATE reefdo_gateway)
