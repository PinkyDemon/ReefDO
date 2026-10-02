# reefdo_sim: the real App on the virtual tank, the real web page on http://localhost:8080/, a controllable
# clock and fault injection. Host only; it links the same core and gateway as the firmware.

add_executable(reefdo_sim
  main.cpp
  file_store.cpp
  file_store.hpp
  http_server.cpp
  http_server.hpp
  controls.cpp
  controls.hpp
  demo.hpp
)
target_link_libraries(reefdo_sim PRIVATE reefdo_gateway)
target_compile_definitions(reefdo_sim PRIVATE REEFDO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(WIN32)
  target_link_libraries(reefdo_sim PRIVATE ws2_32)
endif()
if(MSVC)
  target_compile_options(reefdo_sim PRIVATE /W4 /WX /EHsc /external:W0)
  target_compile_definitions(reefdo_sim PRIVATE _CRT_SECURE_NO_WARNINGS) # plain, portable fopen
else()
  target_compile_options(reefdo_sim PRIVATE -Wall -Wextra -Werror)
  find_package(Threads REQUIRED)
  target_link_libraries(reefdo_sim PRIVATE Threads::Threads)
endif()
