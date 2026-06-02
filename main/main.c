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
#include "esp_heap_caps.h"

static const char *TAG = "APP";

// 内存直传队列：vr_save_task → upload_task
static QueueHandle_t s_upload_queue = NULL;

// LittleFS 积压处理：每轮最多处理文件数，避免阻塞实时上传
#define MAX_BACKLOG_PER_ROUND 2

// 独立上传任务：内存队列优先 + 限量 LittleFS 积压处理
void upload_task(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t)pvParameters;

    // 等待 WiFi 连接
    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Upload task: WiFi connected");

    // 持久化 HTTP 客户端：while 循环外创建，仅在上传失败后重建
    esp_http_client_handle_t client = http_uploader_create_client();
    ESP_LOGI(TAG, "Upload task: persistent HTTP client ready");

    static uint32_t upload_seq = 0;

    while (1) {
        audio_upload_item_t item;

        // 1. 阻塞等待内存直传队列（有数据立即返回，超时触发 LittleFS 处理）
        if (xQueueReceive(queue, &item,
                          pdMS_TO_TICKS(UPLOAD_INTERVAL_MS)) == pdPASS) {
            // 排空队列：一次唤醒处理所有积压的音频块
            do {
                if (!client) {
                    client = http_uploader_create_client();
                    ESP_LOGI(TAG, "Recreated HTTP client after failure");
                }

                char name[32];
                snprintf(name, sizeof(name), "live_%04lu.wav", upload_seq++);

                esp_err_t err = http_upload_buffer_reuse(client,
                                                         item.wav_buf,
                                                         item.wav_size,
                                                         name);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Memory upload success: %s", name);
                } else {
                    // 上传失败：先存 LittleFS，再释放 buffer，最后清理客户端
                    ESP_LOGW(TAG, "Memory upload failed: %s, saving to LittleFS", name);
                    save_wav_buffer_to_littlefs(item.wav_buf, item.wav_size, name);
                    http_uploader_cleanup_client(client);
                    client = NULL;
                }
                free(item.wav_buf);
            } while (xQueueReceive(queue, &item, 0) == pdPASS);
        }

        // 2. 限量处理 LittleFS 积压文件
        {
            size_t file_count = 0;
            char **file_list = littlefs_get_pending_uploads(&file_count);
            int processed = 0;

            for (size_t i = 0; i < file_count && processed < MAX_BACKLOG_PER_ROUND; i++) {
                // 有新的实时音频到达？立即中断积压处理
                if (uxQueueMessagesWaiting(queue) > 0) {
                    ESP_LOGI(TAG, "New real-time data, pausing backlog at %d/%d",
                             processed, MAX_BACKLOG_PER_ROUND);
                    break;
                }

                if (!client) {
                    client = http_uploader_create_client();
                    ESP_LOGI(TAG, "Recreated HTTP client for backlog");
                }

                ESP_LOGI(TAG, "Backlog upload %d/%d: %s",
                         processed + 1, MAX_BACKLOG_PER_ROUND, file_list[i]);

                esp_err_t err = http_upload_file_reuse(client, file_list[i]);
                if (err == ESP_OK) {
                    littlefs_delete_file(file_list[i]);
                } else {
                    ESP_LOGW(TAG, "Backlog upload failed: %s", file_list[i]);
                    http_uploader_cleanup_client(client);
                    client = NULL;
                }
                processed++;
            }

            if (processed > 0) {
                littlefs_cleanup_old_files();
            }

            if (file_list) {
                for (size_t i = 0; i < file_count; i++) free(file_list[i]);
                free(file_list);
            }
        }
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
    xTaskCreate(vr_recording_task, "vr_rec", 12288, NULL, 6, NULL);

    // 7. 启动定期保存任务（增量提取 → 积累 → 内存直传或文件回退）
    xTaskCreate(vr_save_task, "vr_save", 12288, (void *)s_upload_queue, 5, NULL);

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
