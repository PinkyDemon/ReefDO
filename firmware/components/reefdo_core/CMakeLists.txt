# ESP-IDF component wrapper around core/. Same sources, same flags, same JSON knobs as the host build.
get_filename_component(REEFDO_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../core" ABSOLUTE)
get_filename_component(REEFDO_ROOT "${REEFDO_CORE_DIR}/.." ABSOLUTE)
include(${REEFDO_CORE_DIR}/sources.cmake)

set(_srcs)
foreach(s ${REEFDO_CORE_SOURCES})
  list(APPEND _srcs "${REEFDO_CORE_DIR}/${s}")
endforeach()

idf_component_register(
  SRCS ${_srcs}
  INCLUDE_DIRS "${REEFDO_CORE_DIR}/include" "${REEFDO_ROOT}/third_party/arduinojson")

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++23 -fno-exceptions -fno-rtti -Wall -Wextra -Werror)
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${REEFDO_JSON_DEFS})
