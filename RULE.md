# audio_processer 项目规范

## 项目概述

ESP32-S3 音频录制与语音转文字系统。通过 I2S 麦克风采集音频，存为 WAV 到 LittleFS，联网时自动上传到 SiliconFlow API 进行语音识别。

- 芯片：ESP32-S3
- IDF 版本：5.5.4
- 麦克风：INMP441（I2S，24bit/32bit 帧，16kHz 采样率）
- 本地存储：LittleFS（分区名 `storage`，约 14MB）
- 语音识别：SiliconFlow API（FunAudioLLM/SenseVoiceSmall 模型）

## 目录结构

```
audio_processer/
├── CMakeLists.txt              # 顶层构建（设置 COMPONENTS 白名单）
├── partitions.csv              # 分区表
├── sdkconfig                   # IDF 配置（自动生成，不提交）
├── dependencies.lock           # 组件锁文件
├── RULE.md                     # 本文件
│
├── main/                       # 应用入口
│   ├── CMakeLists.txt
│   ├── main.c                  # app_main、主任务循环
│   └── idf_component.yml       # main 组件依赖声明
│
├── components/                 # 自定义组件（每个子目录一个组件）
│   ├── config/                 # 全局配置管理
│   │   ├── CMakeLists.txt
│   │   ├── app_config.h        # WiFi、API、录音参数等常量定义
│   │   └── app_config.c
│   │
│   ├── wifi/                   # WiFi 连接管理
│   │   ├── CMakeLists.txt
│   │   ├── wifi_manager.h      # WiFi 初始化、连接、状态查询接口
│   │   └── wifi_manager.c
│   │
│   ├── audio/                  # 音频采集与编码
│   │   ├── CMakeLists.txt
│   │   ├── voice_recognition.h # I2S 麦克风驱动接口
│   │   ├── voice_recognition.c
│   │   ├── wav_encoder.h       # WAV 编码器接口
│   │   └── wav_encoder.c
│   │
│   ├── storage/                # 本地文件系统管理
│   │   ├── CMakeLists.txt
│   │   ├── littlefs_manager.h  # LittleFS 操作接口
│   │   └── littlefs_manager.c
│   │
│   └── http/                   # 网络上传
│       ├── CMakeLists.txt
│       ├── http_uploader.h     # HTTP 上传接口
│       └── http_uploader.c
│
├── .devcontainer/              # Dev Container 配置
├── .vscode/                    # VS Code 配置
├── .clangd                     # clangd 配置
└── .gitignore
```

## 各组件职责

### config — 全局配置

集中管理所有可调参数，其他组件通过 `#include "app_config.h"` 获取，不硬编码。

需要定义的内容：
- `WIFI_SSID` / `WIFI_PASS` — WiFi 凭据
- `UPLOAD_SERVER_URL` — SiliconFlow API 地址
- `UPLOAD_API_KEY` — API Bearer token
- `UPLOAD_MODEL` — 语音识别模型名
- `RECORD_DURATION_MS` — 每段录音时长（当前 3000ms）
- `RECORD_INTERVAL_MS` — 录音间隔（当前 1000ms）
- `AUDIO_SAMPLE_RATE` / `AUDIO_BIT_DEPTH` / `AUDIO_CHANNELS` — 音频参数
- `MAX_RECORDING_FILES` — 最大缓存文件数（当前 20）
- I2S 引脚定义（BCK/WS/DIN）

### wifi — WiFi 管理

从 `main.c` 拆出，封装 WiFi STA 模式的完整生命周期。

接口设计：
- `wifi_manager_init()` — 初始化 NVS、netif、event loop、WiFi 驱动
- `wifi_manager_is_connected()` — 查询连接状态
- 内部处理断连重试、IP 获取事件

### audio — 音频采集

I2S 麦克风驱动 + WAV 编码。INMP441 输出 24bit/32bit 帧，I2S 以 32 位读取后取高 16 位存入 buffer。

### storage — 本地存储

LittleFS 文件管理。init 时扫描已有文件恢复序号，避免断电后覆盖。

### http — 网络上传

HTTPS POST 上传 WAV 到 SiliconFlow API，multipart/form-data 格式，带 Authorization header。使用 ESP-IDF 证书包验证 HTTPS。上传成功后删除本地文件。

## 组件依赖

```
main ──→ config
  ├──→ wifi ──→ config
  ├──→ audio ──→ storage
  ├──→ storage
  └──→ http ──→ storage, config, mbedtls
```

- `config` 无依赖，被所有组件引用
- `wifi` 依赖 `config`（读取 SSID/密码）
- `audio` 依赖 `storage`（录音保存到 LittleFS）
- `http` 依赖 `storage`（读取待上传文件）+ `config`（读取 API 配置）+ `mbedtls`（HTTPS 证书包）
- `main` 依赖所有组件，只做编排，不含业务逻辑

## 组件规则

### 组件命名

- 组件目录名 = 组件名，全小写，用下划线分隔
- 头文件与组件同名或用描述性名称
- 每个组件一个 `CMakeLists.txt`，用 `idf_component_register()` 注册

### 函数命名

统一格式：`模块前缀_动作`，如：
- `wifi_manager_init()`
- `vr_start_recording()`
- `littlefs_get_next_filename()`
- `http_upload_file()`

### 文件命名

- 源文件：`snake_case.c` / `snake_case.h`
- 常量宏：`UPPER_SNAKE_CASE`
- 类型名：`snake_case_t`（struct/enum）

### 新增组件步骤

1. 在 `components/` 下创建目录
2. 写 `CMakeLists.txt`，用 `idf_component_register()` 注册源文件和依赖
3. 在 `main/CMakeLists.txt` 的 `REQUIRES` 中添加新组件名
4. 顶层 `CMakeLists.txt` 的 `COMPONENTS` 列表中添加（如果需要限制构建范围）

## 分区表

| 名称 | 类型 | 子类型 | 偏移 | 大小 | 用途 |
|------|------|--------|------|------|------|
| nvs | data | nvs | 0x9000 | 24KB | NVS 键值存储 |
| phy_init | data | phy | 0xf000 | 4KB | PHY 初始化 |
| factory | app | factory | 0x10000 | 2MB | 固件 |
| storage | data | littlefs | 0x210000 | ~14MB | 音频文件存储 |

## Git 规范

- 分支命名：`功能描述`（英文，kebab-case），如 `NoSD`、`add-wifi-config`
- 提交信息：中文，简述改动内容
- 不提交：`build/`、`sdkconfig`、`sdkconfig.old`、`managed_components/`

## 开发记录

### 阶段一：基础架构（已完成）

1. **SD卡 → LittleFS 迁移**：移除 FatFS/SD 卡依赖，改用 LittleFS 存储音频文件
2. **组件化重构**：将 main.c 中的内联代码拆分为 audio、storage、http 三个组件
3. **config 组件**：创建集中配置管理，所有硬编码参数（WiFi、API 地址、录音参数、I2S 引脚）统一到 `app_config.h`
4. **wifi 组件**：从 main.c 拆出 WiFi STA 管理，封装 init/is_connected 接口
5. **文件序号恢复**：`littlefs_init()` 时扫描已有文件恢复 `s_file_counter`，解决断电后序号重置问题
6. **头文件对齐**：`voice_recognition.h` 声明与实现同步

### 阶段二：音频驱动修复（已完成）

1. **INMP441 I2S 配置**：从 16 位模式改为 32 位模式（INMP441 输出 24bit/32bit 帧），读取后右移 16 位取有效数据
2. **边界保护**：录音任务中计算最大可读样本数，防止 buffer 写越界
3. **日志优化**：I2S 连续超时时抑制日志刷屏（前 3 次打警告，之后静默）

### 阶段三：SiliconFlow API 对接（已完成）

1. **API 切换**：从本地 HTTP 服务器改为 SiliconFlow 语音转文字 API（`https://api.siliconflow.cn/v1/audio/transcriptions`）
2. **HTTPS 支持**：使用 ESP-IDF 证书包（`esp_crt_bundle_attach`）验证 HTTPS 连接
3. **请求格式**：multipart/form-data，包含 `model` 和 `file` 两个字段
4. **认证**：Authorization header 带 Bearer token
5. **响应读取**：打印 API 返回的转文字结果
6. **WiFi 等待**：main_task 启动后等待最多 10 秒让 WiFi 连上再尝试上传

## 待办

- [ ] 创建 API 接口组件：将 SiliconFlow 返回的转文字结果通过 HTTP server 暴露为 REST API，供其他客户端调用
