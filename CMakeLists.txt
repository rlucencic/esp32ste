# Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7" (target esp32s3, the default)
# or M5Stack Tab5 (target esp32p4: idf.py -B build-tab5 -DIDF_TARGET=esp32p4
# -DSDKCONFIG=build-tab5/sdkconfig ...)
cmake_minimum_required(VERSION 3.16)

# The shared board runtime and the 68000 core: ../esp32emu (with its copies of
# ESP-IDF's esp_lcd and fatfs for ESP-IDF 5 or 6)
include("${CMAKE_CURRENT_LIST_DIR}/../esp32emu/components.cmake")

# The Tab5: the board's settings from ../esp32emu after this project's
if(IDF_TARGET STREQUAL "esp32p4" OR "$ENV{IDF_TARGET}" STREQUAL "esp32p4")
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
                           "${CMAKE_CURRENT_LIST_DIR}/../esp32emu/boards/m5stack_tab5/sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
# ESP-IDF 6 solves the components on its own: its own lock files, so going
# back and forth between ESP-IDF versions doesn't solve them again each time
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(lock_suffix ".idf6")
endif()
if(IDF_TARGET STREQUAL "esp32p4")
    # its own components (the BSP, esp_hosted, ...): its own lock file
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.esp32p4${lock_suffix}")
elseif(lock_suffix)
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock${lock_suffix}")
endif()
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

# A local AHDI template, kept out of both git and the application binary.
file(GLOB ahdi_image CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/roms/ahdi.img")
if(ahdi_image)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ahdi_image}")
    file(SIZE "${ahdi_image}" ahdi_size)
    if(NOT ahdi_size EQUAL 524288)
        message(FATAL_ERROR "roms/ahdi.img must be 512 KiB. See roms/README.md to prepare it.")
    endif()
    esptool_py_flash_to_partition(flash ahdi "${ahdi_image}")
endif()
