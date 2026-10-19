#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/***********************************************************/
/****************    LCD显示屏 ↓   *************************/
#define BSP_LCD_PIXEL_CLOCK_HZ     (80 * 1000 * 1000)
#define BSP_LCD_SPI_NUM            (SPI3_HOST)
#define LCD_CMD_BITS               (8)
#define LCD_PARAM_BITS             (8)
#define BSP_LCD_BITS_PER_PIXEL     (16)
#define LCD_LEDC_CH          LEDC_CHANNEL_0

#define BSP_LCD_H_RES              (320)
#define BSP_LCD_V_RES              (240)

#define BSP_LCD_SPI_MOSI      (GPIO_NUM_40)
#define BSP_LCD_SPI_CLK       (GPIO_NUM_41)
#define BSP_LCD_SPI_CS        (GPIO_NUM_NC)
#define BSP_LCD_DC            (GPIO_NUM_39)
#define BSP_LCD_RST           (GPIO_NUM_NC)
#define BSP_LCD_BACKLIGHT     (GPIO_NUM_42)  

#define BSP_LCD_DRAW_BUF_HEIGHT    (20)

/**
 * @brief 初始化背光 PWM（LEDC）。
 * @return ESP_OK 成功；其他为 LEDC 错误码
 */
esp_err_t bsp_display_brightness_init(void);

/**
 * @brief 设置背光亮度。
 * @param brightness_percent 亮度百分比（0~100）
 * @return ESP_OK 成功；其他为 LEDC 错误码
 */
esp_err_t bsp_display_brightness_set(int brightness_percent);

/**
 * @brief 关闭背光。
 * @return ESP_OK 成功
 */
esp_err_t bsp_display_backlight_off(void);

/**
 * @brief 打开背光。
 * @return ESP_OK 成功
 */
esp_err_t bsp_display_backlight_on(void);

/**
 * @brief 初始化 LCD（ST7789）与面板 IO，并配置显示参数。
 * @return ESP_OK 成功；其他为 LCD 初始化错误码
 */
esp_err_t bsp_lcd_init(void);

/**
 * @brief 用单一颜色填充整屏。
 * @param color RGB565 颜色值
 */
void lcd_set_color(uint16_t color);

/**
 * @brief 在指定矩形区域绘制一张位图。
 * @param x_start 起始 X
 * @param y_start 起始 Y
 * @param x_end   结束 X
 * @param y_end   结束 Y
 * @param gImage  位图数据（RGB565）
 */
void lcd_draw_pictrue(int x_start, int y_start, int x_end, int y_end, const unsigned char *gImage);

/**
 * @brief 初始化 SPI 总线、面板 IO 与 ST7789 驱动（显示子系统总入口）。
 * @return ESP_OK 成功；其他为 SPI / LCD 错误码
 */
esp_err_t bsp_display_new(void);

/**
 * @brief 获取 LCD 面板句柄。
 * @return 面板句柄
 */
esp_lcd_panel_handle_t    hal_display_panel_handle(void);

/**
 * @brief 获取 LCD 面板 IO 句柄。
 * @return 面板 IO 句柄
 */
esp_lcd_panel_io_handle_t hal_display_io_handle(void);
/***************    LCD显示屏 ↑   *************************/
/***********************************************************/
