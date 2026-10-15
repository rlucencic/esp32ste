# Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7" (ESP32-S3-WROOM-1-N16R8)
cmake_minimum_required(VERSION 3.16)

# The shared board runtime and the 68000 core: ../esp32emu
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32emu/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32ste)

# TOS ROM images (not in git) go to their own partitions on "idf.py flash"
foreach(tos tos162 tos206)
    set(tos_image "${CMAKE_SOURCE_DIR}/roms/${tos}.img")
    if(EXISTS "${tos_image}")
        esptool_py_flash_to_partition(flash "${tos}" "${tos_image}")
    else()
        message(WARNING "${tos_image} missing: partition ${tos} will not be flashed")
    endif()
endforeach()
