#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "wifi_manager.h"
#include "voice_recognition.h"
#include "littlefs_manager.h"
#include "http_uploader.h"
#include "transcription.h"
#include "app_config.h"

static const char *TAG = "APP";

// 内存直传队列：vr_save_task → upload_task
static QueueHandle_t s_upload_queue = NULL;

// 独立上传任务：先处理内存队列，再合并上传 LittleFS 遗留文件
void upload_task(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t)pvParameters;

    // 等待 WiFi 连接
    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Upload task: WiFi connected");

    while (1) {
        // 1. 优先处理内存直传队列
        audio_upload_item_t item;
        while (xQueueReceive(queue, &item, 0) == pdPASS) {
            char name[32];
            static uint32_t upload_seq = 0;
            snprintf(name, sizeof(name), "live_%04lu.wav", upload_seq++);
            if (http_upload_buffer(item.wav_buf, item.wav_size, name) == ESP_OK) {
                ESP_LOGI(TAG, "Memory upload success: %s", name);
            } else {
                ESP_LOGW(TAG, "Memory upload failed: %s", name);
            }
            free(item.wav_buf);
        }

        // 2. 处理 LittleFS 中的遗留文件（合并上传）
        http_upload_merged_pending();

        vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTERVAL_MS));
    }
}

void main_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Main task started");

    // 1. 初始化 LittleFS
    if (littlefs_init() != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS init failed");
        vTaskDelete(NULL);
        return;
    }

    // 2. 初始化转文字结果模块
    if (transcription_init() != ESP_OK) {
        ESP_LOGE(TAG, "Transcription init failed");
        vTaskDelete(NULL);
        return;
    }

    // 3. 初始化 WiFi
    wifi_manager_init();

    // 4. 创建上传队列
    s_upload_queue = xQueueCreate(8, sizeof(audio_upload_item_t));
    if (!s_upload_queue) {
        ESP_LOGE(TAG, "Failed to create upload queue");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Upload queue created (depth=8)");

    // 5. 初始化录音模块（环形缓冲区 + I2S）
    if (vr_init() != VR_SUCCESS) {
        ESP_LOGE(TAG, "VR init failed");
        vTaskDelete(NULL);
        return;
    }

    // 6. 启动连续录音任务（永不停止，优先级最高）
    xTaskCreate(vr_recording_task, "vr_rec", 8192, NULL, 6, NULL);

    // 7. 启动定期保存任务（增量提取 → 积累 → 内存直传或文件回退）
    xTaskCreate(vr_save_task, "vr_save", 8192, (void *)s_upload_queue, 5, NULL);

    // 8. 启动独立上传任务（先队列后文件，后台不阻塞保存）
    xTaskCreate(upload_task, "upload", 12288, (void *)s_upload_queue, 4, NULL);

    // 主任务空闲
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    xTaskCreate(main_task, "main_task", 8192, NULL, 5, NULL);
}
