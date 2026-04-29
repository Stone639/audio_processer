#include "wav_encoder.h"
#include "esp_log.h"
#include <string.h>

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


// 打开原始PCM文件（无任何头信息，纯数据）
/**
 * @brief 打开 PCM 原始音频文件用于写入
 * @param pcm_file 指向 FIL 结构体的指针，用于存储文件句柄
 * @param path 文件路径字符串
 * @note 如果文件已存在，将被覆盖（FA_CREATE_ALWAYS）
 * @note 不进行任何音频格式转换，直接写入原始 PCM 数据
 */
void pcm_raw_file_open(FIL *pcm_file, const char *path)
{
    FRESULT res = f_open(pcm_file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Failed to open PCM raw file: %d", res);
    } else {
        ESP_LOGI(TAG, "PCM raw file opened successfully: %s", path);
    }
}

// 写入PCM原始数据到文件
/**
 * @brief 将 PCM 原始数据写入文件
 * 
 * @param pcm_file 指向已打开的文件对象指针
 * @param pcm_buf PCM 数据缓冲区，每个样本为 16 位有符号整数
 * @param sample_count 要写入的 PCM 样本数量
 * 
 * @note 函数会校验参数有效性，无效时直接返回
 * @note 写入失败时会记录错误日志
 * @note 写入成功时在调试日志中记录写入字节数
 */
void pcm_raw_file_write(FIL *pcm_file, const int16_t *pcm_buf, int sample_count)
{
    if (pcm_file == NULL || pcm_buf == NULL || sample_count <= 0) {
        return;
    }

    UINT bytes_written = 0;
    size_t bytes_to_write = sample_count * sizeof(int16_t);

    FRESULT res = f_write(pcm_file, pcm_buf, bytes_to_write, &bytes_written);
    if (res != FR_OK || bytes_written != bytes_to_write) {
        ESP_LOGE(TAG, "Failed to write PCM raw data: res=%d, written=%u/%zu",
                 res, bytes_written, bytes_to_write);
    } else {
        ESP_LOGD(TAG, "PCM raw data written: %u bytes", bytes_written);
    }
}




void wav_encoder_write_header(FIL *file)
{
    wav_header_t header;
    UINT bytes_written;

    memcpy(header.riff_id, "RIFF", 4);
    header.file_size = 0;               // 稍后回填
    memcpy(header.wave_id, "WAVE", 4);
    memcpy(header.fmt_id, "fmt ", 4);
    header.fmt_size = 16;
    header.audio_format = 1;
    header.num_channels = 1;
    header.sample_rate = 16000;
    header.byte_rate = 16000 * 1 * 2;
    header.block_align = 1 * 2;
    header.bits_per_sample = 16;
    memcpy(header.data_id, "data", 4);
    header.data_size = 0;               // 稍后回填

    f_write(file, &header, sizeof(header), &bytes_written);
}

void wav_encoder_encode_data(FIL *file, const int16_t *pcm_buf, int sample_count)
{
    UINT bytes_written;
    f_write(file, pcm_buf, sample_count * sizeof(int16_t), &bytes_written);
}

void wav_encoder_fix_header(FIL *file, int sample_count)
{
    UINT bytes_written;
    uint32_t data_size = sample_count * sizeof(int16_t);
    uint32_t file_size = data_size + 36;   // 整个文件大小减去 8（RIFF 头已占用 4+4）
    // 注意：RIFF 块中的文件大小 = 总文件长度 - 8

    // 回填 data_size 到偏移 40
    f_lseek(file, 40);
    f_write(file, &data_size, 4, &bytes_written);
    // 回填 file_size 到偏移 4
    f_lseek(file, 4);
    f_write(file, &file_size, 4, &bytes_written);
}