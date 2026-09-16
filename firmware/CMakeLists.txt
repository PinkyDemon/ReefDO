cmake_minimum_required(VERSION 3.24)

# ReefDO firmware for the Waveshare ESP32-S3-Relay-6CH. Build with ESP-IDF:
#   tools\idfenv.cmd idf.py -C firmware set-target esp32s3      (once)
#   tools\idfenv.cmd idf.py -C firmware build
#   tools\idfenv.cmd idf.py -C firmware -p COM3 flash monitor
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# One version string for host and board: core/include/reefdo/version.hpp
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../core/include/reefdo/version.hpp" _ver_line REGEX "VERSION = ")
string(REGEX MATCH "\"([^\"]+)\"" _ver_quoted "${_ver_line}")
string(REPLACE "\"" "" PROJECT_VER "${_ver_quoted}")
message(STATUS "ReefDO version ${PROJECT_VER}")
project(reefdo)
