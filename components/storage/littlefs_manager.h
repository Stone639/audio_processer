#ifndef _LITTLEFS_MANAGER_H_
#define _LITTLEFS_MANAGER_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LITTLEFS_BASE_PATH       "/littlefs"
#define LITTLEFS_PARTITION_LABEL "storage"
#define FILE_NAME_PREFIX          "rec_"
#define FILE_NAME_SUFFIX          ".wav"

// 初始化并挂载LittleFS
esp_err_t littlefs_init(void);

// 生成下一个可用的文件名（按序号递增）
void littlefs_get_next_filename(char *buf, size_t buf_len);

// 扫描本地所有未上传的录音文件，返回文件路径列表（需调用者free）
char **littlefs_get_pending_uploads(size_t *out_count);

// 删除指定文件
esp_err_t littlefs_delete_file(const char *filepath);

// 检查并清理旧文件（超过MAX_RECORDING_FILES时删最旧的）
void littlefs_cleanup_old_files(void);

#ifdef __cplusplus
}
#endif

#endif