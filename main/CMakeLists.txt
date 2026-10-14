set(srcs "main.c" "console.c" "floppies.c" "lcd.c" "sdcard.c" "settings.c" "tos_rom.c" "touch.c")
if(CONFIG_ST_WIFI)
    list(APPEND srcs "net.c" "setup_ui.c" "lv_mem_psram.c")
endif()
if(CONFIG_ST_BLE_HID)
    list(APPEND srcs "ble_hid.c")
endif()
if(CONFIG_ST_SPEAKER)
    list(APPEND srcs "speaker.c")
endif()
set(embed "")
if(CONFIG_ST_WEB)
    list(APPEND srcs "web.c")
    list(APPEND embed "web/index.html")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       EMBED_FILES ${embed}
                       REQUIRES esp_driver_gpio
                       PRIV_REQUIRES esp_psram esp_timer esp_partition esp_lcd
                                     esp_driver_usb_serial_jtag vfs nvs_flash bt fatfs sdmmc
                                     esp_driver_sdmmc esp_driver_i2s esp_driver_uart esp_driver_i2c
                                     esp_wifi esp_netif esp_event lwip esp_http_server esp_driver_tsens atari_st)

if(CONFIG_ST_WIFI)
    # LVGL calls lv_mem_psram.c's allocator: make the linker take it from main
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")
endif()
