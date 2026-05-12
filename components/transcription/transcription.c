#include "transcription.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "TRANS";

/* ---------- 内部配置，不对外暴露 ---------- */
#define TRANSCRIPTION_MAX_ENTRIES   20
#define TRANSCRIPTION_TEXT_MAX_LEN  256

/* ---------- 内部数据结构 ---------- */
typedef struct {
    char text[TRANSCRIPTION_TEXT_MAX_LEN];
    int64_t timestamp;
    char filename[32];
} transcription_entry_t;

static transcription_entry_t s_entries[TRANSCRIPTION_MAX_ENTRIES];
static int s_head  = 0;   // 下一个写入位置
static int s_count = 0;   // 当前缓存条数
static SemaphoreHandle_t s_mutex = NULL;

// 回调
static transcription_callback_t s_callback = NULL;
static void *s_callback_user_data = NULL;

/* ---------- 内部工具函数 ---------- */

static int64_t get_timestamp_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 向环形缓冲区写入一条结果（调用者需已持有锁）
static void ring_push(const char *text, const char *filename)
{
    transcription_entry_t *entry = &s_entries[s_head];
    strlcpy(entry->text, text, TRANSCRIPTION_TEXT_MAX_LEN);
    entry->timestamp = get_timestamp_ms();
    if (filename) {
        // 只取文件名部分，去掉路径
        const char *name = strrchr(filename, '/');
        name = name ? name + 1 : filename;
        strlcpy(entry->filename, name, sizeof(entry->filename));
    } else {
        entry->filename[0] = '\0';
    }

    s_head = (s_head + 1) % TRANSCRIPTION_MAX_ENTRIES;
    if (s_count < TRANSCRIPTION_MAX_ENTRIES) {
        s_count++;
    }
}

/* ---------- 对外接口实现 ---------- */

esp_err_t transcription_init(void)
{
    if (s_mutex) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }
    s_head  = 0;
    s_count = 0;
    memset(s_entries, 0, sizeof(s_entries));
    s_callback = NULL;
    s_callback_user_data = NULL;
    ESP_LOGI(TAG, "Initialized (max=%d, text_len=%d)",
             TRANSCRIPTION_MAX_ENTRIES, TRANSCRIPTION_TEXT_MAX_LEN);
    return ESP_OK;
}

void transcription_on_result(transcription_callback_t cb, void *user_data)
{
    s_callback = cb;
    s_callback_user_data = user_data;
}

int transcription_get_latest(char *buf, size_t buf_size)
{
    if (!s_mutex || !buf || buf_size == 0) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int len = 0;
    if (s_count > 0) {
        int idx = (s_head - 1 + TRANSCRIPTION_MAX_ENTRIES) % TRANSCRIPTION_MAX_ENTRIES;
        len = strlcpy(buf, s_entries[idx].text, buf_size);
    }
    xSemaphoreGive(s_mutex);
    return len;
}

int transcription_get_all(char *buf, size_t buf_size)
{
    if (!s_mutex || !buf || buf_size == 0) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_count == 0) {
        xSemaphoreGive(s_mutex);
        buf[0] = '\0';
        return 0;
    }

    // 用 cJSON 构建 JSON 数组
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        xSemaphoreGive(s_mutex);
        return 0;
    }

    // 从最旧到最新遍历
    int start = (s_count < TRANSCRIPTION_MAX_ENTRIES)
                ? 0
                : s_head;  // 缓冲区满时，最旧的是 head 位置
    for (int i = 0; i < s_count; i++) {
        int idx = (start + i) % TRANSCRIPTION_MAX_ENTRIES;
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            xSemaphoreGive(s_mutex);
            return 0;
        }
        cJSON_AddStringToObject(item, "text", s_entries[idx].text);
        cJSON_AddNumberToObject(item, "timestamp", (double)s_entries[idx].timestamp);
        cJSON_AddStringToObject(item, "filename", s_entries[idx].filename);
        cJSON_AddItemToArray(root, item);
    }

    xSemaphoreGive(s_mutex);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return 0;

    int len = snprintf(buf, buf_size, "%s", json_str);
    free(json_str);
    return len;
}

int transcription_get_count(void)
{
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_count;
    xSemaphoreGive(s_mutex);
    return count;
}

esp_err_t transcription_deinit(void)
{
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    s_head  = 0;
    s_count = 0;
    s_callback = NULL;
    s_callback_user_data = NULL;
    ESP_LOGI(TAG, "Deinitialized");
    return ESP_OK;
}

/* ---------- 内部接口：供 http_uploader 调用 ---------- */

esp_err_t transcription_parse_and_add(const char *json_response, const char *filename)
{
    if (!json_response || !s_mutex) return ESP_FAIL;

    cJSON *root = cJSON_Parse(json_response);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON response");
        return ESP_FAIL;
    }

    cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (!text_item || !cJSON_IsString(text_item) || !text_item->valuestring) {
        ESP_LOGW(TAG, "No 'text' field in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const char *text = text_item->valuestring;
    if (strlen(text) == 0) {
        ESP_LOGW(TAG, "Empty transcription text");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ring_push(text, filename);
    int64_t ts = s_entries[(s_head - 1 + TRANSCRIPTION_MAX_ENTRIES) % TRANSCRIPTION_MAX_ENTRIES].timestamp;
    xSemaphoreGive(s_mutex);

    // 取出文件名（不含路径）
    const char *name = filename ? strrchr(filename, '/') : NULL;
    name = name ? name + 1 : filename;

    ESP_LOGI(TAG, "Stored [%d]: %.64s%s",
             s_count, text,
             strlen(text) > 64 ? "..." : "");

    // 通知回调（锁外调用，避免回调中死锁；text 指向 cJSON 内部，需在 Delete 前调用）
    transcription_callback_t cb = s_callback;
    void *ud = s_callback_user_data;
    if (cb) {
        cb(text, ts, name, ud);
    }

    cJSON_Delete(root);
    return ESP_OK;
}
