#include "wav_encoder.h"
#include <string.h>

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