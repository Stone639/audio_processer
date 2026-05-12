#include "voice_recognition.h"
#include "driver/i2s_std.h"
#include "wav_encoder.h"
#include "littlefs_manager.h"
#include "app_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define I2S_PORT            I2S_NUM_0
#define RING_BUFFER_SAMPLES (AUDIO_SAMPLE_RATE * RING_BUFFER_SECONDS)

static const char *TAG = "VR";

// 环形缓冲区（PSRAM）
static int16_t *ring_buffer = NULL;
static volatile uint32_t ring_write_idx = 0;

// 增量保存：跟踪上次保存位置
static uint32_t last_saved_write_idx = 0;

// 积累缓冲区（PSRAM）：积累到 UPLOAD_ACCUMULATE_SECONDS 后一次性构建 WAV
static int16_t *accum_buffer = NULL;
static uint32_t accum_samples = 0;
static uint32_t accum_capacity = 0;  // UPLOAD_ACCUMULATE_SECONDS * AUDIO_SAMPLE_RATE

// I2S
static i2s_chan_handle_t rx_handle = NULL;
static int active_channel = 0;

// --- 环形缓冲区操作 ---

static inline void ring_push_sample(int16_t sample)
{
    ring_buffer[ring_write_idx % RING_BUFFER_SAMPLES] = sample;
    ring_write_idx++;
}

static inline int16_t ring_get_history(uint32_t age)
{
    return ring_buffer[(ring_write_idx - 1 - age) % RING_BUFFER_SAMPLES];
}

// --- 声道检测 ---

static void detect_active_channel(const int32_t *stereo_data, int frame_count)
{
    int64_t left_energy = 0, right_energy = 0;
    for (int i = 0; i < frame_count; i++) {
        int32_t left = stereo_data[i * 2] >> 16;
        int32_t right = stereo_data[i * 2 + 1] >> 16;
        left_energy += (int64_t)left * left;
        right_energy += (int64_t)right * right;
    }
    active_channel = (right_energy > left_energy) ? 1 : 0;
    ESP_LOGI(TAG, "Channel detection: left_energy=%lld, right_energy=%lld, using %s channel",
             left_energy, right_energy, active_channel == 0 ? "LEFT" : "RIGHT");
}

// --- 初始化 ---

vr_error_t vr_init(void)
{
    // 1. 分配环形缓冲区（PSRAM）
    ring_buffer = heap_caps_malloc(RING_BUFFER_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!ring_buffer) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer (%d bytes)", RING_BUFFER_SAMPLES * (int)sizeof(int16_t));
        return VR_ERROR_INIT;
    }
    memset(ring_buffer, 0, RING_BUFFER_SAMPLES * sizeof(int16_t));
    ring_write_idx = 0;
    last_saved_write_idx = 0;
    ESP_LOGI(TAG, "Ring buffer: %d samples (%d KB), %d seconds",
             RING_BUFFER_SAMPLES, (int)(RING_BUFFER_SAMPLES * sizeof(int16_t) / 1024), RING_BUFFER_SECONDS);

    // 2. 分配积累缓冲区（PSRAM）
    accum_capacity = UPLOAD_ACCUMULATE_SECONDS * AUDIO_SAMPLE_RATE;
    accum_buffer = heap_caps_malloc(accum_capacity * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!accum_buffer) {
        ESP_LOGE(TAG, "Failed to allocate accumulation buffer");
        free(ring_buffer);
        ring_buffer = NULL;
        return VR_ERROR_INIT;
    }
    accum_samples = 0;
    ESP_LOGI(TAG, "Accumulation buffer: %d samples (%d KB), %d seconds target",
             accum_capacity, (int)(accum_capacity * sizeof(int16_t) / 1024), UPLOAD_ACCUMULATE_SECONDS);

    // 3. 配置 I2S 通道
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel");
        free(accum_buffer); accum_buffer = NULL;
        free(ring_buffer); ring_buffer = NULL;
        return VR_ERROR_INIT;
    }

    // 4. I2S 标准模式：立体声 32bit，兼容 INMP441 L/R 任意接法
    i2s_std_slot_config_t slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_PIN_BCK,
            .ws   = I2S_PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_PIN_DIN,
            .invert_flags = { false, false, false },
        },
    };
    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S standard mode");
        i2s_del_channel(rx_handle);
        free(accum_buffer); accum_buffer = NULL;
        free(ring_buffer); ring_buffer = NULL;
        return VR_ERROR_INIT;
    }

    // 5. 启用 I2S
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel");
        i2s_del_channel(rx_handle);
        free(accum_buffer); accum_buffer = NULL;
        free(ring_buffer); ring_buffer = NULL;
        return VR_ERROR_INIT;
    }

    ESP_LOGI(TAG, "I2S initialized, continuous recording ready");
    return VR_SUCCESS;
}

// --- 释放 ---

void vr_deinit(void)
{
    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }
    if (accum_buffer) {
        free(accum_buffer);
        accum_buffer = NULL;
    }
    if (ring_buffer) {
        free(ring_buffer);
        ring_buffer = NULL;
    }
    ring_write_idx = 0;
    last_saved_write_idx = 0;
    accum_samples = 0;
    active_channel = 0;
    ESP_LOGI(TAG, "Module deinitialized");
}

// --- 连续录音任务 ---

void vr_recording_task(void *pvParameters)
{
    int32_t i2s_raw[512];
    size_t bytes_read = 0;
    static bool channel_detected = false;

    // 预热 DMA
    int32_t warmup[256];
    size_t warmup_read;
    for (int i = 0; i < 3; i++) {
        i2s_channel_read(rx_handle, warmup, sizeof(warmup), &warmup_read, pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "DMA warmup done, continuous recording started");

    while (1) {
        esp_err_t ret = i2s_channel_read(rx_handle, i2s_raw, sizeof(i2s_raw), &bytes_read, pdMS_TO_TICKS(100));

        if (ret == ESP_OK && bytes_read > 0) {
            int total_samples = bytes_read / sizeof(int32_t);
            int frames = total_samples / 2;

            if (!channel_detected && frames >= 64) {
                detect_active_channel(i2s_raw, frames);
                channel_detected = true;
            }

            for (int i = 0; i < frames; i++) {
                int16_t sample = (int16_t)(i2s_raw[i * 2 + active_channel] >> 16);
                ring_push_sample(sample);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// --- 写 LittleFS 文件（队列满时的回退路径）---

static esp_err_t save_wav_to_littlefs(const int16_t *pcm_data, uint32_t num_samples)
{
    char filepath[64];
    littlefs_get_next_filename(filepath, sizeof(filepath));

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        return ESP_FAIL;
    }

    wav_encoder_write_header(f);
    wav_encoder_encode_data(f, pcm_data, (int)num_samples);
    wav_encoder_fix_header(f, (int)num_samples);
    fclose(f);

    ESP_LOGI(TAG, "Saved to file: %s (%lu samples)", filepath, num_samples);
    return ESP_OK;
}

// --- 定期保存任务 ---

void vr_save_task(void *pvParameters)
{
    QueueHandle_t upload_queue = (QueueHandle_t)pvParameters;

    // 等待环形缓冲区积累足够数据
    vTaskDelay(pdMS_TO_TICKS(RING_BUFFER_SECONDS * 1000));
    last_saved_write_idx = ring_write_idx;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(RECORD_INTERVAL_MS));

        // 增量提取：只取上次保存后新增的样本
        uint32_t current_write_idx = ring_write_idx;
        uint32_t new_samples = current_write_idx - last_saved_write_idx;
        if (new_samples == 0) continue;

        // clamp（防止极端情况下溢出）
        if (new_samples > RING_BUFFER_SAMPLES) {
            new_samples = RING_BUFFER_SAMPLES;
        }

        // 从环形缓冲区提取新样本（反转为时间顺序）
        for (uint32_t i = 0; i < new_samples; i++) {
            accum_buffer[accum_samples + i] = ring_get_history(new_samples - 1 - i);
        }
        accum_samples += new_samples;
        last_saved_write_idx = current_write_idx;

        // 积累未满，继续等待
        if (accum_samples < accum_capacity) continue;

        // 积累满：去除直流偏置
        int32_t sum = 0;
        for (uint32_t i = 0; i < accum_samples; i++) {
            sum += accum_buffer[i];
        }
        int16_t dc_offset = (int16_t)(sum / (int32_t)accum_samples);
        if (dc_offset != 0) {
            for (uint32_t i = 0; i < accum_samples; i++) {
                accum_buffer[i] -= dc_offset;
            }
        }

        // 在内存中构建 WAV
        size_t wav_size = 0;
        uint8_t *wav_buf = wav_encoder_build_buffer(accum_buffer, (int)accum_samples, &wav_size);

        if (wav_buf && upload_queue) {
            audio_upload_item_t item = { .wav_buf = wav_buf, .wav_size = wav_size };
            if (xQueueSend(upload_queue, &item, pdMS_TO_TICKS(100)) == pdPASS) {
                ESP_LOGI(TAG, "Queued %lu samples (%zu bytes) for upload", accum_samples, wav_size);
                wav_buf = NULL; // upload_task 负责 free
            } else {
                // 队列满：回退到 LittleFS 文件
                ESP_LOGW(TAG, "Upload queue full, falling back to file");
                save_wav_to_littlefs(accum_buffer, accum_samples);
                free(wav_buf);
                wav_buf = NULL;
            }
        } else if (wav_buf) {
            // 无队列：直接写文件
            save_wav_to_littlefs(accum_buffer, accum_samples);
            free(wav_buf);
            wav_buf = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to build WAV buffer (%lu samples)", accum_samples);
        }

        // 重置积累
        accum_samples = 0;
    }
}
