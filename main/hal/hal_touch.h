#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"

/**
 * @brief 创建触摸屏设备（FT5x06，走 I2C）。
 * @param ret_touch 输出：触摸句柄
 * @return ESP_OK 成功；其他为驱动错误码
 */
esp_err_t bsp_touch_new(esp_lcd_touch_handle_t *ret_touch);
