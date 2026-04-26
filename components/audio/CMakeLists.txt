idf_component_register(
    SRCS
        "voice_recognition.c"
        "wav_encoder.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        fatfs           # 文件系统
        esp_timer
        # 移除 esp_http_client, json, mbedtls, nvs_flash (如果不需要)
)