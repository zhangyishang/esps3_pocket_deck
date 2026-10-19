#include <stdio.h>
#include "hal_qmi8658.h"
#include "hal_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "math.h"

static const char *TAG = "esp32_s3_szp";

/*******************************************************************************/
/***************************  姿态传感器 QMI8658 ↓   ****************************/

/**
 * @brief QMI8658 在 I2C 总线上的设备句柄（qmi8658_init 里注册设备后保存）。
 */
static i2c_master_dev_handle_t qmi8658_dev = NULL;

/**
 * @brief 读取 QMI8658 寄存器。
 * @param reg_addr 寄存器地址
 * @param data     输出缓冲
 * @param len      读取字节数
 * @return ESP_OK 成功；其他为 I2C 错误码
 */
static esp_err_t qmi8658_register_read(uint8_t reg_addr, uint8_t *data, size_t len)
{
    if (qmi8658_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2c_master_transmit_receive(qmi8658_dev, &reg_addr, 1, data, len, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 read failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 写 QMI8658 单个寄存器。
 * @param reg_addr 寄存器地址
 * @param data     写入值
 * @return ESP_OK 成功；其他为 I2C 错误码
 */
static esp_err_t qmi8658_register_write_byte(uint8_t reg_addr, uint8_t data)
{
    if (qmi8658_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t write_buf[2] = {reg_addr, data};
    esp_err_t ret = i2c_master_transmit(qmi8658_dev, write_buf, sizeof(write_buf), 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 初始化 QMI8658：注册设备、校验 WHO_AM_I、复位并配置运动状态检测。
 * @return ESP_OK 成功；ESP_FAIL 芯片 ID 不符；其他为 I2C 错误码
 */
esp_err_t qmi8658_init(void)
{
    esp_err_t ret = ESP_OK;
    uint8_t id = 0;
    uint8_t count = 0;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_SENSOR_ADDR,
        .scl_speed_hz = BSP_I2C_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(hal_i2c_get_bus(), &dev_cfg, &qmi8658_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 add device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    qmi8658_register_read(QMI8658_WHO_AM_I, &id ,1); // 读芯片的ID号
    while (id != 0x05)  // 判断读到的ID号是否是0x05
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);  // 延时100豪秒
        qmi8658_register_read(QMI8658_WHO_AM_I, &id ,1); // 读取ID号
        count++;
        if (count>=3){
            ret = ESP_FAIL;
            return ret;
        }
    }
    ESP_LOGI(TAG, "QMI8658 OK!");  // 打印信息

    qmi8658_register_write_byte(QMI8658_RESET, 0xb0);  // 复位  
    vTaskDelay(10 / portTICK_PERIOD_MS);  // 延时10ms

    // 配置运动状态检测
    qmi8658_register_write_byte(QMI8658_CATL1_L, 1); // AnyMotionXThr 必须是0~32之间的数
    qmi8658_register_write_byte(QMI8658_CATL1_H, 1); // AnyMotionYThr 必须是0~32之间的数
    qmi8658_register_write_byte(QMI8658_CATL2_L, 1); // AnyMotionZThr 必须是0~32之间的数
    qmi8658_register_write_byte(QMI8658_CATL2_H, 1); // NoMotionXThr 必须是0~32之间的数
    qmi8658_register_write_byte(QMI8658_CATL3_L, 1); // NoMotionYThr 必须是0~32之间的数
    qmi8658_register_write_byte(QMI8658_CATL3_H, 1); // NoMotionZThr 必须是0~32之间的数
    qmi8658_register_write_byte(QMI8658_CATL4_L, 0x77); // MOTION_MODE_CTRL 0111 0111
    qmi8658_register_write_byte(QMI8658_CATL4_H, 0x01); // 0x01(means 1st command)
    qmi8658_register_write_byte(QMI8658_CTRL9, 0x0E); // CTRL_CMD_CONFIGURE_MOTION

    qmi8658_register_write_byte(QMI8658_CATL1_L, 1); // AnyMotionWindow 
    qmi8658_register_write_byte(QMI8658_CATL1_H, 1); // NoMotionWindow 
    qmi8658_register_write_byte(QMI8658_CATL2_L, 0xE8); // SigMotionWaitWindow[7:0]
    qmi8658_register_write_byte(QMI8658_CATL2_H, 0x03); // SigMotionWaitWindow [15:8]
    qmi8658_register_write_byte(QMI8658_CATL3_L, 0xE8); // SigMotionConfirmWindow[7:0]
    qmi8658_register_write_byte(QMI8658_CATL3_H, 0x03); // SigMotionConfirmWindow[15:8]
    // qmi8658_register_write_byte(QMI8658_CATL4_L, 0x40); // NA
    qmi8658_register_write_byte(QMI8658_CATL4_H, 0x02); // 0x02(means 2nd command)
    qmi8658_register_write_byte(QMI8658_CTRL9, 0x0E); // CTRL_CMD_CONFIGURE_MOTION


    qmi8658_register_write_byte(QMI8658_CTRL1, 0x40); // CTRL1 设置地址自动增加
    qmi8658_register_write_byte(QMI8658_CTRL7, 0x03); // CTRL7 允许加速度和陀螺仪
    qmi8658_register_write_byte(QMI8658_CTRL2, 0x95); // CTRL2 设置ACC 4g 250Hz
    qmi8658_register_write_byte(QMI8658_CTRL3, 0xd5); // CTRL3 设置GRY 512dps 250Hz 

    qmi8658_register_write_byte(QMI8658_CTRL8, 0x0E); // CTRL7 允许Any-Motion No-Motion and Significant-Motion

    return ret;
}

/**
 * @brief 关闭 QMI8658（进入低功耗待机）。
 */
void qmi8658_close(void)
{
    qmi8658_register_write_byte(QMI8658_CTRL1, 0x01);
}

/**
 * @brief 读取加速度与陀螺仪的原始值（内部使用，仅在数据就绪时更新）。
 * @param p 姿态数据结构
 */
static void qmi8658_Read_AccAndGry(t_sQMI8658 *p)
{
    uint8_t status, data_ready=0;
    int16_t buf[6];

    qmi8658_register_read(QMI8658_STATUS0, &status, 1);
    if (status & 0x03)
        data_ready = 1;
    if (data_ready == 1){
        data_ready = 0;
        qmi8658_register_read(QMI8658_AX_L, (uint8_t *)buf, 12);
        p->acc_x = buf[0];
        p->acc_y = buf[1];
        p->acc_z = buf[2];
        p->gyr_x = buf[3];
        p->gyr_y = buf[4];
        p->gyr_z = buf[5];
    }
}

/**
 * @brief 由加速度值计算三轴倾角（度）。
 * @param p 姿态数据结构（输出 AngleX / AngleY / AngleZ）
 */
void qmi8658_fetch_angleFromAcc(t_sQMI8658 *p)
{
    float temp;

    qmi8658_Read_AccAndGry(p);
    if (p->acc_x == 0 && p->acc_y == 0 && p->acc_z == 0)
    {
        p->AngleX = 0.0f;
        p->AngleY = 0.0f;
        p->AngleZ = 0.0f;
        return;
    }
    temp = (float)p->acc_x / sqrt( ((float)p->acc_y * (float)p->acc_y + (float)p->acc_z * (float)p->acc_z) );
    p->AngleX = atan(temp)*57.29578f;
    temp = (float)p->acc_y / sqrt( ((float)p->acc_x * (float)p->acc_x + (float)p->acc_z * (float)p->acc_z) );
    p->AngleY = atan(temp)*57.29578f;
    temp = sqrt( ((float)p->acc_x * (float)p->acc_x + (float)p->acc_y * (float)p->acc_y) ) / (float)p->acc_z;
    p->AngleZ = atan(temp)*57.29578f;
}

/**
 * @brief 读取运动状态寄存器（Any-Motion / No-Motion / Significant-Motion 标志）。
 * @return STATUS1 寄存器的原始值
 */
uint8_t qmi8658_fetch_motion(void)
{
    uint8_t status = 0;
    qmi8658_register_read(QMI8658_STATUS1, &status, 1);
    return status;
}

/***************************  姿态传感器 QMI8658 ↑  ****************************/
/*******************************************************************************/
