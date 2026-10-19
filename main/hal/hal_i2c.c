#include <stdio.h>
#include "hal_i2c.h"
#include "esp_log.h"

static const char *TAG = "esp32_s3_szp";

/**
 * @brief 全局 I2C 总线句柄（所有器件共用一条总线，由驱动层保证并发互斥）。
 */
static i2c_master_bus_handle_t i2c_bus_handle = NULL;

/**
 * @brief 获取 I2C 总线句柄。
 * @return 总线句柄；bsp_i2c_init() 未成功时为 NULL
 */
i2c_master_bus_handle_t hal_i2c_get_bus(void)
{
    return i2c_bus_handle;
}

/******************************************************************************/
/***************************  I2C ↓ *******************************************/
/**
 * @brief 创建并初始化 I2C 主机总线。
 * @return ESP_OK 成功；其他为 i2c_new_master_bus 的错误码
 */
esp_err_t bsp_i2c_init(void)
{
    ESP_LOGI(TAG, "Starting I2C bus initialization...");

    i2c_master_bus_config_t i2c_conf = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_LOGI(TAG, "I2C config: port=%d, scl=%d, sda=%d, freq=%d", BSP_I2C_NUM, BSP_I2C_SCL, BSP_I2C_SDA, BSP_I2C_FREQ_HZ);

    esp_err_t ret = i2c_new_master_bus(&i2c_conf, &i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2C bus initialized successfully");
    return ESP_OK;
}
/***************************  I2C ↑  *******************************************/
/*******************************************************************************/
