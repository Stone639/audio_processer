#include "voice_recognition.h"
#include "driver/i2s.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "esp_base64.h"
#include <string.h>
#include <stdlib.h>

#define AUDIO_BUFFER_SIZE 4096

static uint8_t *audio_buffer = NULL;
static size_t audio_buffer_size = 0;
static bool is_recording = false;

// Base64编码函数
static char *base64_encode(const uint8_t *data, size_t data_len)
{
    size_t encoded_size = esp_base64_encode_len(data_len);
    char *encoded = (char *)malloc(encoded_size + 1);
    if (!encoded) {
        return NULL;
    }
    
    esp_base64_encode(encoded, data, data_len);
    encoded[encoded_size] = '\0';
    return encoded;
}

// 调用硅基流动API进行语音识别
vr_error_t vr_process_audio(uint8_t *audio_data, size_t data_len, char **result)
{
    // 硅基流动API的URL
    const char *url = "https://api.siliconflow.cn/v1/speech-to-text";
    
    // API密钥，实际项目中应该从配置文件或安全存储中获取
    const char *api_key = "YOUR_API_KEY";
    
    // 创建HTTP客户端配置
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
    };
    
    // 创建HTTP客户端
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return VR_ERROR_API;
    }
    
    // 设置请求头
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", api_key);
    
    // 构建请求体
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "whisper-large-v3");
    cJSON_AddStringToObject(root, "language", "zh");
    
    // 将音频数据转换为Base64编码
    char *base64_audio = base64_encode(audio_data, data_len);
    if (!base64_audio) {
        cJSON_Delete(root);
        esp_http_client_cleanup(client);
        return VR_ERROR_API;
    }
    cJSON_AddStringToObject(root, "audio", base64_audio);
    
    char *request_body = cJSON_Print(root);
    cJSON_Delete(root);
    free(base64_audio);
    
    if (!request_body) {
        esp_http_client_cleanup(client);
        return VR_ERROR_API;
    }
    
    // 设置请求体
    esp_http_client_set_post_field(client, request_body, strlen(request_body));
    
    // 发送请求
    esp_err_t ret = esp_http_client_perform(client);
    if (ret != ESP_OK) {
        free(request_body);
        esp_http_client_cleanup(client);
        return VR_ERROR_API;
    }
    
    // 获取响应状态码
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        free(request_body);
        esp_http_client_cleanup(client);
        return VR_ERROR_API;
    }
    
    // 读取响应体
    size_t content_length = esp_http_client_get_content_length(client);
    char *response_body = (char *)malloc(content_length + 1);
    if (!response_body) {
        free(request_body);
        esp_http_client_cleanup(client);
        return VR_ERROR_API;
    }
    
    ret = esp_http_client_read(client, response_body, content_length);
    if (ret != content_length) {
        free(request_body);
        free(response_body);
        esp_http_client_cleanup(client);
        return VR_ERROR_API;
    }
    response_body[content_length] = '\0';
    
    // 解析响应体
    root = cJSON_Parse(response_body);
    if (!root) {
        free(request_body);
        free(response_body);
        esp_http_client_cleanup(client);
        return VR_ERROR_API;
    }
    
    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (!text || !cJSON_IsString(text)) {
        cJSON_Delete(root);
        free(request_body);
        free(response_body);
        esp_http_client_cleanup(client);
        return VR_ERROR_API;
    }
    
    // 提取识别结果
    *result = strdup(text->valuestring);
    
    // 清理资源
    cJSON_Delete(root);
    free(request_body);
    free(response_body);
    esp_http_client_cleanup(client);
    
    return VR_SUCCESS;
}

vr_error_t vr_init(void)
{
    // I2S配置
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    // I2S引脚配置
    i2s_pin_config_t pin_config = {
        .bck_io_num = 26,
        .ws_io_num = 25,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = 33
    };

    // 初始化I2S驱动
    esp_err_t ret = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        return VR_ERROR_INIT;
    }

    ret = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (ret != ESP_OK) {
        i2s_driver_uninstall(I2S_NUM_0);
        return VR_ERROR_INIT;
    }

    // 分配音频缓冲区
    audio_buffer = (uint8_t *)malloc(AUDIO_BUFFER_SIZE);
    if (!audio_buffer) {
        i2s_driver_uninstall(I2S_NUM_0);
        return VR_ERROR_INIT;
    }

    audio_buffer_size = 0;
    return VR_SUCCESS;
}

vr_error_t vr_start_recording(void)
{
    if (!audio_buffer) {
        return VR_ERROR_INIT;
    }

    is_recording = true;
    audio_buffer_size = 0;
    return VR_SUCCESS;
}

vr_error_t vr_stop_and_recognize(char **result)
{
    if (!audio_buffer) {
        return VR_ERROR_INIT;
    }

    is_recording = false;
    
    // 调用硅基流动API进行语音识别
    return vr_process_audio(audio_buffer, audio_buffer_size, result);
}

// 录音任务
void vr_recording_task(void *pvParameters)
{
    size_t bytes_read = 0;

    while (1) {
        if (is_recording && audio_buffer_size < AUDIO_BUFFER_SIZE) {
            // 读取麦克风数据
            esp_err_t ret = i2s_read(
                I2S_NUM_0,
                audio_buffer + audio_buffer_size,
                AUDIO_BUFFER_SIZE - audio_buffer_size,
                &bytes_read,
                pdMS_TO_TICKS(10)
            );

            if (ret == ESP_OK) {
                audio_buffer_size += bytes_read;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void vr_deinit(void)
{
    // 释放音频缓冲区
    if (audio_buffer) {
        free(audio_buffer);
        audio_buffer = NULL;
    }

    // 释放I2S资源
    i2s_driver_uninstall(I2S_NUM_0);
}
