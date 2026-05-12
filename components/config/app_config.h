#ifndef _APP_CONFIG_H_
#define _APP_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// WiFi
#define WIFI_SSID       "Redmi Turbo 3"
#define WIFI_PASS       "12345678"

// SiliconFlow 语音转文字 API
#define UPLOAD_SERVER_URL   "https://api.siliconflow.cn/v1/audio/transcriptions"
#define UPLOAD_API_KEY      "Bearer sk-wcvpnwacmmtgzgfqhzkctnjluueyegboqnfftsrmnzftofem"
#define UPLOAD_MODEL        "FunAudioLLM/SenseVoiceSmall"

// 录音参数
#define RECORD_DURATION_MS  3000
#define RECORD_INTERVAL_MS  1000

// 音频参数
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BIT_DEPTH     16
#define AUDIO_CHANNELS      1

// 上传
#define UPLOAD_INTERVAL_MS  5000

// 存储
#define MAX_RECORDING_FILES 20

// 环形缓冲区（秒）
#define RING_BUFFER_SECONDS 3

// 音频积累上传（秒）：积累这么多秒的音频后一次性上传
#define UPLOAD_ACCUMULATE_SECONDS 5

// I2S 引脚
#define I2S_PIN_BCK     47
#define I2S_PIN_WS      10
#define I2S_PIN_DIN     21

#ifdef __cplusplus
}
#endif

#endif
