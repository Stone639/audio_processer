#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 转文字结果回调函数类型
 *
 * 新的转录文字到达时被调用。
 *
 * @param text      转录的文字内容
 * @param timestamp 时间戳（毫秒）
 * @param filename  来源文件名（如 "live_0004.wav" 或 "rec_945.wav"）
 * @param user_data 注册时传入的用户数据指针
 */
typedef void (*transcription_callback_t)(const char *text, int64_t timestamp,
                                         const char *filename, void *user_data);

/**
 * @brief 注册转文字结果回调
 *
 * 新的转录文字到达时，回调会被调用（在上传任务上下文中执行）。
 * 回调中不要做耗时操作，如需处理可发消息到其他任务。
 *
 * @param cb        回调函数，传 NULL 取消注册
 * @param user_data 透传给回调的用户数据指针
 */
void transcription_on_result(transcription_callback_t cb, void *user_data);

/**
 * @brief 初始化转文字结果模块
 *
 * 分配内部资源（环形缓冲区、互斥锁），必须在使用其他函数前调用。
 *
 * @return ESP_OK 成功，ESP_FAIL 失败
 */
esp_err_t transcription_init(void);

/**
 * @brief 获取最新的转文字结果
 *
 * @param buf   输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 实际写入的文本长度，0 表示暂无结果
 */
int transcription_get_latest(char *buf, size_t buf_size);

/**
 * @brief 获取所有缓存的转文字结果（JSON 数组格式）
 *
 * 返回格式示例：
 * [{"text":"你好","timestamp":123456,"filename":"rec_001.wav"},...]
 *
 * @param buf   输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 实际写入的 JSON 长度，0 表示暂无结果
 */
int transcription_get_all(char *buf, size_t buf_size);

/**
 * @brief 获取当前缓存的转文字结果条数
 *
 * @return 缓存条数
 */
int transcription_get_count(void);

/**
 * @brief 反初始化，释放内部资源
 *
 * @return ESP_OK
 */
esp_err_t transcription_deinit(void);

/**
 * @brief 解析 SiliconFlow API 的 JSON 响应并将转文字结果存入缓冲区
 *
 * 供 http_uploader 内部调用，外部模块一般不需要直接使用。
 * 响应格式：{"text": "转录的文字"}
 *
 * @param json_response API 返回的 JSON 字符串
 * @param filename      来源文件名（可选，用于记录）
 * @return ESP_OK 成功，ESP_FAIL 解析失败或文本为空
 */
esp_err_t transcription_parse_and_add(const char *json_response, const char *filename);

#ifdef __cplusplus
}
#endif
