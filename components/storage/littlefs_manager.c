#include "littlefs_manager.h"
#include "app_config.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "LFS";

// 简单的文件序号管理（实际项目可存NVS持久化）
static uint32_t s_file_counter = 1;

// 递归互斥锁：保护所有 LittleFS 文件操作（支持嵌套调用）
static SemaphoreHandle_t s_lfs_mutex = NULL;

static inline void lfs_lock(void)
{
    xSemaphoreTakeRecursive(s_lfs_mutex, portMAX_DELAY);
}

static inline void lfs_unlock(void)
{
    xSemaphoreGiveRecursive(s_lfs_mutex);
}

esp_err_t littlefs_init(void)
{
    ESP_LOGI(TAG, "Initializing LittleFS");

    esp_vfs_littlefs_conf_t conf = {
        .base_path = LITTLEFS_BASE_PATH,
        .partition_label = LITTLEFS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format LittleFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "LittleFS partition not found");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    // 创建递归互斥锁（VFS 挂载成功后）
    s_lfs_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_lfs_mutex) {
        ESP_LOGE(TAG, "Failed to create LFS mutex");
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LittleFS info: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "LittleFS mounted: total=%uKB, used=%uKB", total/1024, used/1024);
    }

    // 扫描已有文件，恢复文件序号到最大值+1，避免覆盖旧文件
    DIR *dir = opendir(LITTLEFS_BASE_PATH);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strstr(entry->d_name, FILE_NAME_PREFIX) && strstr(entry->d_name, FILE_NAME_SUFFIX)) {
                uint32_t num = strtoul(entry->d_name + strlen(FILE_NAME_PREFIX), NULL, 10);
                if (num >= s_file_counter) {
                    s_file_counter = num + 1;
                }
            }
        }
        closedir(dir);
        ESP_LOGI(TAG, "File counter restored to %lu", (unsigned long)s_file_counter);
    }

    return ESP_OK;
}

void littlefs_get_next_filename(char *buf, size_t buf_len)
{
    lfs_lock();
    snprintf(buf, buf_len, "%s/%s%03lu%s",
             LITTLEFS_BASE_PATH, FILE_NAME_PREFIX, s_file_counter++, FILE_NAME_SUFFIX);
    lfs_unlock();
}

// 简单的文件名比较函数（用于排序）
static int _file_name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

char **littlefs_get_pending_uploads(size_t *out_count)
{
    *out_count = 0;
    lfs_lock();
    DIR *dir = opendir(LITTLEFS_BASE_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", LITTLEFS_BASE_PATH);
        lfs_unlock();
        return NULL;
    }

    char **file_list = NULL;
    size_t count = 0;
    struct dirent *entry;

    // 先扫描一遍统计数量
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, FILE_NAME_PREFIX) && strstr(entry->d_name, FILE_NAME_SUFFIX)) {
            count++;
        }
    }
    rewinddir(dir);

    if (count == 0) {
        closedir(dir);
        lfs_unlock();
        return NULL;
    }

    // 分配列表内存
    file_list = (char **)malloc(count * sizeof(char *));
    if (!file_list) {
        closedir(dir);
        lfs_unlock();
        return NULL;
    }

    // 填充列表
    count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, FILE_NAME_PREFIX) && strstr(entry->d_name, FILE_NAME_SUFFIX)) {
            size_t path_len = strlen(LITTLEFS_BASE_PATH) + strlen(entry->d_name) + 2;
            file_list[count] = (char *)malloc(path_len);
            if (!file_list[count]) {
                // 分配失败：清理已分配的资源
                for (size_t j = 0; j < count; j++) free(file_list[j]);
                free(file_list);
                closedir(dir);
                lfs_unlock();
                *out_count = 0;
                return NULL;
            }
            snprintf(file_list[count], path_len, "%s/%s", LITTLEFS_BASE_PATH, entry->d_name);
            count++;
        }
    }
    closedir(dir);
    lfs_unlock();

    // 按文件名排序（确保按录制顺序上传）— 不涉及文件系统操作，无需持锁
    qsort(file_list, count, sizeof(char *), _file_name_cmp);

    *out_count = count;
    return file_list;
}

esp_err_t littlefs_delete_file(const char *filepath)
{
    lfs_lock();
    if (remove(filepath) == 0) {
        ESP_LOGI(TAG, "Deleted file: %s", filepath);
        lfs_unlock();
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to delete file: %s", filepath);
        lfs_unlock();
        return ESP_FAIL;
    }
}

void littlefs_cleanup_old_files(void)
{
    lfs_lock();
    size_t count = 0;
    char **file_list = littlefs_get_pending_uploads(&count);
    if (!file_list || count == 0) {
        if (file_list) free(file_list);
        lfs_unlock();
        return;
    }

    // 如果超过最大数量，删除最旧的（列表前面的）
    while (count > MAX_RECORDING_FILES) {
        littlefs_delete_file(file_list[0]);
        // 简单的移除第一个元素（实际项目可优化）
        free(file_list[0]);
        for (size_t i = 0; i < count - 1; i++) {
            file_list[i] = file_list[i + 1];
        }
        count--;
    }

    // 释放列表内存
    for (size_t i = 0; i < count; i++) {
        free(file_list[i]);
    }
    free(file_list);
    lfs_unlock();
}