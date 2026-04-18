#ifndef _VOICE_RECOGNITION_H_
#define _VOICE_RECOGNITION_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 错误码枚举
typedef enum {
    VR_SUCCESS = 0,      // 成功
    VR_ERROR_INIT,       // 初始化错误
    VR_ERROR_API         // API调用错误
} vr_error_t;

// 初始化语音识别模块
vr_error_t vr_init(void);

// 开始录音
vr_error_t vr_start_recording(void);

// 停止录音并进行识别
vr_error_t vr_stop_and_recognize(char **result);

// 反初始化，释放资源
void vr_deinit(void);

// 处理音频数据（内部使用）
vr_error_t vr_process_audio(uint8_t *audio_data, size_t data_len, char **result);

#endif // _VOICE_RECOGNITION_H_