# The gateway: how every transport (HTTP, console, cloud) reaches the App. Hardware-independent like the core,
# under the same rules; the firmware builds the same sources as an ESP-IDF component: firmware/components/reefdo_gateway.

include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

add_library(reefdo_gateway STATIC ${REEFDO_GATEWAY_SOURCES})
target_include_directories(reefdo_gateway PUBLIC include)
target_link_libraries(reefdo_gateway PUBLIC reefdo_core)

if(MSVC)
  target_compile_options(reefdo_gateway PRIVATE /W4 /WX /EHs-c- /GR- /external:W0)
else()
  target_compile_options(reefdo_gateway PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
