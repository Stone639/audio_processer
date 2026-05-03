#include "http_uploader.h"
#include "littlefs_manager.h"
#include "app_config.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>

static const char *TAG = "HTTP";

// 读取本地文件并通过 HTTPS POST 上传到 SiliconFlow 语音转文字 API
esp_err_t http_upload_file(const char *filepath)
{
    ESP_LOGI(TAG, "Uploading file: %s", filepath);

    // 打开文件
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", filepath);
        return ESP_FAIL;
    }

    // 获取文件大小
    struct stat file_stat;
    stat(filepath, &file_stat);
    size_t file_size = file_stat.st_size;

    // 配置HTTP客户端（HTTPS，使用证书包验证）
    esp_http_client_config_t config = {
        .url = UPLOAD_SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // 设置 Header
    const char *boundary = "----ESP32Boundary1234";
    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Authorization", UPLOAD_API_KEY);

    // 获取文件名
    const char *filename = strrchr(filepath, '/');
    filename = filename ? filename + 1 : "unknown.wav";

    // 构建 multipart body: model 字段 + file 字段
    char body_header[512];
    snprintf(body_header, sizeof(body_header),
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
             "%s\r\n"
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
             "Content-Type: audio/wav\r\n\r\n",
             boundary, UPLOAD_MODEL, boundary, filename);

    char body_footer[64];
    snprintf(body_footer, sizeof(body_footer), "\r\n--%s--\r\n", boundary);

    // 计算总长度并打开连接
    size_t total_len = strlen(body_header) + file_size + strlen(body_footer);
    esp_err_t err = esp_http_client_open(client, total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        fclose(f);
        esp_http_client_cleanup(client);
        return err;
    }

    // 1. 写入 multipart 头部（model + file disposition）
    int wlen = esp_http_client_write(client, body_header, strlen(body_header));
    if (wlen < (int)strlen(body_header)) {
        ESP_LOGE(TAG, "Failed to write HTTP header");
        fclose(f);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // 2. 写入文件内容（分块读，避免大内存占用）
    uint8_t buffer[1024];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        wlen = esp_http_client_write(client, (const char *)buffer, bytes_read);
        if (wlen < (int)bytes_read) {
            ESP_LOGE(TAG, "Failed to write file content");
            fclose(f);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }
    fclose(f);

    // 3. 写入 multipart 尾部
    wlen = esp_http_client_write(client, body_footer, strlen(body_footer));
    if (wlen < (int)strlen(body_footer)) {
        ESP_LOGE(TAG, "Failed to write HTTP footer");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // 4. 读取响应
    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    // 读取响应体（转文字结果）
    char response_buf[512] = {0};
    int content_length = esp_http_client_get_content_length(client);
    if (content_length > 0 && content_length < (int)sizeof(response_buf) - 1) {
        int read_len = esp_http_client_read(client, response_buf, content_length);
        if (read_len > 0) {
            response_buf[read_len] = '\0';
            ESP_LOGI(TAG, "Response: %s", response_buf);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_code == 200 || status_code == 201) {
        ESP_LOGI(TAG, "Upload success: %s", filepath);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Upload failed, status code: %d", status_code);
        return ESP_FAIL;
    }
}

void http_upload_all_pending(void)
{
    size_t count = 0;
    char **file_list = littlefs_get_pending_uploads(&count);
    if (!file_list || count == 0) {
        if (file_list) free(file_list);
        ESP_LOGI(TAG, "No pending files to upload");
        return;
    }

    ESP_LOGI(TAG, "Found %u pending files, starting upload...", count);

    for (size_t i = 0; i < count; i++) {
        if (http_upload_file(file_list[i]) == ESP_OK) {
            littlefs_delete_file(file_list[i]);
        } else {
            ESP_LOGW(TAG, "Upload failed, will retry later: %s", file_list[i]);
            break;
        }
        free(file_list[i]);
    }

    free(file_list);
}
