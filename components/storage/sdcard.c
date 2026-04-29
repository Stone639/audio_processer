#include "sdcard.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

static const char *TAG = "SDCARD";
static sdmmc_card_t *s_card = NULL;
static char s_mount_point[32] = {0};
static bool s_mounted = false;

esp_err_t sdcard_mount(int cs_pin, int mosi_pin, int miso_pin, int clk_pin, const char *mount_point)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "SD card already mounted");
        return ESP_OK;
    }

    // 保存挂载点
    strncpy(s_mount_point, mount_point, sizeof(s_mount_point) - 1);
    s_mount_point[sizeof(s_mount_point) - 1] = '\0';

    esp_err_t ret;

    // 1. SPI 总线配置
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = mosi_pin,
        .miso_io_num = miso_pin,
        .sclk_io_num = clk_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2. SD SPI 设备配置
    sdspi_device_config_t slot_cfg = {
        .gpio_cs = cs_pin,
        .gpio_cd = SDSPI_SLOT_NO_CD,
        .gpio_wp = SDSPI_SLOT_NO_WP,
        .gpio_int = GPIO_NUM_NC,
        .host_id = SPI2_HOST,
    };

    // 3. SD 主机配置
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    // 4. 挂载配置
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    // 5. 挂载
    ret = esp_vfs_fat_sdspi_mount(s_mount_point, &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        // 挂载失败时释放 SPI 总线资源
        spi_bus_free(SPI2_HOST);
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted successfully at %s", s_mount_point);
    sdcard_print_info();
    return ESP_OK;
}

void sdcard_unmount(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "SD card not mounted, nothing to unmount");
        return;
    }

    // 卸载 FAT 文件系统
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SD card unmounted");
    }

    // 释放 SPI 总线
    ret = spi_bus_free(SPI2_HOST);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to free SPI bus: %s", esp_err_to_name(ret));
    }

    s_mounted = false;
    s_card = NULL;
    s_mount_point[0] = '\0';
}

bool sdcard_is_mounted(void)
{
    return s_mounted;
}

void sdcard_print_info(void)
{
    if (!s_mounted || s_card == NULL) {
        ESP_LOGW(TAG, "SD card not mounted, cannot print info");
        return;
    }
    sdmmc_card_print_info(stdout, s_card);
}