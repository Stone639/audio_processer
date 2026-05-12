#include "http_uploader.h"
#include "littlefs_manager.h"
#include "transcription.h"
#include "app_config.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "HTTP";

// --- 通用：HTTP multipart 上传的响应处理 ---

static esp_err_t http_do_upload(esp_http_client_handle_t client,
                                const char *body_header,
                                const uint8_t *file_data, size_t file_size,
                                const char *body_footer,
                                const char *log_name)
{
    // 计算总长度并打开连接
    size_t total_len = strlen(body_header) + file_size + strlen(body_footer);
    esp_err_t err = esp_http_client_open(client, total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        return err;
    }

    // 1. 写入 multipart 头部
    int wlen = esp_http_client_write(client, body_header, strlen(body_header));
    if (wlen < (int)strlen(body_header)) {
        ESP_LOGE(TAG, "Failed to write HTTP header");
        esp_http_client_close(client);
        return ESP_FAIL;
    }

    // 2. 写入文件内容（4KB 缓冲区分块写入）
    size_t offset = 0;
    uint8_t write_buf[4096];
    while (offset < file_size) {
        size_t chunk = file_size - offset;
        if (chunk > sizeof(write_buf)) chunk = sizeof(write_buf);
        memcpy(write_buf, file_data + offset, chunk);
        wlen = esp_http_client_write(client, (const char *)write_buf, chunk);
        if (wlen < (int)chunk) {
            ESP_LOGE(TAG, "Failed to write file content");
            esp_http_client_close(client);
            return ESP_FAIL;
        }
        offset += chunk;
    }

    // 3. 写入 multipart 尾部
    wlen = esp_http_client_write(client, body_footer, strlen(body_footer));
    if (wlen < (int)strlen(body_footer)) {
        ESP_LOGE(TAG, "Failed to write HTTP footer");
        esp_http_client_close(client);
        return ESP_FAIL;
    }

    // 4. 读取响应
    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    char response_buf[1024] = {0};
    int content_length = esp_http_client_get_content_length(client);
    if (content_length > 0 && content_length < (int)sizeof(response_buf) - 1) {
        int read_len = esp_http_client_read(client, response_buf, content_length);
        if (read_len > 0) {
            response_buf[read_len] = '\0';
            ESP_LOGI(TAG, "Response: %s", response_buf);
        }
    }

    esp_http_client_close(client);

    if (status_code == 200 || status_code == 201) {
        ESP_LOGI(TAG, "Upload success: %s", log_name);
        if (response_buf[0] != '\0') {
            transcription_parse_and_add(response_buf, log_name);
        }
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Upload failed, status code: %d", status_code);
        return ESP_FAIL;
    }
}

// --- 构建 multipart body header/footer ---

static void build_multipart_parts(const char *filename,
                                  char *header_out, size_t header_len,
                                  char *footer_out, size_t footer_len)
{
    const char *boundary = "----ESP32Boundary1234";
    snprintf(header_out, header_len,
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
             "%s\r\n"
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
             "Content-Type: audio/wav\r\n\r\n",
             boundary, UPLOAD_MODEL, boundary, filename);
    snprintf(footer_out, footer_len, "\r\n--%s--\r\n", boundary);
}

// --- 从内存 buffer 上传 ---

esp_err_t http_upload_buffer(const uint8_t *wav_buf, size_t wav_size, const char *filename)
{
    ESP_LOGI(TAG, "Uploading buffer: %s (%zu bytes)", filename, wav_size);

    esp_http_client_config_t config = {
        .url = UPLOAD_SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", "----ESP32Boundary1234");
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Authorization", UPLOAD_API_KEY);

    char body_header[512], body_footer[64];
    build_multipart_parts(filename, body_header, sizeof(body_header), body_footer, sizeof(body_footer));

    esp_err_t ret = http_do_upload(client, body_header, wav_buf, wav_size, body_footer, filename);
    esp_http_client_cleanup(client);
    return ret;
}

// --- 从文件上传（保留用于回退路径）---

esp_err_t http_upload_file(const char *filepath)
{
    ESP_LOGI(TAG, "Uploading file: %s", filepath);

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", filepath);
        return ESP_FAIL;
    }

    struct stat file_stat;
    stat(filepath, &file_stat);
    size_t file_size = file_stat.st_size;

    // 将整个文件读入 PSRAM 内存
    uint8_t *file_buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!file_buf) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for file upload", file_size);
        fclose(f);
        return ESP_FAIL;
    }
    size_t bytes_read = fread(file_buf, 1, file_size, f);
    fclose(f);
    if (bytes_read != file_size) {
        ESP_LOGE(TAG, "File read incomplete: %zu/%zu", bytes_read, file_size);
        free(file_buf);
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = UPLOAD_SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", "----ESP32Boundary1234");
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Authorization", UPLOAD_API_KEY);

    const char *filename = strrchr(filepath, '/');
    filename = filename ? filename + 1 : "unknown.wav";

    char body_header[512], body_footer[64];
    build_multipart_parts(filename, body_header, sizeof(body_header), body_footer, sizeof(body_footer));

    esp_err_t ret = http_do_upload(client, body_header, file_buf, file_size, body_footer, filepath);
    free(file_buf);
    esp_http_client_cleanup(client);
    return ret;
}

// --- 合并所有待传文件为单次上传 ---

void http_upload_merged_pending(void)
{
    size_t count = 0;
    char **file_list = littlefs_get_pending_uploads(&count);
    if (!file_list || count == 0) {
        if (file_list) free(file_list);
        return;
    }

    ESP_LOGI(TAG, "Found %u pending files, merging for single upload...", count);

    // 第一遍：计算合并后 PCM 总大小，验证每个文件
    size_t total_pcm_bytes = 0;
    size_t valid_count = 0;
    for (size_t i = 0; i < count; i++) {
        struct stat st;
        if (stat(file_list[i], &st) == 0 && st.st_size > 44) {
            total_pcm_bytes += st.st_size - 44;
            valid_count++;
        }
    }

    if (valid_count == 0) {
        for (size_t i = 0; i < count; i++) free(file_list[i]);
        free(file_list);
        return;
    }

    // 分配合并缓冲区（PSRAM）：44 字节 WAV 头 + 全部 PCM 数据
    size_t merged_size = 44 + total_pcm_bytes;
    uint8_t *merged_buf = heap_caps_malloc(merged_size, MALLOC_CAP_SPIRAM);
    if (!merged_buf) {
        ESP_LOGE(TAG, "Failed to allocate merged buffer (%zu bytes)", merged_size);
        for (size_t i = 0; i < count; i++) free(file_list[i]);
        free(file_list);
        return;
    }

    // 写入 WAV 头（使用总 PCM 大小）
    memset(merged_buf, 0, 44);
    memcpy(merged_buf + 0, "RIFF", 4);
    uint32_t file_size = total_pcm_bytes + 36;
    memcpy(merged_buf + 4, &file_size, 4);
    memcpy(merged_buf + 8, "WAVE", 4);
    memcpy(merged_buf + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(merged_buf + 16, &fmt_size, 4);
    uint16_t audio_format = 1;
    memcpy(merged_buf + 20, &audio_format, 2);
    uint16_t num_channels = AUDIO_CHANNELS;
    memcpy(merged_buf + 22, &num_channels, 2);
    uint32_t sample_rate = AUDIO_SAMPLE_RATE;
    memcpy(merged_buf + 24, &sample_rate, 4);
    uint32_t byte_rate = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BIT_DEPTH / 8);
    memcpy(merged_buf + 28, &byte_rate, 4);
    uint16_t block_align = AUDIO_CHANNELS * (AUDIO_BIT_DEPTH / 8);
    memcpy(merged_buf + 32, &block_align, 2);
    uint16_t bits_per_sample = AUDIO_BIT_DEPTH;
    memcpy(merged_buf + 34, &bits_per_sample, 2);
    memcpy(merged_buf + 36, "data", 4);
    memcpy(merged_buf + 40, &total_pcm_bytes, 4);

    // 第二遍：读取每个文件的 PCM 数据并拼接
    size_t offset = 44;
    bool read_ok = true;
    for (size_t i = 0; i < count; i++) {
        struct stat st;
        if (stat(file_list[i], &st) != 0 || st.st_size <= 44) continue;

        FILE *f = fopen(file_list[i], "rb");
        if (!f) { read_ok = false; break; }

        fseek(f, 44, SEEK_SET); // 跳过 WAV 头
        size_t pcm_bytes = st.st_size - 44;
        size_t nread = fread(merged_buf + offset, 1, pcm_bytes, f);
        fclose(f);

        if (nread != pcm_bytes) { read_ok = false; break; }
        offset += pcm_bytes;
    }

    if (!read_ok) {
        ESP_LOGE(TAG, "Failed to read one or more files for merge");
        free(merged_buf);
        for (size_t i = 0; i < count; i++) free(file_list[i]);
        free(file_list);
        return;
    }

    // 上传合并后的 WAV
    char upload_name[32];
    snprintf(upload_name, sizeof(upload_name), "merged_%u_files.wav", (unsigned)valid_count);
    esp_err_t ret = http_upload_buffer(merged_buf, merged_size, upload_name);
    free(merged_buf);

    if (ret == ESP_OK) {
        // 上传成功：删除所有源文件
        for (size_t i = 0; i < count; i++) {
            littlefs_delete_file(file_list[i]);
        }
    } else {
        ESP_LOGW(TAG, "Merged upload failed, will retry later");
    }

    for (size_t i = 0; i < count; i++) free(file_list[i]);
    free(file_list);

    // 上传完成后清理超限文件
    littlefs_cleanup_old_files();
}

// --- 保留旧接口：逐个上传（回退路径）---

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
            for (size_t j = i; j < count; j++) {
                free(file_list[j]);
            }
            break;
        }
        free(file_list[i]);
    }

    free(file_list);

    // 上传完成后清理超限文件（此时无文件打开，不会冲突）
    littlefs_cleanup_old_files();
}
