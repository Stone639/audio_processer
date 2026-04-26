# Audio 组件 — API 参考

本文档描述 `components/audio` 中提供的主要接口、使用示例与注意事项，便于在 ESP-IDF 项目中集成录音、WAV 保存与简单的语音识别工作流。

**主要模块**
- `voice_recognition`：I2S 录音缓冲、录音任务、保存 WAV、（可选）调用云端识别的封装接口。
- `wav_encoder`：将 PCM 数据写入 WAV 文件、回填头部信息的工具函数。

## 依赖
- ESP-IDF: I2S（`driver/i2s_std.h`）、HTTP 服务器/客户端（`esp_http_server.h` / `esp_http_client.h`）、日志（`esp_log.h`）
- FatFS（`ff.h`）用于文件写入（若需保存 WAV）

## 概览与设计要点
- 采样与格式：默认使用 16 kHz、16-bit、单声道 PCM（可在实现中修改）
- 录音流：`voice_recognition` 提供 `vr_recording_task` 作为参考任务，后台从 I2S 读取数据并累积到内存缓冲区；用户通过 `vr_start_recording()` / `vr_stop_and_save()` 控制录音周期。
- WAV 保存：`wav_encoder` 提供 `wav_encoder_write_header`、`wav_encoder_encode_data`、`wav_encoder_fix_header`，用于在 FatFS 上写入标准 WAV 文件。

---

## `voice_recognition` API（摘要）

- `vr_error_t vr_init(void)`
    - 初始化 I2S 通道、分配内部缓冲区并准备录音任务资源。
    - 返回 `VR_SUCCESS` 或 `VR_ERROR_INIT`。

- `vr_error_t vr_start_recording(void)`
    - 清空内部缓冲区并设置采集标志；实际采集由 `vr_recording_task` 完成。
    - 返回 `VR_SUCCESS` 或 `VR_ERROR_INIT`。
# Audio 组件 API 文档

本文档概述 `components/audio` 提供的主要接口、使用方法与注意事项。该组件包含：
- `voice_recognition`：基于 I2S 的录音、内存缓冲、WAV 保存与（可选）语音识别流程。
- `wav_encoder`：将 PCM 写入 WAV 文件并回填头部。

本文面向在 ESP-IDF 上集成音频采集与保存/识别功能的开发者。

## 概要

- 采样配置：16 kHz、16-bit、单声道（默认，可按需要修改）。
- I2S 驱动基于 `driver/i2s_std.h` API（I2S v2）。
- 音频缓冲可在内存中保存并写入 FAT 文件系统（通过 FatFS）。

## 主要模块与头文件

- `voice_recognition.h`：初始化、开始/停止录音、保存 WAV、录音任务原型。
- `voice_recognition.c`：录音实现（参考 `vr_init`、`vr_start_recording`、`vr_stop_and_save`、`vr_recording_task`）。
- `wav_encoder.h/.c`：WAV 文件头写入、PCM 数据写入、回填头部大小。

## API 速览

所有符号定义见 `components/audio/*` 源文件，以下为常用函数说明：

- `vr_error_t vr_init(void)`
    - 初始化 I2S 通道、分配内部音频缓冲区并启用通道。
    - 返回 `VR_SUCCESS` 或 `VR_ERROR_INIT`。

- `vr_error_t vr_start_recording(void)`
    - 开始录音（只是置位状态，实际采集由 `vr_recording_task` 将数据写入缓冲区）。
    - 返回 `VR_SUCCESS` 或 `VR_ERROR_INIT`（未初始化）。

- `vr_error_t vr_stop_and_save(const char *filename)`
    - 停止录音并将缓冲区内容保存为 WAV 文件（使用 FatFS，路径例如 "/sdcard/rec.wav"）。
    - 返回 `VR_SUCCESS` 或 `VR_ERROR_FILE`。

- `void vr_deinit(void)`
    - 释放缓冲区、停用并删除 I2S 通道。

- `void vr_recording_task(void *pvParameters)`
    - 推荐以 FreeRTOS 任务方式运行：循环读取 I2S 数据并追加到内部缓冲区，直到停止或缓冲区满。

- `wav_encoder_write_header(FIL *file)` / `wav_encoder_encode_data(...)` / `wav_encoder_fix_header(...)`
    - 辅助函数：写入 WAV 头、写 PCM 数据、回填文件长度字段。

## 使用示例（要点）

1. 在 `app_main()` 中调用 `vr_init()`：

```c
if (vr_init() != VR_SUCCESS) {
        ESP_LOGE("AUDIO", "vr_init failed");
        return;
}
```

2. 创建录音任务：

```c
xTaskCreate(vr_recording_task, "vr_rec", 4096, NULL, 5, NULL);
```

3. 开始录音、延时、停止并保存：

```c
vr_start_recording();
vTaskDelay(pdMS_TO_TICKS(3000)); // 录 3 秒
vr_stop_and_save("/sdcard/rec.wav");
```

4. 释放资源：

```c
vr_deinit();
```

## 设计与注意事项

- 缓冲区大小：`AUDIO_BUFFER_SIZE` 在 `voice_recognition.c` 中定义（示例为 16kHz×2 字节×3 秒）。根据需求调整以容纳所需录音时长。
- 线程安全：接口并非并发安全；在多任务环境中对状态变更（开始/停止/保存）请加互斥或事件同步。
- I2S 引脚与 DMA：示例代码在 `voice_recognition.c` 中设置了具体 GPIO（BCK、WS、DIN），请根据硬件接线修改。
- WAV 写入：`wav_encoder` 使用 FatFS 同步写文件，确保在使用前已初始化并挂载文件系统（例如 SD 卡或 SPIFFS）。
- HTTP 服务：若需通过 HTTP 提供下载（如 `/rec.wav`），将 HTTP 服务器初始化并在 handler 中读取保存的文件或直接从内存返回数据。HTTP 相关代码应放在函数/任务内部，而非全局作用域。

## 常见错误与排查

- 编译错误 `expected identifier or '(' before '{' token`：通常由顶层存在裸 `{ ... }` 代码块造成，确保所有逻辑在函数内。
- 未定义 `download_handler`：若启用 HTTP 下载功能，请实现回调函数并确保签名为 `esp_err_t download_handler(httpd_req_t *req)`。
- I2S 读不到数据：检查引脚、供电、麦克风工作电压、以及 `i2s_channel_init_std_mode` 的 `slot_cfg` 与麦克风输出格式是否一致。

## 目录与参考文件

- `components/audio/voice_recognition.h` — API 声明
- `components/audio/voice_recognition.c` — 实现（录音、缓冲、保存）
- `components/audio/wav_encoder.h` / `wav_encoder.c` — WAV 工具

## 版本历史

- 1.0 2026-04-25：文档重写，整理 API、示例与注意事项。