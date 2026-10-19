#include <stdio.h>
#include "hal_touch.h"
#include "hal_display.h"
#include "hal_i2c.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft5x06.h"

static const char *TAG = "esp32_s3_szp";


// 触摸屏初始化
esp_err_t bsp_touch_new(esp_lcd_touch_handle_t *ret_touch)
{
    ESP_LOGI(TAG, "Starting touch screen initialization...");
    
    /* Initialize touch */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_V_RES,
        .y_max = BSP_LCD_H_RES,
        .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
        .int_gpio_num = GPIO_NUM_NC, 
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_config.scl_speed_hz = 400000;
    
    ESP_LOGI(TAG, "Creating touch screen I2C IO on bus: %p", hal_i2c_get_bus());
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(hal_i2c_get_bus(), &tp_io_config, &tp_io_handle), TAG, "");
    
    ESP_LOGI(TAG, "Creating FT5x06 touch screen driver");
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, ret_touch));
    
    ESP_LOGI(TAG, "Touch screen initialized successfully");
    return ESP_OK;
}
