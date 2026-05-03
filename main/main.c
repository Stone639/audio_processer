#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "wifi_manager.h"
#include "voice_recognition.h"
#include "littlefs_manager.h"
#include "http_uploader.h"
#include "app_config.h"

static const char *TAG = "APP";

// --------------------------
// 主任务：分段录音 + 触发上传
// --------------------------
void main_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Main task started");

    // 1. 初始化LittleFS
    if (littlefs_init() != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS init failed");
        vTaskDelete(NULL);
        return;
    }

    // 2. 初始化录音模块
    if (vr_init() != VR_SUCCESS) {
        ESP_LOGE(TAG, "VR init failed");
        vTaskDelete(NULL);
        return;
    }
    xTaskCreate(vr_recording_task, "vr_rec", 4096, NULL, 5, NULL);

    // 3. 等待WiFi连接（最多等10秒）
    ESP_LOGI(TAG, "Waiting for WiFi...");
    for (int i = 0; i < 20; i++) {
        if (wifi_manager_is_connected()) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (wifi_manager_is_connected()) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGW(TAG, "WiFi not ready, will retry in main loop");
    }

    // 4. 先尝试上传上次断电前遗留的文件
    http_upload_all_pending();

    // 4. 主循环：每3秒录一段，存本地，然后尝试上传
    while (1) {
        ESP_LOGI(TAG, "Starting new recording segment...");
        
        // 录一段
        vr_start_recording();
        vTaskDelay(pdMS_TO_TICKS(RECORD_DURATION_MS));
        
        // 停止并保存到LittleFS
        vr_stop_and_save_to_littlefs();

        // 尝试上传（如果有网就传，没网就留在本地下次）
        if (wifi_manager_is_connected()) {
            http_upload_all_pending();
        } else {
            ESP_LOGW(TAG, "WiFi not connected, skip upload");
        }
        ESP_LOGI(TAG, "Segment done, waiting for next cycle...\n");
        vTaskDelay(pdMS_TO_TICKS(RECORD_INTERVAL_MS));
    }
}

void app_main(void)
{
    // 初始化WiFi
    wifi_manager_init();

    // 启动主任务
    xTaskCreate(main_task, "main_task", 8192, NULL, 5, NULL);
}