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

// 内存直传队列项：upload_task 收到后直接上传，完成后 free(wav_buf)
typedef struct {
    uint8_t *wav_buf;   // PSRAM 中的完整 WAV（含 44 字节头），upload_task 负责 free
    size_t   wav_size;  // WAV 总字节数
} audio_upload_item_t;

vr_error_t vr_init(void);
void vr_deinit(void);
void vr_recording_task(void *pvParameters);
void vr_save_task(void *pvParameters);

#endif
