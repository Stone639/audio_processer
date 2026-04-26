#ifndef WAV_ENCODER_H
#define WAV_ENCODER_H

#include <stdint.h>
#include "ff.h"   // FatFS 头文件

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 写入 WAV 文件头（44 字节，占位）
 * @param file  FatFS 文件对象指针（已打开可写）
 */
void wav_encoder_write_header(FIL *file);

/**
 * @brief 写入 PCM 数据到 WAV 文件
 * @param file         FatFS 文件对象指针
 * @param pcm_buf      int16_t PCM 数据缓冲区
 * @param sample_count 采样点数
 */
void wav_encoder_encode_data(FIL *file, const int16_t *pcm_buf, int sample_count);

/**
 * @brief 回填 WAV 文件头中的大小字段（必须在数据写入后调用）
 * @param file  FatFS 文件对象指针
 * @param sample_count 总采样点数
 */
void wav_encoder_fix_header(FIL *file, int sample_count);

#ifdef __cplusplus
}
#endif

#endif