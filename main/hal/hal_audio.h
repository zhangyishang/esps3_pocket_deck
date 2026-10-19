#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"

/******************************************************************************/
/***************************   I2S  ↓    **************************************/

#define EXAMPLE_RECV_BUF_SIZE   (2400)
#define EXAMPLE_SAMPLE_RATE     (16000)
#define EXAMPLE_MCLK_MULTIPLE   (384)
#define EXAMPLE_MCLK_FREQ_HZ    (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)
#define EXAMPLE_VOICE_VOLUME    (70)

#define I2S_NUM         (0)
#define I2S_MCK_IO      (GPIO_NUM_38)
#define I2S_BCK_IO      (GPIO_NUM_14)
#define I2S_WS_IO       (GPIO_NUM_13)
#define I2S_DO_IO       (GPIO_NUM_45)
#define I2S_DI_IO       (-1)

/***********************************************************/
/*********************    音频 ↓   *************************/
#define ADC_I2S_CHANNEL 4

#define VOLUME_DEFAULT    50        // 默认声音大小 0~100

#define CODEC_DEFAULT_SAMPLE_RATE          (16000)  // 默认采样率 16K
#define CODEC_DEFAULT_BIT_WIDTH            (16)     // 默认位宽 16bit
#define CODEC_DEFAULT_ADC_VOLUME           (24.0)   // 默认ADC增益 24.0dB
#define CODEC_DEFAULT_CHANNEL              (2)      // 默认通道数 2

#define BSP_I2S_NUM                  I2S_NUM_1

#define GPIO_I2S_LRCK       (GPIO_NUM_13)
#define GPIO_I2S_MCLK       (GPIO_NUM_38)
#define GPIO_I2S_SCLK       (GPIO_NUM_14)
#define GPIO_I2S_SDIN       (GPIO_NUM_12)
#define GPIO_I2S_DOUT       (GPIO_NUM_45)
#define GPIO_PWR_CTRL       (GPIO_NUM_NC)

/**
 * @brief 初始化音频编解码器（ES8311 播放 + ES7210 录音），并设置默认格式与音量。
 * @return ESP_OK 成功
 */
esp_err_t bsp_codec_init(void);

/**
 * @brief 向音频输出写入 PCM 数据。
 * @param audio_buffer  音频数据
 * @param len           字节数
 * @param bytes_written 输出：实际写入的字节数
 * @param timeout_ms    超时（毫秒，当前未使用）
 * @return ESP_OK 成功；其他为 codec 错误码
 */
esp_err_t bsp_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms);

/**
 * @brief 设置采样率、位宽与声道数（相同格式重复设置会被跳过）。
 * @param rate     采样率
 * @param bits_cfg 位宽
 * @param ch       声道模式
 * @return ESP_OK 成功；其他为 codec 错误码
 */
esp_err_t bsp_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch);

/**
 * @brief 设置输出静音。
 * @param enable true 静音，false 取消静音
 * @return ESP_OK 成功；其他为 codec 错误码
 */
esp_err_t bsp_codec_mute_set(bool enable);

/**
 * @brief 设置输出音量。
 * @param volume     音量（0~100）
 * @param volume_set 输出：实际生效的音量，可为 NULL
 * @return ESP_OK 成功；其他为 codec 错误码
 */
esp_err_t bsp_codec_volume_set(int volume, int *volume_set);

/**
 * @brief 获取麦克风的通道数。
 * @return 通道数（ADC_I2S_CHANNEL）
 */
int bsp_get_feed_channel(void);

/**
 * @brief 读取一段麦克风 PCM 数据（可选做通道重排）。
 * @param is_get_raw_channel true 保持原始通道顺序
 * @param buffer             输出缓冲
 * @param buffer_len         字节数
 * @return ESP_OK 成功；其他为 codec 错误码
 */
esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len);

/**
 * @brief 关闭 I2S 发送通道。
 * @return ESP_OK 成功；其他为 I2S 错误码
 */
esp_err_t hal_i2s_tx_disable(void);

/**
 * @brief 向 I2S 发送通道预装数据（通道启用前调用）。
 * @param data    音频数据
 * @param len     字节数
 * @param written 实际装入的字节数
 * @return ESP_OK 成功；其他为 I2S 错误码
 */
esp_err_t hal_i2s_tx_preload(const void *data, size_t len, size_t *written);

/**
 * @brief 使能 I2S 发送通道。
 * @return ESP_OK 成功；其他为 I2S 错误码
 */
esp_err_t hal_i2s_tx_enable(void);

/**
 * @brief 向 I2S 发送通道写入 PCM 数据。
 * @param d       音频数据
 * @param len     字节数
 * @param w       实际写入的字节数
 * @param timeout 超时（FreeRTOS tick）
 * @return ESP_OK 成功；其他为 I2S 错误码
 */
esp_err_t hal_i2s_tx_write(const void *d, size_t len, size_t *w, uint32_t timeout);

/* ==================== 单声道 16k 读写（语音助手用）==================== */

/**
 * @brief 从麦克风读一段单声道 PCM（内部把 4 通道 TDM 抽第 0 路）。
 * @param dst        输出缓冲
 * @param samples    样本数
 * @param timeout_ms 超时（毫秒，当前未使用）
 * @return ESP_OK 成功；其他为 codec 错误码
 */
esp_err_t hal_mic_read_mono(int16_t *dst, size_t samples, uint32_t timeout_ms);

/**
 * @brief 向喇叭写一段单声道 PCM（内部复制成左右两路）。
 * @param src        单声道 PCM
 * @param samples    样本数
 * @param timeout_ms 超时（毫秒）
 * @return ESP_OK 成功；其他为 codec 错误码
 */
esp_err_t hal_spk_write_mono(const int16_t *src, size_t samples, uint32_t timeout_ms);
