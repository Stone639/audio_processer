idf_component_register(
    SRCS  
        "littlefs_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        littlefs
        esp_timer
        esp_http_client
        vfs
        config
)