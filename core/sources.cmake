# Shared between the host build (core/CMakeLists.txt) and the ESP-IDF component wrapper
# (firmware/components/reefdo_core/CMakeLists.txt). Paths are relative to core/.
set(REEFDO_CORE_SOURCES
  src/modbus.cpp
  src/solubility.cpp
  src/filter.cpp
  src/probe.cpp
  src/ladder.cpp
  src/config.cpp
  src/service.cpp
  src/log.cpp
  src/sim.cpp
  src/app.cpp
  src/api.cpp
)

# ArduinoJson (third_party, MIT) is the one library the core uses — only from config.cpp / api.cpp.
# Disable everything Arduino- or iostream-flavoured; string_view is the interface we want.
set(REEFDO_JSON_DEFS
  ARDUINOJSON_ENABLE_ARDUINO_STRING=0
  ARDUINOJSON_ENABLE_ARDUINO_STREAM=0
  ARDUINOJSON_ENABLE_ARDUINO_PRINT=0
  ARDUINOJSON_ENABLE_PROGMEM=0
  ARDUINOJSON_ENABLE_STD_STREAM=0
  ARDUINOJSON_ENABLE_STD_STRING=0
  ARDUINOJSON_ENABLE_STRING_VIEW=1
)
