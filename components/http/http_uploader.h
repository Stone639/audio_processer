#ifndef _HTTP_UPLOADER_H_
#define _HTTP_UPLOADER_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 上传单个文件
esp_err_t http_upload_file(const char *filepath);

// 批量上传所有本地待传文件（传完自动删除）
void http_upload_all_pending(void);

#ifdef __cplusplus
}
#endif

#endif