#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

/***********************************************************/
/***************    IO扩展芯片 ↓   *************************/
#define PCA9557_INPUT_PORT              0x00
#define PCA9557_OUTPUT_PORT             0x01
#define PCA9557_POLARITY_INVERSION_PORT 0x02
#define PCA9557_CONFIGURATION_PORT      0x03

#define LCD_CS_GPIO                 BIT(0)
#define PA_EN_GPIO                  BIT(1)
#define DVP_PWDN_GPIO               BIT(2)

#define PCA9557_SENSOR_ADDR             0x19

#define SET_BITS(_m, _s, _v)  ((_v) ? (_m)|((_s)) : (_m)&~((_s)))

/**
 * @brief 初始化 IO 扩展芯片（配置三个引脚为输出，并写入安全默认值：
 *        摄像头掉电、功放静音、LCD 片选拉高）。
 */
void pca9557_init(void);

/**
 * @brief 控制 LCD 片选。
 * @param level 0 输出低电平，非 0 输出高电平
 */
void lcd_cs(uint8_t level);

/**
 * @brief 控制音频功放使能。
 * @param level 0 静音，非 0 使能
 */
void pa_en(uint8_t level);

/**
 * @brief 控制摄像头电源。
 * @param level 0 上电，非 0 进入掉电模式
 */
void dvp_pwdn(uint8_t level);
/***************    IO扩展芯片 ↑   *************************/
/***********************************************************/
