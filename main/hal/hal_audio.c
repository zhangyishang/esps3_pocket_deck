#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "hal_audio.h"
#include "hal_i2c.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"      // heap_caps_malloc：单声道转换用的临时缓冲放 PSRAM
#include "driver/i2s_std.h"

static const char *TAG = "esp32_s3_szp";

/*********************    音频 ↓   *************************/
static esp_codec_dev_handle_t play_dev_handle;    // speaker句柄
static esp_codec_dev_handle_t record_dev_handle;  // microphone句柄

static i2s_chan_handle_t i2s_tx_chan = NULL; // 发送通道
static i2s_chan_handle_t i2s_rx_chan = NULL; // 接收通道
static const audio_codec_data_if_t *i2s_data_if = NULL;  /* Codec data interface */


/**
  * @brief  初始化音频输出芯片
  * @param  None
  * @retval 音频输出句柄
  */
static esp_err_t bsp_audio_init(void)
{
    esp_err_t ret = ESP_FAIL;
    if (i2s_tx_chan && i2s_rx_chan) {
        /* Audio was initialized before */
        return ESP_OK;
    }

    /* Setup I2S peripheral */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan)); //

    /* Setup I2S channels */
    const i2s_std_config_t std_cfg_default = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),   // 采样率16000
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(32, I2S_SLOT_MODE_STEREO),  // 32位 2通道
        .gpio_cfg = { 
            .mclk = GPIO_I2S_MCLK, 
            .bclk = GPIO_I2S_SCLK, 
            .ws   = GPIO_I2S_LRCK, 
            .dout = GPIO_I2S_DOUT, 
            .din  = GPIO_I2S_SDIN, 
        },
    };

    if (i2s_tx_chan != NULL) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_tx_chan, &std_cfg_default), err, TAG, "I2S channel initialization failed");
        ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_tx_chan), err, TAG, "I2S enabling failed");
    }
    if (i2s_rx_chan != NULL) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_rx_chan, &std_cfg_default), err, TAG, "I2S channel initialization failed");
        ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_rx_chan), err, TAG, "I2S enabling failed");
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BSP_I2S_NUM,
        .rx_handle = i2s_rx_chan,
        .tx_handle = i2s_tx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);  // 创建 I2S 数据接口
    if (i2s_data_if == NULL) {   
        goto err;
    }   

    return ESP_OK;

err:
    if (i2s_tx_chan) {
        i2s_del_channel(i2s_tx_chan);
    }
    if (i2s_rx_chan) {
        i2s_del_channel(i2s_rx_chan);
    }

    return ret;
}

/**
  * @brief  初始化音频输出芯片
  * @param  None
  * @retval 音频输出句柄
  */
static esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    ESP_LOGI(TAG, "Starting audio codec speaker initialization...");
    
    if (i2s_data_if == NULL) {
        /* Configure I2S peripheral and Power Amplifier */
        ESP_LOGI(TAG, "Initializing I2S peripheral...");
        ESP_ERROR_CHECK(bsp_audio_init());
    }
    assert(i2s_data_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    ESP_LOGI(TAG, "Creating I2C control interface for ES8311 codec on bus: %p", hal_i2c_get_bus());
    audio_codec_i2c_cfg_t i2c_cfg = {
        .bus_handle = hal_i2c_get_bus(),
        .addr = ES8311_CODEC_DEFAULT_ADDR, //codec组件库中的默认地址 0x30
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg); // 创建 I2C 控制接口
    assert(i2c_ctrl_if);
    ESP_LOGI(TAG, "I2C control interface created successfully");

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,   // PA 电源电压
        .codec_dac_voltage = 3.3,  // Codec 芯片 DAC 电压
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_PWR_CTRL,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg); // 创建 ES8311 编解码器设备接口
    assert(es8311_dev);

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };   // 创建音频编解码器设备配置结构体
    return esp_codec_dev_new(&codec_dev_cfg);  // 返回创建音频编解码器设备句柄
}

/**
  * @brief  初始化音频输入芯片
  * @param  None
  * @retval 音频输入句柄
  */
static esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    ESP_LOGI(TAG, "Starting audio codec microphone initialization...");
    // i2s_data_if 是全局/静态变量，如果为空就再初始化一次i2s。
    if (i2s_data_if == NULL) {
        /* Configure I2S peripheral and Power Amplifier */
        ESP_LOGI(TAG, "Initializing I2S peripheral...");
        ESP_ERROR_CHECK(bsp_audio_init());
    }
    assert(i2s_data_if);

    ESP_LOGI(TAG, "Creating I2C control interface for ES7210 codec on bus: %p", hal_i2c_get_bus());
    audio_codec_i2c_cfg_t i2c_cfg = {
        .bus_handle = hal_i2c_get_bus(),
        .addr = 0x82,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(i2c_ctrl_if);
    ESP_LOGI(TAG, "I2C control interface created successfully");
    // 创建 ES7210 编解码器设备接口
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = i2c_ctrl_if,        
        .mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2 | ES7120_SEL_MIC3 | ES7120_SEL_MIC4, // 选择所有麦克风，但只有两个也不碍事
    };
    const audio_codec_if_t *es7210_dev = es7210_codec_new(&es7210_cfg); 
    assert(es7210_dev);
    //包装成统一的 esp_codec_dev 句柄
    esp_codec_dev_cfg_t codec_es7210_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_es7210_dev_cfg);
}
/**
  * @brief  设置音频编解码器的采样率、位宽和通道数
  * @param  rate: 采样率
  * @param  bits_cfg: 位宽
  * @param  ch: 通道数
  * @retval ESP_OK: 设置成功
  * @retval ESP_FAIL: 设置失败
  */
esp_err_t bsp_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    esp_err_t ret = ESP_OK;

    static bool   applied   = false;
    static uint32_t last_rate = 0;
    static uint32_t last_bits = 0;
    static i2s_slot_mode_t last_ch = I2S_SLOT_MODE_MONO;
    
    if (applied && last_rate == rate && last_bits == bits_cfg && last_ch == ch) {
        return ESP_OK;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate,
        .channel = ch,
        .bits_per_sample = bits_cfg,
    };
    
    if (play_dev_handle) {
        ret = esp_codec_dev_close(play_dev_handle);
    }
    
    if (record_dev_handle) {
        ret |= esp_codec_dev_close(record_dev_handle);
        ret |= esp_codec_dev_set_in_gain(record_dev_handle, CODEC_DEFAULT_ADC_VOLUME);
    }

    // 缓存判断只看 open 的结果：close 的返回值可能带无关噪音，不代表格式是否生效
    esp_err_t open_ret = ESP_OK;
    if (play_dev_handle) {
        open_ret = esp_codec_dev_open(play_dev_handle, &fs);
    }
    if (record_dev_handle) {
        open_ret |= esp_codec_dev_open(record_dev_handle, &fs);
    }
    if (open_ret == ESP_OK) {
        applied   = true;
        last_rate = rate;
        last_bits = bits_cfg;
        last_ch   = ch;
    }

    ret |= open_ret;
    return ret;
}

/**
  * @brief  初始化音频编解码器
  * @param  None
  * @retval ESP_OK,初始化成功
  * @retval ESP_FAIL,初始化失败
  */
esp_err_t bsp_codec_init(void)
{
    play_dev_handle = bsp_audio_codec_speaker_init();
    assert((play_dev_handle) && "play_dev_handle not initialized"); // 播放设备句柄
    
    record_dev_handle = bsp_audio_codec_microphone_init();
    assert((record_dev_handle) && "record_dev_handle not initialized"); // 录音设备句柄 断言句柄非空，否则程序中止，并打印提示信息

    bsp_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);
    esp_codec_dev_set_out_vol(play_dev_handle, VOLUME_DEFAULT);

    return ESP_OK;
}

/**
  * @brief  播放音频，将音频数据写入音频输出设备
  * @param  audio_buffer: 音频数据缓冲区
  * @param  len: 音频数据长度
  * @param  bytes_written: 实际写入的字节数
  * @param  timeout_ms: 超时时间（毫秒）
  * @retval ESP_OK: 播放成功
  * @retval ESP_FAIL: 播放失败
  */
esp_err_t bsp_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_write(play_dev_handle, audio_buffer, len);
    *bytes_written = len;
    return ret;
}

// 设置静音与否
esp_err_t bsp_codec_mute_set(bool enable)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_set_out_mute(play_dev_handle, enable);
    return ret;
}

/**
  * @brief  设置音量
  * @param  volume: 音量值
  * @param  volume_set: 实际设置的音量值
  * @retval ESP_OK: 设置成功
  * @retval ESP_FAIL: 设置失败
  */
esp_err_t bsp_codec_volume_set(int volume, int *volume_set)
{
    esp_err_t ret = ESP_OK;
    float v = volume;
    ret = esp_codec_dev_set_out_vol(play_dev_handle, (int)v);
    return ret;
}

// 获取MIC通道数
int bsp_get_feed_channel(void)
{
    return ADC_I2S_CHANNEL;
}

/**
  * @brief  把I2S数据重新排布成模型需要的顺序
  * @param  is_get_raw_channel: 是否获取原始通道数据
  * @param  buffer: 存放音频数据的缓冲区
  * @param  buffer_len: 缓冲区长度
  * @retval ESP_OK: 获取成功
  * @retval ESP_FAIL: 获取失败
  */
esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len)
{
    esp_err_t ret = ESP_OK;
    
    int audio_chunksize = buffer_len / (sizeof(int16_t) * ADC_I2S_CHANNEL);  

    ret = esp_codec_dev_read(record_dev_handle, (void *)buffer, buffer_len);
    
    if (!is_get_raw_channel) {
        for (int i = 0; i < audio_chunksize; i++) {
            int16_t ref = buffer[4 * i + 0];
            buffer[3 * i + 0] = buffer[4 * i + 1];
            buffer[3 * i + 1] = buffer[4 * i + 3];
            buffer[3 * i + 2] = ref;
        }
    }

    return ret;
}

/**
 * @brief 关闭 I2S 发送通道。
 * @return ESP_OK 成功；其他为 I2S 错误码
 */
esp_err_t hal_i2s_tx_disable(void)
{
    return i2s_channel_disable(i2s_tx_chan);
}

/**
 * @brief 向 I2S 发送通道预装数据（通道启用前调用，避免起播时的空白帧）。
 * @param data    音频数据
 * @param len     字节数
 * @param written 实际装入的字节数
 * @return ESP_OK 成功；其他为 I2S 错误码
 */
esp_err_t hal_i2s_tx_preload(const void *data, size_t len, size_t *written)
{
    return i2s_channel_preload_data(i2s_tx_chan, data, len, written);
}

/**
 * @brief 使能 I2S 发送通道。
 * @return ESP_OK 成功；其他为 I2S 错误码
 */
esp_err_t hal_i2s_tx_enable(void)
{
    return i2s_channel_enable(i2s_tx_chan);
}

/**
 * @brief 向 I2S 发送通道写入 PCM 数据（阻塞直到写完或超时）。
 * @param d       音频数据
 * @param len     字节数
 * @param w       实际写入的字节数
 * @param timeout 超时（FreeRTOS tick）
 * @return ESP_OK 成功；其他为 I2S 错误码
 */
esp_err_t hal_i2s_tx_write(const void *d, size_t len, size_t *w, uint32_t timeout)
{
    return i2s_channel_write(i2s_tx_chan, d, len, w, timeout);
}

/* ==================== 单声道 16k 读写（给语音助手用）==================== */

/**
 * @brief 从麦克风读一段**单声道** PCM。
 *
 * ES7210 配的是 4 通道 TDM，esp_codec_dev_read 只能按 4 通道读。
 * 这里读进 4 通道临时缓冲，再抽第 0 路当单声道返回 —— 其余 3 路是同一批
 * 麦克风的其它通道（阵列），做语音识别用不上。
 *
 * @param dst        输出（单声道，样本数 = samples）
 * @param samples    要的样本数
 * @param timeout_ms 超时（毫秒，当前未使用）
 * @return ESP_OK 成功
 */
esp_err_t hal_mic_read_mono(int16_t *dst, size_t samples, uint32_t timeout_ms)
{
    if (dst == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (record_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 4 通道临时缓冲，放 PSRAM */
    size_t need = samples * ADC_I2S_CHANNEL * sizeof(int16_t);
    static int16_t *raw = NULL;
    static size_t raw_cap = 0;
    if (raw == NULL || raw_cap < need) {
        if (raw != NULL) {
            heap_caps_free(raw);
        }
        raw = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        raw_cap = (raw != NULL) ? need : 0;
        if (raw == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = esp_codec_dev_read(record_dev_handle, (void *)raw, need);
    if (ret != ESP_OK) {
        return ret;
    }
    for (size_t i = 0; i < samples; i++) {
        dst[i] = raw[i * ADC_I2S_CHANNEL];
    }
    return ESP_OK;
}

/**
 * @brief 向喇叭写一段**单声道** PCM。
 *
 * 播放通道配的是 2 声道，所以把单声道复制到左右两路再送。
 *
 * @param src        单声道 PCM
 * @param samples    样本数
 * @param timeout_ms 超时（毫秒）
 * @return ESP_OK 成功
 */
esp_err_t hal_spk_write_mono(const int16_t *src, size_t samples, uint32_t timeout_ms)
{
    if (src == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (play_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes = samples * 2 * sizeof(int16_t);
    static int16_t *st = NULL;
    static size_t st_cap = 0;
    if (st == NULL || st_cap < bytes) {
        if (st != NULL) {
            heap_caps_free(st);
        }
        st = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        st_cap = (st != NULL) ? bytes : 0;
        if (st == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    for (size_t i = 0; i < samples; i++) {
        st[2 * i]     = src[i];
        st[2 * i + 1] = src[i];
    }

    /* 分块写入。
     * 原来是一次 bsp_i2s_write(st, bytes) 把整块塞进去（例如 BEEP 那次是 38KB），
     * 而 bsp_i2s_write 内部把 *bytes_written 直接写成 len —— 即使底层只写进去一部分
     * 也会报全部写完。I2S DMA 一次能吃的量有限，超大块会被截断，
     * 表现就是「返回 ESP_OK 但一点声音都没有」。
     * 音乐 App 没事，是因为播放器每次只来 1~4KB。
     * 这里按 1KB 分块，和播放器的粒度对齐。 */
    const size_t CHUNK = 1024;
    esp_err_t ret = ESP_OK;
    for (size_t off = 0; off < bytes; off += CHUNK) {
        size_t n = (bytes - off < CHUNK) ? (bytes - off) : CHUNK;
        size_t written = 0;
        esp_err_t e = bsp_i2s_write((uint8_t *)st + off, n, &written, timeout_ms);
        if (e != ESP_OK) {
            ret = e;
            break;
        }
    }
    return ret;
}
/*********************    音频 ↑   *************************/
/***********************************************************/
