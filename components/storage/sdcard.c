#include "sdcard.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>
#include <string.h>
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"

static const char *TAG = "SDCARD";
static sdmmc_card_t *s_card = NULL;
static char s_mount_point[32] = {0};
static bool s_mounted = false;
// 固定 SPI 主机号（SPI2_HOST 对应 HSPI），SPI 模式核心标识
#define SDCARD_SPI_HOST SPI2_HOST

/**
 * @brief SPI 模式挂载 SD 卡
 * @param cs_pin     SPI-CS 引脚（GPIO 号）
 * @param mosi_pin   SPI-MOSI 引脚（GPIO 号）
 * @param miso_pin   SPI-MISO 引脚（GPIO 号）
 * @param clk_pin    SPI-CLK 引脚（GPIO 号）
 * @param mount_point 挂载点（如 "/sdcard"）
 * @return esp_err_t 成功返回 ESP_OK，失败返回对应错误码
 */
esp_err_t sdcard_mount(int cs_pin, int mosi_pin, int miso_pin, int clk_pin, const char *mount_point)
{
    // 1. 基础校验
    if (s_mounted) {
        ESP_LOGW(TAG, "SD card already mounted at %s", s_mount_point);
        return ESP_OK;
    }
    if (mount_point == NULL || strlen(mount_point) == 0) {
        ESP_LOGE(TAG, "Invalid mount point");
        return ESP_ERR_INVALID_ARG;
    }
    // 引脚合法性检查（GPIO 0~39 为有效范围）
    const int pins[] = {cs_pin, mosi_pin, miso_pin, clk_pin};
    for (int i = 0; i < 4; i++) {
        if (pins[i] < 0 || pins[i] > 39) {
            ESP_LOGE(TAG, "Invalid GPIO pin: %d", pins[i]);
            return ESP_ERR_INVALID_ARG;
        }
    }

    // 2. 保存挂载点
    strncpy(s_mount_point, mount_point, sizeof(s_mount_point) - 1);
    s_mount_point[sizeof(s_mount_point) - 1] = '\0';

    esp_err_t ret = ESP_OK;

    // 3. SPI 总线配置（SPI 模式核心配置）
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = mosi_pin,    // SPI 主机输出从机输入引脚
        .miso_io_num = miso_pin,    // SPI 主机输入从机输出引脚
        .sclk_io_num = clk_pin,     // SPI 时钟引脚
        .quadwp_io_num = -1,        // SPI 模式无需四线模式，置 -1
        .quadhd_io_num = -1,        // SPI 模式无需四线模式，置 -1
        .max_transfer_sz = 4 * 1024,// 增大最大传输尺寸（适配更大数据块）
        .flags = SPICOMMON_BUSFLAG_GPIO_PINS, // 显式指定 GPIO 引脚模式
    };
    // 初始化 SPI 总线（指定 DMA 通道，提升传输效率）
    ret = spi_bus_initialize(SDCARD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed (host=%d): %s", SDCARD_SPI_HOST, esp_err_to_name(ret));
        return ret;
    }

    // 4. SD SPI 设备配置（SPI 模式专属配置）
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = cs_pin;         // SPI-CS 引脚
    slot_cfg.gpio_cd = SDSPI_SLOT_NO_CD; // 不使用卡检测引脚
    slot_cfg.gpio_wp = SDSPI_SLOT_NO_WP; // 不使用写保护引脚
    slot_cfg.gpio_int = GPIO_NUM_NC;   // 无中断引脚
    slot_cfg.host_id = SDCARD_SPI_HOST;// 绑定到指定 SPI 主机

    // 5. SD SPI 主机配置（SPI 模式核心）
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SDCARD_SPI_HOST;       // 绑定 SPI 主机
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; // SPI 时钟频率（默认 20MHz，可调整）

    // 6. FAT 文件系统挂载配置
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,  // 挂载失败时自动格式化
        .max_files = 8,                  // 最大同时打开文件数
        .allocation_unit_size = 16 * 1024,// 分配单元大小（优化读写效率）
    };

    // 7. 执行 SPI 模式挂载
    ret = esp_vfs_fat_sdspi_mount(s_mount_point, &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card SPI mount failed: %s", esp_err_to_name(ret));
        // 挂载失败，释放 SPI 总线资源
        spi_bus_free(SDCARD_SPI_HOST);
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted via SPI at %s", s_mount_point);
    sdcard_print_info();
    return ESP_OK;
}

/**
 * @brief 卸载 SD 卡（SPI 模式资源释放）
 */
void sdcard_unmount(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "SD card not mounted, skip unmount");
        return;
    }

    // 1. 卸载 FAT 文件系统
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SD card unmounted");
    }

    // 2. 释放 SPI 总线（SPI 模式核心资源释放）
    ret = spi_bus_free(SDCARD_SPI_HOST);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to free SPI bus (host=%d): %s", SDCARD_SPI_HOST, esp_err_to_name(ret));
    }

    // 3. 重置状态
    s_mounted = false;
    s_card = NULL;
    memset(s_mount_point, 0, sizeof(s_mount_point));
}

/**
 * @brief 检查 SD 卡是否已挂载（SPI 模式）
 * @return bool 已挂载返回 true，否则 false
 */
bool sdcard_is_mounted(void)
{
    return s_mounted;
}

/**
 * @brief 打印 SD 卡信息（SPI 模式）
 */
void sdcard_print_info(void)
{
    if (!s_mounted || s_card == NULL) {
        ESP_LOGW(TAG, "SD card not mounted, cannot print info");
        return;
    }
    ESP_LOGI(TAG, "SD card SPI mode info:");
    sdmmc_card_print_info(stdout, s_card);
}