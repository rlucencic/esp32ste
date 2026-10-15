idf_component_register(SRCS "main.c" "settings.c" "tos_rom.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES emu atari_st esp_partition nvs_flash esp_psram)
