#ifndef _HTTP_UPLOADER_H_
#define _HTTP_UPLOADER_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 从内存 buffer 上传 WAV（用于队列直传路径）
esp_err_t http_upload_buffer(const uint8_t *wav_buf, size_t wav_size, const char *filename);

// 上传单个文件（用于回退路径）
esp_err_t http_upload_file(const char *filepath);

// 合并所有待传文件为单次上传（减少 TLS 握手）
void http_upload_merged_pending(void);

// 逐个上传所有待传文件（回退路径）
void http_upload_all_pending(void);

#ifdef __cplusplus
}
#endif

#endif
