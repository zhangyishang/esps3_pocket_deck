#include <stdio.h>
#include "hal_pca9557.h"
#include "hal_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "esp32_s3_szp";

/***********************************************************/
/***************    IO扩展芯片 ↓   *************************/


static i2c_master_dev_handle_t pca9557_dev = NULL;// PCA9557 在 I2C 总线上的设备句柄。

// 读取PCA9557寄存器的值
static esp_err_t pca9557_register_read(uint8_t reg_addr, uint8_t *data, size_t len)
{
    if (pca9557_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2c_master_transmit_receive(pca9557_dev, &reg_addr, 1, data, len, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 read failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

// 给PCA9557的寄存器写值
static esp_err_t pca9557_register_write_byte(uint8_t reg_addr, uint8_t data)
{
    if (pca9557_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t write_buf[2] = {reg_addr, data};
    esp_err_t ret = i2c_master_transmit(pca9557_dev, write_buf, sizeof(write_buf), 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

// 初始化PCA9557 IO扩展芯片
void pca9557_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_SENSOR_ADDR,
        .scl_speed_hz = BSP_I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(hal_i2c_get_bus(), &dev_cfg, &pca9557_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 add device failed: %s", esp_err_to_name(ret));
        return;
    }

    // 写入控制引脚默认值 DVP_PWDN=1  PA_EN = 0  LCD_CS = 1
    pca9557_register_write_byte(PCA9557_OUTPUT_PORT, 0x05);
    // 把PCA9557芯片的IO1 IO1 IO2设置为输出 其它引脚保持默认的输入
    pca9557_register_write_byte(PCA9557_CONFIGURATION_PORT, 0xf8);
}

// 设置PCA9557芯片的某个IO引脚输出高低电平
static esp_err_t pca9557_set_output_state(uint8_t gpio_bit, uint8_t level)
{
    uint8_t data;
    esp_err_t res = ESP_FAIL;

    pca9557_register_read(PCA9557_OUTPUT_PORT, &data, 1);
    res = pca9557_register_write_byte(PCA9557_OUTPUT_PORT, SET_BITS(data, gpio_bit, level));

    return res;
}

// 控制 PCA9557_LCD_CS 引脚输出高低电平 参数0输出低电平 参数1输出高电平
void lcd_cs(uint8_t level)
{
    pca9557_set_output_state(LCD_CS_GPIO, level);
}

// 控制 PCA9557_PA_EN 引脚输出高低电平 参数0输出低电平 参数1输出高电平
void pa_en(uint8_t level)
{
    pca9557_set_output_state(PA_EN_GPIO, level);
}

// 控制 PCA9557_DVP_PWDN 引脚输出高低电平 参数0输出低电平 参数1输出高电平
void dvp_pwdn(uint8_t level)
{
    pca9557_set_output_state(DVP_PWDN_GPIO, level);
}

/***************    IO扩展芯片 ↑   *************************/
/***********************************************************/
