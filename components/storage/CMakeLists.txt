idf_component_register(
    SRCS  "sdcard.c" 
    INCLUDE_DIRS "."
    REQUIRES
        driver
        fatfs
        esp_driver_sdmmc
        esp_driver_sdspi
)