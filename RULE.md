# audio_processer 项目规范

## 项目概述

ESP32-S3 音频录制与语音转文字系统。通过 I2S 麦克风持续采集音频，积累后自动上传到 SiliconFlow API 进行语音识别，支持回调和轮询两种方式获取转录文字。

- 芯片：ESP32-S3
- IDF 版本：5.5.4
- 麦克风：INMP441（I2S，24bit/32bit 帧，16kHz 采样率）
- 本地存储：LittleFS（分区名 `storage`，约 14MB，用于回退存储）
- 语音识别：SiliconFlow API（FunAudioLLM/SenseVoiceSmall 模型）
- 主要数据路径：环形缓冲区 → 内存 WAV 构建 → FreeRTOS 队列 → 直接上传（跳过 LittleFS）

## 目录结构

```
audio_processer/
├── CMakeLists.txt              # 顶层构建（设置 COMPONENTS 白名单）
├── partitions.csv              # 分区表
├── sdkconfig                   # IDF 配置（自动生成，不提交）
├── dependencies.lock           # 组件锁文件
├── RULE.md                     # 本文件
├── README.md                   # 使用文档（硬件接线、配置、接口说明）
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
│   ├── http/                   # 网络上传
│   │   ├── CMakeLists.txt
│   │   ├── http_uploader.h     # HTTP 上传接口
│   │   └── http_uploader.c
│   │
│   └── transcription/          # 转文字结果管理
│       ├── CMakeLists.txt
│       ├── transcription.h     # 对外接口：init/get_latest/get_all/get_count/deinit
│       └── transcription.c
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
- `RECORD_INTERVAL_MS` — 保存间隔（当前 1000ms）
- `UPLOAD_INTERVAL_MS` — 上传检查间隔（当前 5000ms）
- `AUDIO_SAMPLE_RATE` / `AUDIO_BIT_DEPTH` / `AUDIO_CHANNELS` — 音频参数
- `MAX_RECORDING_FILES` — 最大缓存文件数（当前 20）
- `RING_BUFFER_SECONDS` — 环形缓冲区秒数（当前 3）
- `UPLOAD_ACCUMULATE_SECONDS` — 积累多少秒后上传（当前 5）
- I2S 引脚定义（BCK/WS/DIN）

### wifi — WiFi 管理

从 `main.c` 拆出，封装 WiFi STA 模式的完整生命周期。

接口设计：
- `wifi_manager_init()` — 初始化 NVS、netif、event loop、WiFi 驱动
- `wifi_manager_is_connected()` — 查询连接状态
- 内部处理断连重试、IP 获取事件

### audio — 音频采集

I2S 麦克风驱动 + 环形缓冲区 + WAV 编码。连续录音架构：

- `vr_recording_task`（优先级 6）：I2S 立体声读取 → 自动检测有效声道 → 单声道写入环形缓冲区（PSRAM，3 秒窗口）
- `vr_save_task`（优先级 5）：增量提取新样本 → 积累 5 秒 → 在 PSRAM 中构建 WAV → 通过队列直传 upload_task（队列满时回退到 LittleFS 文件）
- 上传由 main 中独立的 `upload_task` 负责，不阻塞录音和保存

接口设计：
- `vr_init()` — 分配环形缓冲区 + 积累缓冲区（PSRAM）、初始化 I2S
- `vr_recording_task()` — 连续录音任务
- `vr_save_task()` — 定期保存任务（接收 FreeRTOS 队列句柄作为 pvParameters）
- `vr_deinit()` — 释放资源

### storage — 本地存储

LittleFS 文件管理。init 时扫描已有文件恢复序号，避免断电后覆盖。

### http — 网络上传

三种上传方式：
- `http_upload_buffer()` — 从 PSRAM 内存 buffer 直接上传（主路径，队列直传）
- `http_upload_merged_pending()` — 合并 LittleFS 中所有待传文件为单次上传（回退路径）
- `http_upload_file()` — 上传单个文件（保留接口）

通用流程：multipart/form-data POST 到 SiliconFlow API，ESP-IDF 证书包验证 HTTPS，上传成功后删除源文件，解析转文字结果存入 transcription 模块。

### transcription — 转文字结果管理

环形缓冲区存储最近 20 条转文字结果（每条 256 字节）。两种获取方式：
- **回调通知**：`transcription_on_result(cb, user_data)` 注册回调，新文字到达时自动触发
- **主动查询**：`transcription_get_latest()` / `transcription_get_all()` / `transcription_get_count()`

内部完成 cJSON 解析、mutex 保护。

## 组件依赖

```
main ──→ config
  ├──→ wifi ──→ config
  ├──→ audio ──→ storage, config
  ├──→ storage
  ├──→ transcription ──→ json, config
  └──→ http ──→ storage, config, mbedtls, transcription
```

- `config` 无依赖，被所有组件引用
- `wifi` 依赖 `config`（读取 SSID/密码）
- `audio` 依赖 `storage`（LittleFS 回退路径）+ `config`（音频参数）。不依赖 wifi/http，上传由 main 负责。通过 FreeRTOS 队列与 upload_task 通信
- `transcription` 依赖 `json`（cJSON 解析）+ `config`
- `http` 依赖 `storage`（读取待上传文件）+ `config`（读取 API 配置）+ `mbedtls`（HTTPS 证书包）+ `transcription`（上传成功后存入转文字结果）
- `main` 依赖所有组件，编排三个独立任务：录音（优先级 6）、保存（优先级 5）、上传（优先级 4）

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

### 阶段四：转文字结果模块（已完成）

1. **transcription 组件**：新建 `components/transcription/`，封装转文字结果的环形缓冲区（20 条，每条 256 字节）
2. **完全封装**：.h 只暴露 5 个函数（init/get_latest/get_all/get_count/deinit），内部结构体、mutex、cJSON 解析全部在 .c 中
3. **http_uploader 集成**：上传成功后自动解析 SiliconFlow JSON 响应，提取 `text` 字段存入缓冲区
4. **线程安全**：FreeRTOS mutex 保护并发访问

### 阶段五：音频驱动深度修复（已完成）

1. **API 返回空文本**：根因是 I2S 单声道模式只读左声道，INMP441 L/R 引脚接高电平时数据在右声道，读到全零
2. **立体声读取**：改为 `I2S_SLOT_MODE_STEREO` + `I2S_STD_SLOT_BOTH`，运行时比较左右声道能量自动选择有效声道
3. **I2S 读取不稳定**：DMA 缓冲区从 64 帧增到 256 帧，读取超时从 10ms 增到 100ms，录音前预热 DMA（丢弃初始 3 次读取）
4. **直流偏置去除**：INMP441 输出有严重 DC offset（~16328），录音后计算均值减去，信号从 `max=32767, min=0` 变为对称

### 阶段六：连续录音架构改造（已完成）

1. **原架构问题**：分段录音 3 秒 → 停止 → 保存 → 上传（串行），上传耗时 20+ 秒期间不录音，语音丢失
2. **环形缓冲区**：320KB PSRAM 分配，10 秒 16kHz 16bit 单声道窗口，录音任务持续写入永不停止
3. **三个独立任务**：
   - `vr_recording_task`（优先级 6）：I2S → 环形缓冲区
   - `vr_save_task`（优先级 5）：每 ~14 秒提取 10 秒音频，去 DC offset，保存 WAV
   - `upload_task`（优先级 4）：每 5 秒检查待上传文件，后台 HTTP 上传
4. **保存与上传解耦**：上传不再阻塞录音和保存，三个任务完全独立
5. **sdkconfig.defaults**：新增 PSRAM 启用、16MB Flash、自定义分区表配置

### 阶段七：实时性优化（已完成）

1. **增量保存**：消除 90% 音频重叠，文件从 ~320KB 降到 ~32KB
2. **内存直传**：WAV 在 PSRAM 中构建（`wav_encoder_build_buffer`），通过 FreeRTOS 队列（depth=8）直传 upload_task，跳过 LittleFS 读写
3. **合并上传**：LittleFS 遗留文件合并为单次 HTTPS POST（`http_upload_merged_pending`），减少 TLS 握手
4. **队列缓冲**：upload_queue depth=8，40 秒缓冲覆盖上传延迟
5. **转文字回调**：新增 `transcription_on_result()` 回调接口，新文字到达自动通知
6. **HTTP 缓冲区**：写缓冲从 1KB 增到 4KB，减少系统调用次数
7. **FD 泄漏修复**：修复 `littlefs_cleanup_old_files` 与 HTTP 上传的竞态条件，cleanup 移到上传完成后执行

### IRAM 溢出问题（待解决）

**现象**：构建报告 IRAM 使用 100%（15356/16384 字节，剩余 0）。

**根因**：项目代码无 `IRAM_ATTR`，是 ESP-IDF 组件（FreeRTOS、WiFi 驱动、中断处理等）默认将 `.text` 放入 IRAM 导致。

**风险**：IRAM 满载后新增代码或 IDF 组件升级可能导致链接失败或运行时崩溃。

**解决方向**（需在 sdkconfig.defaults 中配置）：
- `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y` — 将 FreeRTOS 非关键函数移到 Flash
- `CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y` — 从 PSRAM 加载指令（需 PSRAM 支持）
- `CONFIG_SPIRAM_RODATA=y` — 将只读数据放 PSRAM
- 增大指令/数据 Cache 减少 IRAM 压力

**待办**：逐项尝试上述配置，验证编译通过且运行稳定。

## 待办

- [x] 重新构建：需先删 sdkconfig，让 sdkconfig.defaults 生效（PSRAM + 16MB Flash + 自定义分区表）
- [x] 构建验证：确认 PSRAM 分配成功、环形缓冲区正常工作
- [x] 运行验证：确认转录文字正常返回
- [ ] **IRAM 溢出修复**：IRAM 使用 100%，需在 sdkconfig.defaults 中配置 `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y` 等选项
- [ ] 接入摔倒检测逻辑到 main.c 主循环
- [ ] 麦克风硬件排查：DC offset ~16328 偏大，可能 L/R 引脚浮空或虚焊
- [ ] README.md 补充：构建烧录步骤、分区表说明、故障排查
