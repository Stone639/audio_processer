idf_component_register(
    SRCS  "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_i2s
        audio
        json
        storage
        joltwallet__littlefs
        esp_wifi
        nvs_flash
        http
        wifi
        config
)