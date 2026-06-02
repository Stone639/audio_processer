#ifndef _HTTP_UPLOADER_H_
#define _HTTP_UPLOADER_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_client.h"

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

// --- 持久化 HTTP 客户端（keep-alive）API ---

// 创建启用 keep-alive 的持久化 HTTP 客户端，预设 Content-Type 和 Authorization
esp_http_client_handle_t http_uploader_create_client(void);

// 复用已有客户端上传 WAV 内存 buffer（不创建/销毁客户端）
esp_err_t http_upload_buffer_reuse(esp_http_client_handle_t client,
                                   const uint8_t *wav_buf, size_t wav_size,
                                   const char *filename);

// 复用已有客户端上传单个 WAV 文件
esp_err_t http_upload_file_reuse(esp_http_client_handle_t client,
                                  const char *filepath);

// 销毁由 http_uploader_create_client 创建的客户端
void http_uploader_cleanup_client(esp_http_client_handle_t client);

#ifdef __cplusplus
}
#endif

#endif
