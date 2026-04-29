#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdcard.h"
#include "voice_recognition.h"

// ==================== 引脚定义 ====================
// SD 卡 SPI 引脚（根据图片：CS=38, MOSI=35, MISO=37, CLK=36）
#define SD_CS      38
#define SD_MOSI    35
#define SD_MISO    37
#define SD_CLK     36
#define MOUNT_POINT "/sdcard"

// I2S 麦克风引脚（INMP441）
// 注意：这些引脚已在 voice_recognition.c 中定义，此处仅作注释说明
// BCK = 47, WS = 10, DIN = 21

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "System starting...");

    // 1. 挂载 SD 卡
    ESP_LOGI(TAG, "Mounting SD card (SPI mode)...");
    esp_err_t ret = sdcard_mount(SD_CS, SD_MOSI, SD_MISO, SD_CLK, MOUNT_POINT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed, aborting");
        return;
    }
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);

    // 2. 初始化麦克风（I2S）
    ESP_LOGI(TAG, "Initializing microphone (INMP441)...");
    if (vr_init() != VR_SUCCESS) {
        ESP_LOGE(TAG, "Microphone init failed");
        sdcard_unmount();
        return;
    }
    ESP_LOGI(TAG, "Microphone ready");

    // 3. 创建录音任务（后台持续读取 I2S 数据并存入缓冲区）
    xTaskCreate(vr_recording_task, "recording_task", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "Recording task created");

    // 4. 录音示例：录音 5 秒并保存到 SD 卡
    const char *filename = MOUNT_POINT "/recording.wav";
    ESP_LOGI(TAG, "Start recording for 5 seconds...");
    vr_start_recording();
    vTaskDelay(pdMS_TO_TICKS(5000));   // 录音 5 秒
    vr_stop_and_save(filename);
    ESP_LOGI(TAG, "Recording saved to %s", filename);

    // 5. 可选：再录一段 3 秒
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Start recording for 3 seconds...");
    vr_start_recording();
    vTaskDelay(pdMS_TO_TICKS(3000));
    vr_stop_and_save(MOUNT_POINT "/recording2.wav");
    ESP_LOGI(TAG, "Second recording saved");

    // 6. 保持系统运行，让后续可以继续操作（例如通过命令触发录音）
    // 实际项目中可以在这里添加 HTTP 服务器或命令循环
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 注意：正常不会执行到这里，若需要完全退出可调用：
    // vr_deinit();
    // sdcard_unmount();
}