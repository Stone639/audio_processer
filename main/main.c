#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "INMP441";

// 根据实际接线修改 GPIO 编号
#define I2S_BCK_IO      GPIO_NUM_47
#define I2S_WS_IO       GPIO_NUM_10
#define I2S_DIN_IO      GPIO_NUM_21

#define I2S_NUM         I2S_NUM_0
#define SAMPLE_RATE     16000

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing I2S for INMP441...");

    // 1. 创建 I2S 接收通道
    i2s_chan_handle_t rx_handle;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    // 2. 配置标准 I2S 模式
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DIN_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    // 重要：对于 INMP441，需要显式设置 slot 位宽和掩码
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;   // 修正点：ws_width 替代 bit_width
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;         // 只接收左声道

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));

    // 3. 启用通道
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    ESP_LOGI(TAG, "I2S initialized. Start reading audio data...");

    // 4. 循环读取数据
    int32_t sample_buffer[64];
    size_t bytes_read;
    while (1) {
        if (i2s_channel_read(rx_handle, sample_buffer, sizeof(sample_buffer), &bytes_read, portMAX_DELAY) == ESP_OK) {
            int samples_read = bytes_read / sizeof(int32_t);
            for (int i = 0; i < samples_read; i++) {
                // INMP441 24 位数据在 32 位槽中左对齐，需右移 8 位
                int32_t audio_val = sample_buffer[i] >> 8;
                printf("%ld\n", audio_val);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}