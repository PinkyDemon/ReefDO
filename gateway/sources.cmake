# Shared between the host build (gateway/CMakeLists.txt) and the ESP-IDF component wrapper
# (firmware/components/reefdo_gateway/CMakeLists.txt). Paths are relative to gateway/.
set(REEFDO_GATEWAY_SOURCES
  src/views.cpp
  src/router.cpp
  src/cloud.cpp
)
