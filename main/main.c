#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "voice_recognition.h"
#include "esp_log.h"

static const char *TAG = "WAV_TEST";

// SD 卡引脚配置（根据实际接线修改）
#define SD_CS_PIN    5
#define SD_MOSI_PIN  11
#define SD_MISO_PIN  13
#define SD_CLK_PIN   12
#define SD_MOUNT_POINT "/sdcard"

void app_main(void)
{
    esp_err_t ret;

    // 1. 初始化 SD 卡（必须先挂载文件系统，否则无法保存 WAV）
    ret = sdcard_mount(SD_CS_PIN, SD_MOSI_PIN, SD_MISO_PIN, SD_CLK_PIN, SD_MOUNT_POINT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed");
        return;
    }

    // 2. 初始化语音录音模块（I2S + 音频缓冲区）
    vr_error_t vr_ret = vr_init();
    if (vr_ret != VR_SUCCESS) {
        ESP_LOGE(TAG, "Voice recognition init failed: %d", vr_ret);
        sdcard_unmount();
        return;
    }

    // 3. 创建录音任务（后台读取 I2S 数据到缓冲区）
    xTaskCreate(vr_recording_task, "vr_rec", 4096, NULL, 5, NULL);
    if (xTaskGetHandle("vr_rec") == NULL) {
        ESP_LOGE(TAG, "Create recording task failed");
        vr_deinit();
        sdcard_unmount();
        return;
    }

    // 4. 开始录音（置位标志，后台任务开始填充缓冲区）
    vr_ret = vr_start_recording();
    if (vr_ret != VR_SUCCESS) {
        ESP_LOGE(TAG, "Start recording failed: %d", vr_ret);
        vr_deinit();
        sdcard_unmount();
        return;
    }

    // 5. 录制指定时长（示例：3 秒，可修改）
    ESP_LOGI(TAG, "Recording for 3 seconds...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    // 6. 停止录音并保存为 WAV 文件（路径为 SD 卡挂载点 + 文件名）
    const char *wav_path = "/sdcard/test_rec.wav";
    vr_ret = vr_stop_and_save(wav_path);
    if (vr_ret == VR_SUCCESS) {
        ESP_LOGI(TAG, "WAV file saved to: %s", wav_path);
    } else {
        ESP_LOGE(TAG, "Save WAV failed: %d", vr_ret);
    }

    // 7. 释放资源（测试完成后可选）
    vr_deinit();
    sdcard_unmount();
    ESP_LOGI(TAG, "Test completed");
}