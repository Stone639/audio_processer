#ifndef SDCARD_H
#define SDCARD_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并挂载 SD 卡（SPI 模式）
 * @param cs_pin      CS 引脚号
 * @param mosi_pin    MOSI 引脚号
 * @param miso_pin    MISO 引脚号
 * @param clk_pin     CLK 引脚号
 * @param mount_point 挂载点路径（例如 "/sdcard"）
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t sdcard_mount(int cs_pin, int mosi_pin, int miso_pin, int clk_pin, const char *mount_point);

/**
 * @brief 卸载 SD 卡并释放资源
 */
void sdcard_unmount(void);

/**
 * @brief 检查 SD 卡是否已挂载
 * @return true 已挂载，false 未挂载
 */
bool sdcard_is_mounted(void);

/**
 * @brief 获取 SD 卡容量信息（打印到 stdout）
 */
void sdcard_print_info(void);

#ifdef __cplusplus
}
#endif

#endif