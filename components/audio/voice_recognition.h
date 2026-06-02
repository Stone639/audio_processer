#ifndef _VOICE_RECOGNITION_H_
#define _VOICE_RECOGNITION_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    VR_SUCCESS = 0,
    VR_ERROR_INIT,
    VR_ERROR_FILE
} vr_error_t;

// 内存直传队列项：upload_task 收到后直接上传，完成后 free(wav_buf)
typedef struct {
    uint8_t *wav_buf;   // PSRAM 中的完整 WAV（含 44 字节头），upload_task 负责 free
    size_t   wav_size;  // WAV 总字节数
} audio_upload_item_t;

vr_error_t vr_init(void);
void vr_deinit(void);
void vr_recording_task(void *pvParameters);
void vr_save_task(void *pvParameters);

// 将预构建的完整 WAV buffer 保存到 LittleFS（上传失败回退路径）
// wav_buf 已包含 44 字节 WAV 头 + PCM 数据，调用者保留所有权
esp_err_t save_wav_buffer_to_littlefs(const uint8_t *wav_buf, size_t wav_size,
                                       const char *log_name);

#endif
