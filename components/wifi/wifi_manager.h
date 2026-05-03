#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WiFi STA 模式并连接
 *        内部处理 NVS、netif、event loop、WiFi 驱动初始化
 * @return ESP_OK 成功启动连接流程
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 查询 WiFi 是否已连接并获取到 IP
 */
bool wifi_manager_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif
