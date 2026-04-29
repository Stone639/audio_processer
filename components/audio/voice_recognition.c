#include "voice_recognition.h"
#include "driver/i2s_std.h"
#include "wav_encoder.h"
#include "ff.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

#define AUDIO_BUFFER_SIZE   (16000 * 2 * 3)   // 3秒音频：16000采样/秒 * 2字节/采样 * 3秒 = 96000字节
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

    // 3. 配置标准 I2S 模式参数（16kHz, 16bit, 单声道）
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),          // 采样率 16000 Hz
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,    // 无需 MCLK
            .bclk = 47,                 // BCK 引脚
            .ws   = 10,                 // WS 引脚
            .dout = I2S_GPIO_UNUSED,    // 仅接收，不发送
            .din  = 21,                 // DATA_IN 引脚
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

vr_error_t vr_stop_and_save(const char *filename)
{
    if (!audio_buffer) {
        return VR_ERROR_INIT;
    }

    is_recording = false;

    if (audio_buffer_size == 0) {
        ESP_LOGW(TAG, "No audio data recorded");
        return VR_ERROR_FILE;
    }

    // 打开文件
    FIL file;
    FRESULT fr = f_open(&file, filename, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "Failed to open file '%s', error: %d", filename, fr);
        return VR_ERROR_FILE;
    }

    // 写入 WAV 头（占位）
    wav_encoder_write_header(&file);

    // 写入 PCM 数据
    int sample_count = audio_buffer_size / sizeof(int16_t);
    wav_encoder_encode_data(&file, (const int16_t*)audio_buffer, sample_count);

    // 回填头部长度字段
    wav_encoder_fix_header(&file, sample_count);

    f_close(&file);
    ESP_LOGI(TAG, "Audio saved to '%s', %d bytes", filename, audio_buffer_size);
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
void vr_recording_task(void *pvParameters)
{
    size_t bytes_read = 0;
    uint8_t *buffer_ptr = NULL;
    size_t remaining = 0;
    esp_err_t ret;

    while (1) {
        if (is_recording && audio_buffer_size < AUDIO_BUFFER_SIZE) {
            buffer_ptr = audio_buffer + audio_buffer_size;
            remaining = AUDIO_BUFFER_SIZE - audio_buffer_size;

            ret = i2s_channel_read(rx_handle, buffer_ptr, remaining, &bytes_read, pdMS_TO_TICKS(10));
            if (ret == ESP_OK && bytes_read > 0) {
                audio_buffer_size += bytes_read;
                if (audio_buffer_size >= AUDIO_BUFFER_SIZE) {
                    ESP_LOGW(TAG, "Audio buffer full, stopping recording automatically");
                    is_recording = false;   // 缓冲区满后自动停止
                }
            } else if (ret != ESP_OK) {
                ESP_LOGW(TAG, "I2S read error: %s", esp_err_to_name(ret));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}