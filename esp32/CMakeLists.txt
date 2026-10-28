cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32s3)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Firmware version from tags named esp32/vX.Y.Z: "esp32-X.Y.Z" on the tag,
# "esp32-X.Y.Z-N-gHASH" N commits later, "-dirty" with uncommitted changes,
# and "esp32-0.0.0-gHASH" before the first tag. Other tags (server/v...,
# pi/v...) are ignored. The prefix tells it apart from Pi versions in the web
# UI. Shown on the setup page and reported to the server.
find_package(Git QUIET)
set(DIGITS_VERSION "0.0.0")
set(DIGITS_COMMIT "")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --match "esp32/v*" --dirty
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE described
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE described_failed)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE DIGITS_COMMIT
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT described_failed AND described MATCHES "^esp32/v(.+)$")
        set(DIGITS_VERSION "${CMAKE_MATCH_1}")
    elseif(DIGITS_COMMIT)
        execute_process(COMMAND "${GIT_EXECUTABLE}" diff --quiet HEAD
                        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" RESULT_VARIABLE dirty)
        set(DIGITS_VERSION "0.0.0-g${DIGITS_COMMIT}")
        if(dirty)
            string(APPEND DIGITS_VERSION "-dirty")
        endif()
    endif()
    # Re-run this on commits, checkouts and new tags (not on mere edits).
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --git-dir WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    OUTPUT_VARIABLE git_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    get_filename_component(git_dir "${git_dir}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    foreach(watched HEAD logs/HEAD packed-refs)
        if(EXISTS "${git_dir}/${watched}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${git_dir}/${watched}")
        endif()
    endforeach()
endif()
string(PREPEND DIGITS_VERSION "esp32-")
string(LENGTH "${DIGITS_VERSION}" version_length)
if(version_length GREATER 31)
    message(WARNING "Version ${DIGITS_VERSION} exceeds the app description's 31 characters and is cut")
endif()
set(PROJECT_VER "${DIGITS_VERSION}")
idf_build_set_property(COMPILE_DEFINITIONS "DIGITS_COMMIT=\"${DIGITS_COMMIT}\"" APPEND)
message(STATUS "Digits ESP32 firmware version ${DIGITS_VERSION}")

project(digits_signaling)

# Caller-selectable ringtone clips: esp32/ringtones/NN-Name.<audio> are packed
# into the "ringtones" partition image and flashed with the app (regular
# `idf.py flash`). See ringtones/README.md and tools/pack_ringtones.py.
idf_build_get_property(python PYTHON)
file(GLOB ringtone_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ringtones/*")
# Removing a file must repack too: the listing file changes (and is
# rewritten) only when the folder's file list does.
string(REPLACE ";" "\n" ringtone_listing "${ringtone_files}")
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/ringtone_files.txt" CONTENT "${ringtone_listing}\n")
partition_table_get_partition_info(ringtone_partition_size "--partition-name ringtones" "size")
set(ringtone_image "${CMAKE_BINARY_DIR}/ringtones.bin")
add_custom_command(
    OUTPUT "${ringtone_image}"
    COMMAND "${python}" "${CMAKE_SOURCE_DIR}/tools/pack_ringtones.py"
            --dir "${CMAKE_SOURCE_DIR}/ringtones" --out "${ringtone_image}"
            --partition-size "${ringtone_partition_size}"
    DEPENDS ${ringtone_files} "${CMAKE_BINARY_DIR}/ringtone_files.txt"
            "${CMAKE_SOURCE_DIR}/tools/pack_ringtones.py"
    COMMENT "Packing ringtone clips"
    VERBATIM)
add_custom_target(ringtone_image ALL DEPENDS "${ringtone_image}")
esptool_py_flash_to_partition(flash "ringtones" "${ringtone_image}")
add_dependencies(flash ringtone_image)
