# ESP-IDF component wrapper around gateway/. Same sources and flags as the host build.
get_filename_component(REEFDO_GATEWAY_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../gateway" ABSOLUTE)
include(${REEFDO_GATEWAY_DIR}/sources.cmake)

set(_srcs)
foreach(s ${REEFDO_GATEWAY_SOURCES})
  list(APPEND _srcs "${REEFDO_GATEWAY_DIR}/${s}")
endforeach()

idf_component_register(
  SRCS ${_srcs}
  INCLUDE_DIRS "${REEFDO_GATEWAY_DIR}/include"
  REQUIRES reefdo_core)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++23 -fno-exceptions -fno-rtti -Wall -Wextra -Werror)
