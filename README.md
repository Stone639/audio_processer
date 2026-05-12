# ESP32-S3 音频转文字系统

通过 I2S 麦克风持续采集音频，自动上传到云端 API 进行语音识别，返回转录文字。

## 硬件配置

### 芯片与外设

| 项目 | 规格 |
|------|------|
| 芯片 | ESP32-S3 |
| 麦克风 | INMP441（I2S 数字麦克风） |
| PSRAM | 用于环形缓冲区和上传缓冲区 |
| Flash | 16MB（含 ~14MB LittleFS 存储分区） |

### I2S 引脚接线

配置位于 `components/config/app_config.h`：

```c
#define I2S_PIN_BCK     47   // I2S 位时钟（Bit Clock）
#define I2S_PIN_WS      10   // I2S 字选择（Word Select / LRCL）
#define I2S_PIN_DIN     21   // I2S 数据输入（Data In）
```

INMP441 接线：

```
INMP441        ESP32-S3
──────         ────────
VDD     →     3.3V
GND     →     GND
SD      →     GPIO 21 (DIN)
WS      →     GPIO 10 (WS)
SCK     →     GPIO 47 (BCK)
L/R     →     GND（左声道）或 VDD（右声道，自动检测）
```

> L/R 引脚接法不影响功能，系统启动后会自动检测有效声道并选择能量更高的那个。

### 音频参数

```c
#define AUDIO_SAMPLE_RATE   16000   // 16kHz
#define AUDIO_BIT_DEPTH     16      // 16bit
#define AUDIO_CHANNELS      1       // 单声道（自动从立体声提取）
```

## WiFi 配置

配置位于 `components/config/app_config.h`：

```c
#define WIFI_SSID       "你的WiFi名"
#define WIFI_PASS       "你的WiFi密码"
```

系统启动后自动连接 WiFi，上传任务会等待连接成功后再开始工作。

## 语音模型 API 配置

使用 [SiliconFlow](https://siliconflow.cn) 的语音转文字 API：

```c
#define UPLOAD_SERVER_URL   "https://api.siliconflow.cn/v1/audio/transcriptions"
#define UPLOAD_API_KEY      "Bearer 你的API密钥"
#define UPLOAD_MODEL        "FunAudioLLM/SenseVoiceSmall"
```

| 参数 | 说明 |
|------|------|
| `UPLOAD_SERVER_URL` | API 端点，支持标准 Whisper 兼容格式 |
| `UPLOAD_API_KEY` | Bearer Token，从 SiliconFlow 控制台获取 |
| `UPLOAD_MODEL` | 语音识别模型，SenseVoiceSmall 支持中英文 |

### 上传时序参数

```c
#define UPLOAD_INTERVAL_MS          5000    // 上传检查间隔（毫秒）
#define UPLOAD_ACCUMULATE_SECONDS   5       // 积累多少秒音频后上传一次
```

## 转文字结果接口

`transcription` 模块提供两种方式获取转录文字：**回调通知**和**主动查询**。

### 方式一：回调通知（推荐）

新文字到达时自动调用注册的回调函数，无需轮询：

```c
#include "transcription.h"

// 回调函数：新文字到达时被调用
void on_transcription(const char *text, int64_t timestamp,
                      const char *filename, void *user_data)
{
    printf("[%lld] %s: %s\n", timestamp, filename, text);
    // 如需通知其他任务，可在这里 xQueueSend 或 xTaskNotify
}

// 注册回调（在 transcription_init() 之后调用）
transcription_on_result(on_transcription, NULL);
```

回调参数：

| 参数 | 类型 | 说明 |
|------|------|------|
| `text` | `const char*` | 转录的文字内容 |
| `timestamp` | `int64_t` | 时间戳（毫秒） |
| `filename` | `const char*` | 来源文件名，如 `"live_0004.wav"` |
| `user_data` | `void*` | 注册时传入的用户数据指针 |

> 回调在上传任务上下文中执行，不要做耗时操作。传 `NULL` 可取消注册。

### 方式二：主动查询

```c
#include "transcription.h"

// 获取最新一条转录文字
char buf[256];
int len = transcription_get_latest(buf, sizeof(buf));
if (len > 0) {
    printf("最新文字: %s\n", buf);
}

// 获取当前缓存条数
int count = transcription_get_count();

// 获取所有结果（JSON 数组格式）
char json[2048];
int json_len = transcription_get_all(json, sizeof(json));
// 返回: [{"text":"你好","timestamp":123456,"filename":"live_0004.wav"}, ...]
```

### 接口总览

| 函数 | 说明 |
|------|------|
| `transcription_init()` | 初始化模块，必须先调用 |
| `transcription_on_result(cb, user_data)` | 注册回调，新文字到达时通知 |
| `transcription_get_latest(buf, size)` | 获取最新一条文字 |
| `transcription_get_all(buf, size)` | 获取全部结果（JSON） |
| `transcription_get_count()` | 获取缓存条数 |
| `transcription_deinit()` | 释放资源 |

## 构建与烧录

```bash
# 首次构建（需先删除旧 sdkconfig 让 sdkconfig.defaults 生效）
rm -f sdkconfig
idf.py build

# 烧录并监控
idf.py flash monitor
```

## 系统架构

```
┌─────────────────┐    ┌──────────────┐    ┌──────────────────┐
│  I2S 麦克风     │───→│  环形缓冲区   │───→│  vr_save_task    │
│  (优先级 6)     │    │  (PSRAM 3s)  │    │  增量提取+积累   │
└─────────────────┘    └──────────────┘    └────────┬─────────┘
                                                    │
                                          FreeRTOS 队列（depth=8）
                                                    │
                                                    ▼
                    ┌──────────────┐    ┌──────────────────────┐
                    │  LittleFS    │←───│    upload_task        │
                    │  (回退存储)  │    │  队列直传 + 文件合并  │
                    └──────────────┘    └──────────┬───────────┘
                                                   │
                                                   ▼
                                        ┌─────────────────────┐
                                        │  SiliconFlow API    │
                                        │  语音转文字         │
                                        └──────────┬──────────┘
                                                   │
                                                   ▼
                                        ┌─────────────────────┐
                                        │  transcription 模块  │
                                        │  回调通知 + 缓存查询 │
                                        └─────────────────────┘
```
