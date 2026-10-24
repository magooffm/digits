cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32s3)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
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
