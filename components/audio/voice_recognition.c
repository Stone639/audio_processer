#include "voice_recognition.h"
#include "driver/i2s_std.h"
#include "wav_encoder.h"
#include "littlefs_manager.h"
#include "app_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_BUFFER_SIZE   (AUDIO_SAMPLE_RATE * (AUDIO_BIT_DEPTH / 8) * AUDIO_CHANNELS * (RECORD_DURATION_MS / 1000))
#define I2S_PORT            I2S_NUM_0

static const char *TAG = "VR";

static uint8_t *audio_buffer = NULL;
static size_t audio_buffer_size = 0;
static bool is_recording = false;
static i2s_chan_handle_t rx_handle = NULL;   // I2S 接收通道句柄

vr_error_t vr_init(void)
{
    // 1. 分配音频缓冲区
    audio_buffer = (uint8_t *)malloc(AUDIO_BUFFER_SIZE);
    if (!audio_buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        return VR_ERROR_INIT;
    }
    audio_buffer_size = 0;

    // 2. 配置 I2S 通道（仅接收，主模式）
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;          // DMA 描述符数量
    chan_cfg.dma_frame_num = 64;        // 每帧样本数（决定中断频率）
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel");
        free(audio_buffer);
        audio_buffer = NULL;
        return VR_ERROR_INIT;
    }

    // 3. 配置标准 I2S 模式参数（32位宽，适配 INMP441 的 24bit/32bit 帧格式）
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_PIN_BCK,
            .ws   = I2S_PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_PIN_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S standard mode");
        i2s_del_channel(rx_handle);
        free(audio_buffer);
        audio_buffer = NULL;
        return VR_ERROR_INIT;
    }

    // 4. 启用 I2S 通道
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel");
        i2s_del_channel(rx_handle);
        free(audio_buffer);
        audio_buffer = NULL;
        return VR_ERROR_INIT;
    }

    ESP_LOGI(TAG, "I2S initialized successfully");
    return VR_SUCCESS;
}

vr_error_t vr_start_recording(void)
{
    if (!audio_buffer) {
        ESP_LOGE(TAG, "Module not initialized");
        return VR_ERROR_INIT;
    }
    is_recording = true;
    audio_buffer_size = 0;
    ESP_LOGI(TAG, "Recording started");
    return VR_SUCCESS;
}

vr_error_t vr_stop_and_save_to_littlefs(void)
{
    if (!audio_buffer) return VR_ERROR_INIT;
    is_recording = false;
    if (audio_buffer_size == 0) return VR_ERROR_FILE;

    // 1. 生成LittleFS下的文件名
    char filepath[64];
    littlefs_get_next_filename(filepath, sizeof(filepath));

    // 2. 打开文件（LittleFS路径）
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        return VR_ERROR_FILE;
    }

    // 3. 写入WAV（复用你原来的wav_encoder逻辑）
    wav_encoder_write_header(f);
    int sample_count = audio_buffer_size / sizeof(int16_t);
    wav_encoder_encode_data(f, (const int16_t*)audio_buffer, sample_count);
    wav_encoder_fix_header(f, sample_count);

    fclose(f);
    ESP_LOGI(TAG, "WAV saved locally: %s", filepath);

    // 4. 检查并清理旧文件（防止存满）
    littlefs_cleanup_old_files();

    return VR_SUCCESS;
}

void vr_deinit(void)
{
    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }
    if (audio_buffer) {
        free(audio_buffer);
        audio_buffer = NULL;
    }
    audio_buffer_size = 0;
    is_recording = false;
    ESP_LOGI(TAG, "Module deinitialized");
}

// 录音任务（需在 app_main 中创建）
// I2S 以 32 位读取（适配 INMP441），转换为 16 位存入 audio_buffer
void vr_recording_task(void *pvParameters)
{
    int32_t i2s_raw[256];   // I2S 读取缓冲区（32位样本）
    size_t bytes_read = 0;
    esp_err_t ret;
    int timeout_count = 0;

    while (1) {
        if (is_recording && audio_buffer_size < AUDIO_BUFFER_SIZE) {
            // 计算本次最多读多少32位样本（留够16位存储空间）
            size_t max_raw = (AUDIO_BUFFER_SIZE - audio_buffer_size) / sizeof(int16_t);
            size_t read_bytes = max_raw * sizeof(int32_t);
            if (read_bytes > sizeof(i2s_raw)) read_bytes = sizeof(i2s_raw);

            ret = i2s_channel_read(rx_handle, i2s_raw, read_bytes, &bytes_read, pdMS_TO_TICKS(10));
            if (ret == ESP_OK && bytes_read > 0) {
                timeout_count = 0;
                // 32位转16位：取高16位（INMP441 有效数据在高位）
                int samples_read = bytes_read / sizeof(int32_t);
                int16_t *out = (int16_t *)(audio_buffer + audio_buffer_size);
                for (int i = 0; i < samples_read; i++) {
                    out[i] = (int16_t)(i2s_raw[i] >> 16);
                }
                audio_buffer_size += samples_read * sizeof(int16_t);
                if (audio_buffer_size >= AUDIO_BUFFER_SIZE) {
                    ESP_LOGI(TAG, "Audio buffer full (%d bytes), stopping recording", audio_buffer_size);
                    is_recording = false;
                }
            } else if (ret != ESP_OK) {
                timeout_count++;
                if (timeout_count <= 3) {
                    ESP_LOGW(TAG, "I2S read error: %s", esp_err_to_name(ret));
                } else if (timeout_count == 4) {
                    ESP_LOGW(TAG, "I2S read keeps failing, suppressing further logs. Check microphone wiring.");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}