#include "wav_encoder.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "WAV_ENCODER";

typedef struct {
    uint8_t  riff_id[4];
    uint32_t file_size;
    uint8_t  wave_id[4];
    uint8_t  fmt_id[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint8_t  data_id[4];
    uint32_t data_size;
} __attribute__((packed)) wav_header_t;

// 打开原始PCM文件（LittleFS 路径，如 "/rec_raw.pcm"）
void pcm_raw_file_open(FILE *pcm_file, const char *path)
{
    // 以二进制写模式打开，覆盖已有文件（LittleFS 兼容）
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open PCM raw file: %s", path);
        *pcm_file = *f; // 传递空指针
        return;
    }
    *pcm_file = *f;
    ESP_LOGI(TAG, "PCM raw file opened successfully: %s", path);
}

// 写入PCM原始数据到LittleFS文件
void pcm_raw_file_write(FILE *pcm_file, const int16_t *pcm_buf, int sample_count)
{
    if (pcm_file == NULL || pcm_buf == NULL || sample_count <= 0) {
        return;
    }

    size_t bytes_to_write = sample_count * sizeof(int16_t);
    size_t bytes_written = fwrite(pcm_buf, 1, bytes_to_write, pcm_file);

    if (bytes_written != bytes_to_write) {
        ESP_LOGE(TAG, "Failed to write PCM raw data: written=%zu/%zu",
                 bytes_written, bytes_to_write);
    } else {
        ESP_LOGD(TAG, "PCM raw data written: %zu bytes", bytes_written);
    }
}

// 写入WAV头（适配 FILE*）
void wav_encoder_write_header(FILE *file)
{
    if (!file) {
        ESP_LOGE(TAG, "Invalid FILE pointer for WAV header");
        return;
    }

    wav_header_t header;
    memset(&header, 0, sizeof(header));

    memcpy(header.riff_id, "RIFF", 4);
    header.file_size = 0;               // 稍后回填
    memcpy(header.wave_id, "WAVE", 4);
    memcpy(header.fmt_id, "fmt ", 4);
    header.fmt_size = 16;
    header.audio_format = 1;            // PCM 格式
    header.num_channels = AUDIO_CHANNELS;
    header.sample_rate = AUDIO_SAMPLE_RATE;
    header.byte_rate = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BIT_DEPTH / 8);
    header.block_align = AUDIO_CHANNELS * (AUDIO_BIT_DEPTH / 8);
    header.bits_per_sample = AUDIO_BIT_DEPTH;
    memcpy(header.data_id, "data", 4);
    header.data_size = 0;               // 稍后回填

    // 写入WAV头（44字节）
    size_t written = fwrite(&header, 1, sizeof(header), file);
    if (written != sizeof(header)) {
        ESP_LOGE(TAG, "Failed to write WAV header: written=%zu/%zu",
                 written, sizeof(header));
    }
}

// 写入PCM数据到WAV文件（适配 FILE*）
void wav_encoder_encode_data(FILE *file, const int16_t *pcm_buf, int sample_count)
{
    if (!file || !pcm_buf || sample_count <= 0) {
        ESP_LOGE(TAG, "Invalid params for WAV data encode");
        return;
    }

    size_t bytes_to_write = sample_count * sizeof(int16_t);
    size_t bytes_written = fwrite(pcm_buf, 1, bytes_to_write, file);

    if (bytes_written != bytes_to_write) {
        ESP_LOGE(TAG, "Failed to write WAV data: written=%zu/%zu",
                 bytes_written, bytes_to_write);
    }
}

// 回填WAV头大小（适配 FILE* 的 fseek/fwrite）
void wav_encoder_fix_header(FILE *file, int sample_count)
{
    if (!file || sample_count <= 0) {
        ESP_LOGE(TAG, "Invalid params for WAV header fix");
        return;
    }

    uint32_t data_size = sample_count * sizeof(int16_t);
    uint32_t file_size = data_size + 36;  // 总大小-8（RIFF头）

    // 1. 回填data_size（偏移40）
    fseek(file, 40, SEEK_SET);  // 标准IO偏移接口
    size_t written = fwrite(&data_size, 1, 4, file);
    if (written != 4) {
        ESP_LOGE(TAG, "Failed to fix WAV data size: written=%zu", written);
    }

    // 2. 回填file_size（偏移4）
    fseek(file, 4, SEEK_SET);
    written = fwrite(&file_size, 1, 4, file);
    if (written != 4) {
        ESP_LOGE(TAG, "Failed to fix WAV file size: written=%zu", written);
    }

    // 恢复文件指针到末尾（可选）
    fseek(file, 0, SEEK_END);
}

uint8_t* wav_encoder_build_buffer(const int16_t *pcm_buf, int sample_count, size_t *out_size)
{
    if (!pcm_buf || sample_count <= 0 || !out_size) return NULL;

    size_t data_size = sample_count * sizeof(int16_t);
    size_t total_size = 44 + data_size;

    uint8_t *buf = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;

    // 直接在 buf 开头写入 header（不依赖结构体，避免对齐问题）
    memset(buf, 0, 44);
    memcpy(buf + 0, "RIFF", 4);
    uint32_t file_size = data_size + 36;
    memcpy(buf + 4, &file_size, 4);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(buf + 16, &fmt_size, 4);
    uint16_t audio_format = 1;
    memcpy(buf + 20, &audio_format, 2);
    uint16_t num_channels = AUDIO_CHANNELS;
    memcpy(buf + 22, &num_channels, 2);
    uint32_t sample_rate = AUDIO_SAMPLE_RATE;
    memcpy(buf + 24, &sample_rate, 4);
    uint32_t byte_rate = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BIT_DEPTH / 8);
    memcpy(buf + 28, &byte_rate, 4);
    uint16_t block_align = AUDIO_CHANNELS * (AUDIO_BIT_DEPTH / 8);
    memcpy(buf + 32, &block_align, 2);
    uint16_t bits_per_sample = AUDIO_BIT_DEPTH;
    memcpy(buf + 34, &bits_per_sample, 2);
    memcpy(buf + 36, "data", 4);
    memcpy(buf + 40, &data_size, 4);

    // PCM 数据
    memcpy(buf + 44, pcm_buf, data_size);

    *out_size = total_size;
    return buf;
}