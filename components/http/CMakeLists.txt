idf_component_register(
    SRCS
       "http_uploader.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        littlefs
        esp_timer
        esp_http_client
        vfs
        storage
        config
        mbedtls
        transcription
)