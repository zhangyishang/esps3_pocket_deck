#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

/******************************************************************************/
/***************************  I2C ↓ *******************************************/
#define BSP_I2C_SDA           (GPIO_NUM_1)
#define BSP_I2C_SCL           (GPIO_NUM_2)

#define BSP_I2C_NUM           (0)
#define BSP_I2C_FREQ_HZ       100000         // 100kHz

/**
 * @brief 创建并初始化 I2C 主机总线。
 * @return ESP_OK 成功；其他为 i2c_new_master_bus 的错误码
 */
esp_err_t bsp_i2c_init(void);

/**
 * @brief 获取 I2C 总线句柄（所有器件共用这一条总线）。
 * @return 总线句柄；bsp_i2c_init() 未成功时为 NULL
 */
i2c_master_bus_handle_t hal_i2c_get_bus(void);
/***************************  I2C ↑  *******************************************/
/*******************************************************************************/
