#ifndef _VOICE_RECOGNITION_H_
#define _VOICE_RECOGNITION_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    VR_SUCCESS = 0,
    VR_ERROR_INIT,
    VR_ERROR_FILE
} vr_error_t;

/**
 * @brief 初始化 I2S 麦克风驱动（16kHz, 16bit, 单声道）
 * @return VR_SUCCESS 或 VR_ERROR_INIT
 */
vr_error_t vr_init(void);

/**
 * @brief 开始录音（清空内部缓冲区）
 * @return VR_SUCCESS 或 VR_ERROR_INIT
 */
vr_error_t vr_start_recording(void);

/**
 * @brief 停止录音并将缓冲区数据保存为 WAV 文件到 LittleFS
 * @return VR_SUCCESS 或 VR_ERROR_FILE
 */
vr_error_t vr_stop_and_save_to_littlefs(void);

/**
 * @brief 释放资源，卸载 I2S 驱动
 */
void vr_deinit(void);

/**
 * @brief 录音任务函数（需在 FreeRTOS 中创建）
 * @param pvParameters 未使用
 */
void vr_recording_task(void *pvParameters);

#endif