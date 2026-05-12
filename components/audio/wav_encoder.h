#ifndef WAV_ENCODER_H
#define WAV_ENCODER_H

#include <stdint.h>
#include <stdio.h>  // 替换 FatFS 的 FIL 为标准 FILE

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 写入 WAV 文件头（44 字节，占位）
 * @param file  标准 FILE 指针（已打开可写，LittleFS 路径）
 */
void wav_encoder_write_header(FILE *file);

/**
 * @brief 写入 PCM 数据到 WAV 文件
 * @param file         标准 FILE 指针
 * @param pcm_buf      int16_t PCM 数据缓冲区
 * @param sample_count 采样点数
 */
void wav_encoder_encode_data(FILE *file, const int16_t *pcm_buf, int sample_count);

/**
 * @brief 回填 WAV 文件头中的大小字段（必须在数据写入后调用）
 * @param file  标准 FILE 指针
 * @param sample_count 总采样点数
 */
void wav_encoder_fix_header(FILE *file, int sample_count);

// 新增：PCM原始文件的句柄和函数声明（适配 LittleFS）
void pcm_raw_file_open(FILE *pcm_file, const char *path);
void pcm_raw_file_write(FILE *pcm_file, const int16_t *pcm_buf, int sample_count);

/**
 * @brief 在内存中构建完整 WAV（44 字节头 + PCM 数据），用于免文件上传
 * @param pcm_buf      int16_t PCM 数据
 * @param sample_count 采样点数
 * @param out_size     输出：WAV 总字节数
 * @return PSRAM 分配的 WAV 缓冲区，调用者负责 free；失败返回 NULL
 */
uint8_t* wav_encoder_build_buffer(const int16_t *pcm_buf, int sample_count, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif