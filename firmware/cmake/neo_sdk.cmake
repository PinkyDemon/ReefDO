# The ESP RainMaker Neo firmware SDK (the phone app, main/cloud_link.cpp), downloaded at configure time into
# firmware/external/. It is not on the ESP Component Registry, and its components include build files from the
# repository root, so the component manager cannot fetch it. Pinned by commit and SHA-256; ReefDO's patches
# (patches/esp-rainmaker-neo-firmware/*.patch) are applied once. On the first build the SDK clones its own
# esp-aws-iot libraries (git) next to it. A pin or patch change downloads it again; deleting firmware/external
# does too.
#   Source: https://github.com/espressif/esp-rainmaker-neo-firmware (Apache-2.0)
set(REEFDO_NEO_SDK_COMMIT "ff4a7946e5aea67386ebfc85a60303cec7210010") # 0.8.0, 2026-09-09
set(REEFDO_NEO_SDK_SHA256 "1076c22aac587126f7b903c8a5dd7d2655f4a3970c80242dad65f4e181fe60ce")
set(REEFDO_NEO_SDK_URL
    "https://github.com/espressif/esp-rainmaker-neo-firmware/archive/${REEFDO_NEO_SDK_COMMIT}.tar.gz")
get_filename_component(_neo_external "${CMAKE_CURRENT_LIST_DIR}/../external" ABSOLUTE)
set(REEFDO_NEO_SDK_DIR "${_neo_external}/esp-rainmaker-neo-firmware") # main/idf_component.yml refers to it

file(GLOB _neo_patches "${CMAKE_CURRENT_LIST_DIR}/../patches/esp-rainmaker-neo-firmware/*.patch")
list(SORT _neo_patches)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_neo_patches} "${CMAKE_CURRENT_LIST_FILE}")

# What the tree must be: this commit with these patches.
set(_neo_want "${REEFDO_NEO_SDK_COMMIT}")
foreach(_neo_patch IN LISTS _neo_patches)
    file(SHA256 "${_neo_patch}" _neo_hash)
    string(APPEND _neo_want " ${_neo_hash}")
endforeach()

file(MAKE_DIRECTORY "${_neo_external}")
file(LOCK "${_neo_external}/neo_sdk.lock" GUARD FILE TIMEOUT 600) # two builds configuring at once
set(_neo_stamp "${REEFDO_NEO_SDK_DIR}/.reefdo-stamp")
set(_neo_have "")
if(EXISTS "${_neo_stamp}")
    file(READ "${_neo_stamp}" _neo_have)
endif()
if(NOT _neo_have STREQUAL _neo_want)
    message(STATUS "ESP RainMaker Neo firmware SDK: downloading ${REEFDO_NEO_SDK_COMMIT}")
    set(_neo_tmp "${_neo_external}/neo_sdk.tmp")
    file(REMOVE_RECURSE "${REEFDO_NEO_SDK_DIR}" "${_neo_tmp}")
    file(DOWNLOAD "${REEFDO_NEO_SDK_URL}" "${_neo_tmp}/sdk.tar.gz" EXPECTED_HASH SHA256=${REEFDO_NEO_SDK_SHA256}
         TLS_VERIFY ON STATUS _neo_status)
    list(GET _neo_status 0 _neo_code)
    if(NOT _neo_code EQUAL 0)
        message(FATAL_ERROR "ESP RainMaker Neo firmware SDK: download failed (${_neo_status}): ${REEFDO_NEO_SDK_URL}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_neo_tmp}/sdk.tar.gz" DESTINATION "${_neo_tmp}")
    set(_neo_src "${_neo_tmp}/esp-rainmaker-neo-firmware-${REEFDO_NEO_SDK_COMMIT}")
    # The tree sits inside ReefDO's repository: without the ceiling, git apply would take the patch paths as
    # ReefDO's and skip them.
    find_package(Git REQUIRED)
    foreach(_neo_patch IN LISTS _neo_patches)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${_neo_tmp}" "${GIT_EXECUTABLE}"
                                apply --whitespace=nowarn "${_neo_patch}"
                        WORKING_DIRECTORY "${_neo_src}" RESULT_VARIABLE _neo_result ERROR_VARIABLE _neo_error)
        if(NOT _neo_result EQUAL 0)
            message(FATAL_ERROR "ESP RainMaker Neo firmware SDK: ${_neo_patch} does not apply:\n${_neo_error}")
        endif()
    endforeach()
    file(RENAME "${_neo_src}" "${REEFDO_NEO_SDK_DIR}")
    file(REMOVE_RECURSE "${_neo_tmp}")
    file(WRITE "${_neo_stamp}" "${_neo_want}")
endif()
file(LOCK "${_neo_external}/neo_sdk.lock" RELEASE)
