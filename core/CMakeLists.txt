# The hardware-independent core as a host static library. No ESP-IDF header is ever included here.
# The firmware builds the same sources as an ESP-IDF component: firmware/components/reefdo_core.

include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
get_filename_component(REEFDO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_library(reefdo_core STATIC ${REEFDO_CORE_SOURCES})
target_include_directories(reefdo_core PUBLIC include)
target_include_directories(reefdo_core SYSTEM PUBLIC "${REEFDO_ROOT}/third_party/arduinojson")
target_compile_definitions(reefdo_core PUBLIC ${REEFDO_JSON_DEFS})
target_compile_features(reefdo_core PUBLIC cxx_std_23)

if(MSVC)
  # /EHs-c- = no exception unwinding, /GR- = no RTTI. Warnings are errors — but not from SYSTEM headers.
  target_compile_options(reefdo_core PRIVATE /W4 /WX /EHs-c- /GR- /external:W0)
else()
  target_compile_options(reefdo_core PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
