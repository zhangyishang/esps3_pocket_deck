#include "ui_music.h"
#include "ui_common.h"
#include "audio_player.h"
#include "file_iterator.h"
#include "hal_pca9557.h"
#include "hal_sd.h"
#include "hal_audio.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "ui_msg.h"
#include "freertos/task.h"   // vTaskDelay：seek 里 audio_player_play 失败时重试用
#include <stdio.h>
#include <stdlib.h>     // free()：music_iterator_free 要用
#include <string.h>
#include <sys/stat.h>   // stat() / S_ISDIR：判断音乐目录是否真的存在

static const char *TAG = "app_ui";

/*********************  第2个图标   音乐播放器 *********************************************************************************************/
static audio_player_config_t player_config = {0};
static uint8_t g_sys_volume = VOLUME_DEFAULT;
static file_iterator_instance_t *file_iterator = NULL;

lv_obj_t *music_list;
lv_obj_t *label_play_pause;
lv_obj_t *btn_play_pause;
lv_obj_t *volume_slider;

lv_obj_t *music_title_label;
lv_obj_t *btn_music_back;

/* ---------------- 本次修复新增的生命周期标记 ---------------- */

static bool s_player_created = false;

static volatile bool s_music_alive = false;

static bool s_sd_mounted_by_music = false;

static const char *s_error_text = NULL;

/* ==================== 播放进度 / 定位（seek） ==================== */

static lv_obj_t *progress_slider;      // 可拖动的进度条，取值是 0..100 百分比
static lv_obj_t *lab_time_now;         // 左端 "0:42"
static lv_obj_t *lab_time_total;       // 右端 "3:15"
static lv_timer_t *s_progress_timer;   // 每 500ms 把进度画上去

static char     s_cur_path[128];       // 当前曲目完整路径：定位时要重新打开它
static uint32_t s_duration_ms;         // 总时长，0 = 没解析出来（此时不允许定位）
static long     s_audio_start;         // 音频数据起点（已跳过 ID3v2）
static long     s_audio_bytes;         // 音频数据总字节
static uint32_t s_base_ms;             // 定位后的起点时间，加在 PCM 计时之上

static volatile uint32_t s_pcm_bytes;
static volatile uint32_t s_pcm_bytes_per_sec;

/* ==================== 音乐源（SD 卡上的 /sdcard/music） ==================== */

// 释放 file_iterator 实例。
static void music_iterator_free(file_iterator_instance_t *it)
{
    if (it == NULL) {
        return;
    }
    if (it->list != NULL) {
        for (size_t i = 0; i < it->count; i++) {
            free(it->list[i]);
        }
        free(it->list);
    }
    free((void *)it->directory_path);
    free(it);
}

// 确保音乐源可用：SD 卡已挂载，且 /sdcard/music 是一个真实存在的目录。
static bool music_source_prepare(void)
{
    if (!bsp_sdcard_is_mounted()) {
        esp_err_t ret = bsp_sdcard_mount();   // 幂等：已挂载时直接返回 ESP_OK
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SD 卡挂载失败: %s", esp_err_to_name(ret));
            s_error_text = "SD卡挂载失败\n请检查卡是否插好、格式是否为 FAT";
            return false;
        }
        s_sd_mounted_by_music = true;
    }

    struct stat st;
    if (stat(SD_BASE, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGE(TAG, "音乐目录不存在或不是目录: %s", SD_BASE);
        s_error_text = "SD卡里没有 music 文件夹\n请把音乐放在 SD:/music 下";
        return false;
    }

    return true;
}

/* ---------------------------- MP3 帧头：解析与对齐 ---------------------------- */

// 只认 Layer III：本工程的解码器 libhelix 就是 MP3 用的。
static const int MP3_BR_V1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
static const int MP3_BR_V2[16] = {0,  8, 16, 24, 32, 40, 48, 56,  64,  80,  96, 112, 128, 144, 160, 0};
static const int MP3_SR_V1[4]  = {44100, 48000, 32000, 0};
static const int MP3_SR_V2[4]  = {22050, 24000, 16000, 0};
static const int MP3_SR_V25[4] = {11025, 12000,  8000, 0};

// 解析 4 字节帧头：合法则填好各个出参（都可传 NULL）并返回帧长（字节），否则返回 0。
static long mp3_frame_len(const uint8_t *b,
                          int *out_version, int *out_bitrate, int *out_sample_rate,
                          int *out_samples_per_frame, int *out_channels, int *out_has_crc)
{
    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) {
        return 0;
    }

    int version = (b[1] >> 3) & 0x03;   // 3=MPEG1, 2=MPEG2, 0=MPEG2.5, 1=保留
    int layer   = (b[1] >> 1) & 0x03;   // 1=Layer III
    int br_idx  = (b[2] >> 4) & 0x0F;
    int sr_idx  = (b[2] >> 2) & 0x03;
    int padding = (b[2] >> 1) & 0x01;

    if (version == 1 || layer != 1 || br_idx == 0 || br_idx == 15 || sr_idx == 3) {
        return 0;
    }

    int bitrate = (version == 3) ? MP3_BR_V1[br_idx] : MP3_BR_V2[br_idx];
    int sr = (version == 3) ? MP3_SR_V1[sr_idx]
                            : ((version == 2) ? MP3_SR_V2[sr_idx] : MP3_SR_V25[sr_idx]);
    if (bitrate == 0 || sr == 0) {
        return 0;
    }

    if (out_version)         *out_version = version;
    if (out_bitrate)         *out_bitrate = bitrate;
    if (out_sample_rate)     *out_sample_rate = sr;
    if (out_samples_per_frame) *out_samples_per_frame = (version == 3) ? 1152 : 576;
    if (out_channels)        *out_channels = ((b[3] >> 6) & 0x03) == 3 ? 1 : 2;
    if (out_has_crc)         *out_has_crc = ((b[1] & 0x01) == 0) ? 1 : 0;   // 保护位 0 = 帧头后跟 2 字节 CRC

    // MPEG1 Layer3 帧长 = 144 × 码率 / 采样率 + padding；MPEG2/2.5 用 72
    int coef = (version == 3) ? 144 : 72;
    return (long)coef * bitrate * 1000 / sr + padding;
}

static long mp3_align_to_frame(FILE *fp, long start, long file_size, long limit)
{
    uint8_t buf[256];       // 小块：本函数会在 LVGL 任务上跑，栈只有 6KB
    uint8_t nxt[4];
    const long scan_limit = start + limit;

    for (long scan = start; scan + 4 <= file_size && scan < scan_limit; ) {
        fseek(fp, scan, SEEK_SET);
        size_t got = fread(buf, 1, sizeof(buf), fp);
        if (got < 4) {
            break;
        }

        for (size_t i = 0; i + 4 <= got; i++) {
            long flen = mp3_frame_len(buf + i, NULL, NULL, NULL, NULL, NULL, NULL);
            if (flen <= 0) {
                continue;
            }

            long cand = scan + (long)i;
            if (cand + flen + 4 > file_size) {
                continue;
            }

            // 验第二帧：连续两帧都合法才认
            if (fseek(fp, cand + flen, SEEK_SET) != 0) {
                continue;
            }
            if (fread(nxt, 1, sizeof(nxt), fp) != sizeof(nxt)) {
                continue;
            }
            if (mp3_frame_len(nxt, NULL, NULL, NULL, NULL, NULL, NULL) > 0) {
                return cand;
            }
        }

        if (got < sizeof(buf)) {
            break;                              // 读到文件尾
        }
        scan += (long)got - 3;                  // 留 3 字节重叠，帧头跨界不漏
    }
    return -1;
}

/* ========================================================================= */

/* ---------------------------- MP3 头解析：总时长 ---------------------------- */

static uint32_t music_mp3_duration_ms(const char *path)
{
    s_audio_start = 0;
    s_audio_bytes = 0;

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size <= 0) {
        fclose(fp);
        return 0;
    }

    // ---- 跳过 ID3v2：10 字节头 + 4 个 syncsafe 字节表示的长度 ----
    long pos = 0;
    uint8_t hdr[10];
    fseek(fp, 0, SEEK_SET);
    if (fread(hdr, 1, sizeof(hdr), fp) == sizeof(hdr) &&
        hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        long tag = ((long)(hdr[6] & 0x7F) << 21) | ((long)(hdr[7] & 0x7F) << 14) |
                   ((long)(hdr[8] & 0x7F) << 7)  |  (long)(hdr[9] & 0x7F);
        pos = 10 + tag;
        if (hdr[5] & 0x10) {        // 有 footer 的话再加 10 字节
            pos += 10;
        }
    }
    if (pos >= size) {
        fclose(fp);
        return 0;
    }

    // ---- 找第一个合法帧头 ----
    // 用 mp3_align_to_frame()（连续两帧都合法才认），比内联的"只看 0xFFEx"稳。
    // 扫 64KB 还找不到就当这个文件解析不了：正常情况下音频数据起点后面
    // 几十字节内就有帧头。
    long frame_off = mp3_align_to_frame(fp, pos, size, 64 * 1024);
    if (frame_off < 0) {
        fclose(fp);
        return 0;
    }

    // 帧头在上一句里已经验过，这里再读一次只是为了把各个字段取出来
    uint8_t head[4];
    int version = 0, bitrate = 0, sample_rate = 0;
    int samples_per_frame = 0, channels = 2, has_crc = 0;
    fseek(fp, frame_off, SEEK_SET);
    if (fread(head, 1, sizeof(head), fp) != sizeof(head) ||
        mp3_frame_len(head, &version, &bitrate, &sample_rate,
                      &samples_per_frame, &channels, &has_crc) <= 0) {
        fclose(fp);
        return 0;
    }

    s_audio_start = frame_off;
    s_audio_bytes = size - frame_off;

    // ---- Xing / Info：有精确帧数就用帧数 ----
    int side_info = (version == 3) ? (channels == 1 ? 17 : 32)
                                   : (channels == 1 ?  9 : 17);
    uint8_t xh[12];
    fseek(fp, frame_off + 4 + has_crc * 2 + side_info, SEEK_SET);
    if (fread(xh, 1, sizeof(xh), fp) == sizeof(xh) &&
        ((xh[0] == 'X' && xh[1] == 'i' && xh[2] == 'n' && xh[3] == 'g') ||
         (xh[0] == 'I' && xh[1] == 'n' && xh[2] == 'f' && xh[3] == 'o'))) {
        uint32_t flags = ((uint32_t)xh[4] << 24) | ((uint32_t)xh[5] << 16) |
                         ((uint32_t)xh[6] << 8)  |  (uint32_t)xh[7];
        if (flags & 0x0001) {                    // 帧数存在
            uint32_t frames = ((uint32_t)xh[8] << 24) | ((uint32_t)xh[9] << 16) |
                              ((uint32_t)xh[10] << 8) |  (uint32_t)xh[11];
            fclose(fp);
            return (uint32_t)((uint64_t)frames * samples_per_frame * 1000 / sample_rate);
        }
    }

    fclose(fp);

    return (uint32_t)((uint64_t)s_audio_bytes * 8 / (uint32_t)bitrate);
}

/* ========================================================================= */

// 播放指定序号的音乐
static void play_index(int index)
{
    ESP_LOGI(TAG, "play_index(%d)", index);

    if (file_iterator == NULL) {
        return;
    }

    char filename[128];
    int retval = file_iterator_get_full_path_from_index(file_iterator, (size_t)index, filename, sizeof(filename));
    // 返回 0 = 下标无效；返回 >= sizeof(filename) = 路径被 snprintf 截断了，
    // 截断后的路径指向的是另一个文件，宁可不放。
    if (retval <= 0 || retval >= (int)sizeof(filename)) {
        ESP_LOGE(TAG, "unable to retrieve filename for index %d", index);
        return;
    }

    strlcpy(s_cur_path, filename, sizeof(s_cur_path));
    s_pcm_bytes = 0;
    s_base_ms   = 0;
    s_duration_ms = music_mp3_duration_ms(s_cur_path);

    FILE *fp = fopen(filename, "rb");
    if (fp) {
        ESP_LOGI(TAG, "Playing '%s'", filename);
        audio_player_play(fp);
    } else {
        ESP_LOGE(TAG, "unable to open index %d, filename '%s'", index, filename);
    }
}

// 设置声音处理函数
static esp_err_t _audio_player_mute_fn(AUDIO_PLAYER_MUTE_SETTING setting)
{
    esp_err_t ret = ESP_OK;
    // 判断是否需要静音
    bsp_codec_mute_set(setting == AUDIO_PLAYER_MUTE ? true : false);
    // 如果不是静音 设置音量
    if (setting == AUDIO_PLAYER_UNMUTE) {
        bsp_codec_volume_set(g_sys_volume, NULL);
    }
    ret = ESP_OK;

    return ret;
}

// 播放音乐函数 播放音乐的时候 会不断进入
static esp_err_t _audio_player_write_fn(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;

    ret = bsp_i2s_write(audio_buffer, len, bytes_written, timeout_ms);

    // 进度条的"已播时长"就靠这一句累计出来。
    // 本函数运行在 audio task 上，这里写的 s_pcm_bytes 由 LVGL 任务的定时器读；
    // 它是 32 位单字，Xtensa 上读写原子，不加锁。
    s_pcm_bytes += *bytes_written;

    return ret;
}

// 设置采样率 播放的时候进入一次
static esp_err_t _audio_player_std_clock(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    esp_err_t ret = ESP_OK;
    ESP_LOGI(TAG, "切换采样率: %u Hz, %u bit, %d ch", (unsigned)rate, (unsigned)bits_cfg, (int)ch);
    ret = bsp_codec_set_fs(rate, bits_cfg, ch); // 如果播放的音乐固定是16000采样率 这里可以不用打开 如果采样率未知 把这里打开

    // 每秒写进 I2S 的 PCM 字节数 = 采样率 × 每样本字节数 × 声道数。
    // 拿它把 s_pcm_bytes 换算成时间，比按 MP3 码率估算准得多（与 CBR/VBR 无关）。
    // ch 是 I2S_SLOT_MODE_*，不是声道数，所以自己做一次映射。
    uint32_t channels = (ch == I2S_SLOT_MODE_MONO) ? 1 : 2;
    s_pcm_bytes_per_sec = rate * (bits_cfg / 8) * channels;

    return ret;
}

// 回调函数 播放器每次动作都会进入
//
// **本函数运行在 esp-audio-player 自己的任务上（"Audio Task"），不是 LVGL 任务。**
// 这一条是理解本文件所有并发问题的前提。
static void _audio_player_callback(audio_player_cb_ctx_t *ctx)
{
    ESP_LOGI(TAG, "ctx->audio_event = %d", ctx->audio_event);

    switch (ctx->audio_event) {
    case AUDIO_PLAYER_CALLBACK_EVENT_IDLE: {  // 播放完一首歌 进入这个case
        // 页面已经关了（用户按了返回），此时正在拆除播放器。
        // 这里必须拦住：file_iterator 即将被释放，而"再播一首"会往一条
        // 马上要 vQueueDelete 掉的队列上发请求。
        //
        // 只拦这一个分支 —— PLAYING/PAUSE 那两条只管功放开关（pa_en），
        // 退出流程里 audio_player_delete() 会让状态走到 SHUTDOWN，
        // 功放的关闭由 btn_music_back_cb 显式负责，见那里的说明。
        if (!s_music_alive) {
            break;
        }
        ESP_LOGI(TAG, "AUDIO_PLAYER_REQUEST_IDLE");
        // 指向下一首歌
        file_iterator_next(file_iterator);
        int index = file_iterator_get_index(file_iterator);
        ESP_LOGI(TAG, "playing index '%d'", index);
        play_index(index);
        // 修改当前播放的曲目显示。
        ui_msg_t msg = { .id = UI_MSG_MUSIC_TRACK_CHANGED };
        msg.v.i[0] = index;
        ui_msg_send(&msg, 0);
        break;
    }
    case AUDIO_PLAYER_CALLBACK_EVENT_PLAYING: // 正在播放音乐
        ESP_LOGI(TAG, "AUDIO_PLAYER_REQUEST_PLAY");
        pa_en(1); // 打开音频功放
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_PAUSE: // 正在暂停音乐
        ESP_LOGI(TAG, "AUDIO_PLAYER_REQUEST_PAUSE");
        pa_en(0); // 关闭音频功放
        break;
    default:
        break;
    }
}


// mp3播放器初始化。成功返回 true。
bool mp3_player_init(void)
{
    music_iterator_free(file_iterator);
    file_iterator = file_iterator_new(SD_BASE);
    if (file_iterator == NULL) {
        // file_scan() 失败（内存不足 / 目录在扫描过程中消失）会返回 NULL。
        // 目录本身不存在的情况在上游 music_source_prepare() 就已经挡掉了。
        ESP_LOGE(TAG, "扫描音乐目录失败: %s", SD_BASE);
        s_error_text = "扫描音乐目录失败\n（内存不足或目录被移走）";
        return false;
    }

    // 初始化音频播放
    player_config.mute_fn = _audio_player_mute_fn;
    player_config.write_fn = _audio_player_write_fn;
    player_config.clk_set_fn = _audio_player_std_clock;
    player_config.priority = 6;
    player_config.coreID = 1;

    esp_err_t ret = audio_player_new(player_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_player_new 失败: %s", esp_err_to_name(ret));
        s_error_text = "音频播放器初始化失败";
        return false;
    }
    // 记在创建成功**之后**：删除必须与创建配对，见 s_player_created 的说明。
    s_player_created = true;

    ret = audio_player_callback_register(_audio_player_callback, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册播放回调失败: %s", esp_err_to_name(ret));
        s_error_text = "音频播放器初始化失败";
        return false;
    }

    return true;
}


// 按钮样式相关定义
typedef struct {
    lv_style_t style_bg;
    lv_style_t style_focus_no_outline;
} button_style_t;

static button_style_t g_btn_styles;

button_style_t *ui_button_styles(void)
{
    return &g_btn_styles;
}

// 按钮样式初始化
static void ui_button_style_init(void)
{
    /*Init the style for the default state*/
    lv_style_init(&g_btn_styles.style_focus_no_outline);
    lv_style_set_outline_width(&g_btn_styles.style_focus_no_outline, 0);

    lv_style_init(&g_btn_styles.style_bg);
    lv_style_set_bg_opa(&g_btn_styles.style_bg, LV_OPA_100);
    lv_style_set_bg_color(&g_btn_styles.style_bg, lv_color_make(255, 255, 255));
    lv_style_set_shadow_width(&g_btn_styles.style_bg, 0);
}

// 播放暂停按钮 事件处理函数
static void btn_play_pause_cb(lv_event_t *event)
{
    lv_obj_t *btn = lv_event_get_target(event);
    lv_obj_t *lab = (lv_obj_t *) btn->user_data;

    audio_player_state_t state = audio_player_get_state();
    printf("state=%d\n", state);
    if(state == AUDIO_PLAYER_STATE_IDLE){
        lvgl_port_lock(0);
        lv_label_set_text_static(lab, LV_SYMBOL_PAUSE);
        lvgl_port_unlock();
        int index = file_iterator_get_index(file_iterator);
        ESP_LOGI(TAG, "playing index '%d'", index);
        play_index(index);
    }else if (state == AUDIO_PLAYER_STATE_PAUSE) {
        lvgl_port_lock(0);
        lv_label_set_text_static(lab, LV_SYMBOL_PAUSE);
        lvgl_port_unlock();
        audio_player_resume();
    } else if (state == AUDIO_PLAYER_STATE_PLAYING) {
        lvgl_port_lock(0);
        lv_label_set_text_static(lab, LV_SYMBOL_PLAY);
        lvgl_port_unlock();
        audio_player_pause();
    }
}

// 上一首 下一首 按键事件处理函数
static void btn_prev_next_cb(lv_event_t *event)
{
    bool is_next = (bool) event->user_data;

    if (is_next) {
        ESP_LOGI(TAG, "btn next");
        file_iterator_next(file_iterator);
    } else {
        ESP_LOGI(TAG, "btn prev");
        file_iterator_prev(file_iterator);
    }
    // 修改当前的音乐名称
    int index = file_iterator_get_index(file_iterator);
    lvgl_port_lock(0);
    lv_dropdown_set_selected(music_list, index);
    // lv_obj_t *label_title = (lv_obj_t *) music_list->user_data;
    // lv_label_set_text_static(label_title, file_iterator_get_name_from_index(file_iterator, index));
    lvgl_port_unlock();
    // 执行音乐事件
    audio_player_state_t state = audio_player_get_state();
    printf("prev_next_state=%d\n", state);
    if (state == AUDIO_PLAYER_STATE_IDLE) { 
        // Nothing to do
    }else if (state == AUDIO_PLAYER_STATE_PAUSE){ // 如果当前正在暂停歌曲
        ESP_LOGI(TAG, "playing index '%d'", index);
        play_index(index);
        audio_player_pause();
    } else if (state == AUDIO_PLAYER_STATE_PLAYING) { // 如果当前正在播放歌曲
        // 播放歌曲
        ESP_LOGI(TAG, "playing index '%d'", index);
        play_index(index);
    }
}

// 音量调节滑动条 事件处理函数
static void volume_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    int volume = lv_slider_get_value(slider); // 获取slider的值
    bsp_codec_volume_set(volume, NULL); // 设置声音大小
    g_sys_volume = volume; // 把声音赋值给g_sys_volume保存
    ESP_LOGI(TAG, "volume '%d'", volume);
}

// 音乐列表 点击事件处理函数
static void music_list_cb(lv_event_t *event)
{   
    // 下拉框的弹层是独立对象（挂在 lv_layer_top() 上），不继承下拉框自身的字体。
    // 这个回调在 LV_EVENT_VALUE_CHANGED 时触发，那时弹层已经创建过，
    // 补设一次字体（主题/继承之外的保险）。
    lv_obj_t *popup = lv_dropdown_get_list(music_list);
    if (popup != NULL) {
        lv_obj_set_style_text_font(popup, &font_alipuhui20, 0);
    }

    uint16_t index = lv_dropdown_get_selected(music_list);
    ESP_LOGI(TAG, "switching index to '%d'", index);
    file_iterator_set_index(file_iterator, index);
    
    audio_player_state_t state = audio_player_get_state();
    if (state == AUDIO_PLAYER_STATE_PAUSE){ // 如果当前正在暂停歌曲
        play_index(index);
        audio_player_pause();
    } else if (state == AUDIO_PLAYER_STATE_PLAYING) { // 如果当前正在播放歌曲
        play_index(index);
    }
}

// 下拉框被点开：弹层刚由 lv_dropdown_open 创建出来，立刻给它设中文字体。
// LVGL 的事件顺序是「控件类自己的处理函数先跑，用户回调后跑」，
// 所以进到这里时 lv_dropdown_get_list() 一定已经非空（第一次展开也一样）。
static void music_list_open_cb(lv_event_t *event)
{
    lv_obj_t *popup = lv_dropdown_get_list(music_list);
    if (popup != NULL) {
        lv_obj_set_style_text_font(popup, &font_alipuhui20, 0);
    }
}

// 临时诊断用：判断字符串是不是合法 UTF-8（不进固件就靠它分辨编码问题）。
static bool is_valid_utf8_local(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) { p++; continue; }
        int extra;
        if ((*p & 0xE0) == 0xC0) extra = 1;
        else if ((*p & 0xF0) == 0xE0) extra = 2;
        else if ((*p & 0xF8) == 0xF0) extra = 3;
        else return false;
        p++;
        for (int i = 0; i < extra; i++, p++) {
            if ((*p & 0xC0) != 0x80) return false;
        }
    }
    return true;
}

// 音乐名称加入列表
static void build_file_list(lv_obj_t *music_list)
{
    if (file_iterator == NULL) {
        return;   // 调用方已保证非空，这里只是不让将来多一个调用点就炸
    }

    lvgl_port_lock(0);
    lv_dropdown_clear_options(music_list);
    lvgl_port_unlock();

    for(size_t i = 0; i<file_iterator->count; i++)
    {
        const char *file_name = file_iterator_get_name_from_index(file_iterator, i);
        if (NULL != file_name) {
            // ── 临时诊断：把文件名的原始字节打出来 ──
            // 目的：一次分清是"编码不是 UTF-8"还是"字体里没这个字形"。
            // 看日志时对照：
            //   合法 UTF-8 + 屏幕上仍是方框  -> 字体/显示问题
            //   不合法 UTF-8                -> 编码问题（GBK 之类）
            ESP_LOGI(TAG, "[文件名 %u] 字符串: %s", (unsigned)i, file_name);
            ESP_LOGW(TAG, "[文件名 %u] UTF-8 合法性: %s", (unsigned)i,
                     is_valid_utf8_local(file_name) ? "合法" : "**不合法**");
            ESP_LOG_BUFFER_HEXDUMP(TAG, file_name, strlen(file_name) + 1, ESP_LOG_WARN);

            lvgl_port_lock(0);
            lv_dropdown_add_option(music_list, file_name, i); // 添加音乐名称到列表中
            lvgl_port_unlock();
        }
    }
    lvgl_port_lock(0);
    lv_dropdown_set_selected(music_list, 0); // 选择列表中的第一个
    lvgl_port_unlock();
}

/* ==================== 进度条：显示 + 拖动定位 ==================== */

// 按 PCM 字节数算出已播时长。s_base_ms 是定位后的起点。
static uint32_t music_elapsed_ms(void)
{
    uint32_t rate = s_pcm_bytes_per_sec;
    if (rate == 0) {
        return s_base_ms;
    }
    return s_base_ms + (uint32_t)((uint64_t)s_pcm_bytes * 1000 / rate);
}

// 百分比（滑块取值 0..100）换算成毫秒。时长未知时返回 0。
static uint32_t music_percent_to_ms(int32_t percent)
{
    if (s_duration_ms == 0 || percent <= 0) {
        return 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    return (uint32_t)((uint64_t)s_duration_ms * (uint32_t)percent / 100);
}

static void music_time_label_set(lv_obj_t *lab, uint32_t ms)
{
    if (lab == NULL) {
        return;
    }
    lv_label_set_text_fmt(lab, "%u:%02u",
                          (unsigned)(ms / 60000u), (unsigned)((ms / 1000u) % 60u));
}

// 定位到指定时间。
//
// 做法：重新 fopen 当前曲目，fseek 到按比例折算出的字节偏移，再 audio_player_play()。
// 组件收到新的 PLAY 请求会中断正在播的那个文件并 fclose 掉它的 FILE*，所以
// **不需要**先 audio_player_stop()，也不会泄漏文件句柄。
//
// 只允许在 LVGL 任务上调用（本函数不加锁，见本文件开头关于死锁的说明）。
static void music_seek_to_ms(uint32_t target_ms)
{
    if (s_cur_path[0] == '\0') {
        ESP_LOGW(TAG, "seek: 还没有当前曲目（先选一首歌再拖）");
        return;
    }
    if (s_duration_ms == 0 || s_audio_bytes <= 0) {
        // 总时长没解析出来时不允许定位：进度条的刻度本身就是错的，
        // 按它折出来的字节偏移会落到完全无关的位置。
        ESP_LOGW(TAG, "seek: 总时长未知（dur=%u, audio_bytes=%ld），不定位",
                 (unsigned)s_duration_ms, s_audio_bytes);
        return;
    }
    if (target_ms > s_duration_ms) {
        target_ms = s_duration_ms;
    }

    long offset = s_audio_start +
                  (long)((uint64_t)s_audio_bytes * target_ms / s_duration_ms);

    FILE *fp = fopen(s_cur_path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "seek: 打不开 '%s'", s_cur_path);
        return;
    }

    // **必须先对齐到真正的帧头**：上面那个偏移是按比例折出来的，几乎必然落在
    // 某一帧的中间。直接从这里开播的话，libhelix 的 MP3FindSyncWord 会在帧数据
    // 里撞上假同步，MP3Decode 返回 -6 (ERR_MP3_INVALID_FRAMEHEADER) 并一直刷
    // 错误 —— 听感就是"拖到某些位置就静音/卡住"。
    long aligned = mp3_align_to_frame(fp, offset, s_audio_start + s_audio_bytes, 16 * 1024);
    if (aligned < 0) {
        fclose(fp);
        ESP_LOGW(TAG, "seek: 从 %ld 往后 16KB 内找不到帧头，放弃这次定位", offset);
        return;
    }
    fseek(fp, aligned, SEEK_SET);

    esp_err_t ret = audio_player_play(fp);
    if (ret != ESP_OK) {
        // 组件的 audio_player_play() 内部是 xQueueSend(..., 0) ——
        // **零等待**，队列里恰好还压着一个事件时就发不进去（audio_player.cpp:471）。
        // 等几个 tick 再试一次，否则用户这一拖就白费了。
        vTaskDelay(pdMS_TO_TICKS(5));
        ret = audio_player_play(fp);
    }
    if (ret != ESP_OK) {
        // 约定：audio_player_play 返回非 ESP_OK 时 FILE* 的所有权仍在调用方，
        // 必须自己关，否则每拖一次漏一个句柄。
        fclose(fp);
        ESP_LOGE(TAG, "seek: audio_player_play 失败 %s", esp_err_to_name(ret));
        return;
    }

    // 计时重新起算：已播时长 = 定位点 + 之后新写入的 PCM 字节数
    s_pcm_bytes = 0;
    s_base_ms   = target_ms;

    ESP_LOGI(TAG, "seek: %u ms -> %ld 字节（对齐前 %ld，共 %ld；时长 %u ms）",
             (unsigned)target_ms, aligned, offset, s_audio_bytes, (unsigned)s_duration_ms);
}

// 用户此刻是不是正按着进度条。
//
// 用 LVGL 自己的 LV_STATE_PRESSED 判断，而**不是**自己维护一个
// "按下时置 true / 松手时置 false" 的标志位：那种标志位一旦在某个
// 异常路径（PRESS_LOST、indev 被 reset）上没被清掉，就会永远卡在 true，
// 此后定时器再也不更新进度条 —— 表现就是"进度条不动了、也调不动"。
// 状态位由 LVGL 保证，松手/丢焦点都会自动清。
static bool progress_is_dragging(void)
{
    return (progress_slider != NULL) &&
           lv_obj_has_state(progress_slider, LV_STATE_PRESSED);
}

// 进度条事件。
//
// 关键的一条：**只有松手才真正去定位**。拖动过程中 LVGL 会连发几十个
// VALUE_CHANGED，每个都 fopen/fseek/重开解码的话又慢又吵；拖动期间只更新
// 左端那个时间数字，让人看得见自己拖到哪儿了。
static void progress_slider_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *slider = lv_event_get_target(event);

    if (code == LV_EVENT_VALUE_CHANGED) {
        music_time_label_set(lab_time_now,
                             music_percent_to_ms(lv_slider_get_value(slider)));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_duration_ms > 0) {
            music_seek_to_ms(music_percent_to_ms(lv_slider_get_value(slider)));
        }
    }
}

// 每 500ms 把进度画到界面上。
// 之所以要靠定时器而不是"播放器回调"：组件根本不发进度事件，两个时间源
// 又分别在不同任务上，只能由 LVGL 任务定时去取。
static void progress_timer_cb(lv_timer_t *timer)
{
    if (progress_slider == NULL) {
        return;
    }

    if (!progress_is_dragging()) {
        if (s_duration_ms > 0) {
            uint32_t el = music_elapsed_ms();
            if (el > s_duration_ms) {
                el = s_duration_ms;
            }
            lv_slider_set_value(progress_slider,
                                (int32_t)((uint64_t)el * 100 / s_duration_ms),
                                LV_ANIM_OFF);
            music_time_label_set(lab_time_now, el);
        } else {
            // 总时长没解析出来（不是 MP3、文件损坏…）：滑块不动，但已播时间
            // 照样显示 —— 它是从 PCM 字节算出来的，不依赖文件头。
            music_time_label_set(lab_time_now, music_elapsed_ms());
        }
    }

    if (s_duration_ms > 0) {
        music_time_label_set(lab_time_total, s_duration_ms);
    } else {
        lv_label_set_text(lab_time_total, "--:--");
    }
}

// 播放器界面初始化
void music_ui(void)
{
    lvgl_port_lock(0);

    ui_button_style_init();// 初始化按键风格

    /* 创建播放暂停控制按键 */
    btn_play_pause = lv_btn_create(icon_in_obj);
    lv_obj_align(btn_play_pause, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_size(btn_play_pause, 50, 50);
    lv_obj_set_style_radius(btn_play_pause, 25, LV_STATE_DEFAULT);
    lv_obj_add_flag(btn_play_pause, LV_OBJ_FLAG_CHECKABLE);

    lv_obj_add_style(btn_play_pause, &ui_button_styles()->style_focus_no_outline, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(btn_play_pause, &ui_button_styles()->style_focus_no_outline, LV_STATE_FOCUSED);

    label_play_pause = lv_label_create(btn_play_pause);

    lv_label_set_text_static(label_play_pause, LV_SYMBOL_PLAY);
    lv_obj_center(label_play_pause);

    lv_obj_set_user_data(btn_play_pause, (void *) label_play_pause);
    lv_obj_add_event_cb(btn_play_pause, btn_play_pause_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 创建上一首控制按键 */
    lv_obj_t *btn_play_prev = lv_btn_create(icon_in_obj);
    lv_obj_set_size(btn_play_prev, 50, 50);
    lv_obj_set_style_radius(btn_play_prev, 25, LV_STATE_DEFAULT);
    lv_obj_clear_flag(btn_play_prev, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_align_to(btn_play_prev, btn_play_pause, LV_ALIGN_OUT_LEFT_MID, -40, 0); 

    lv_obj_add_style(btn_play_prev, &ui_button_styles()->style_focus_no_outline, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(btn_play_prev, &ui_button_styles()->style_focus_no_outline, LV_STATE_FOCUSED);
    lv_obj_add_style(btn_play_prev, &ui_button_styles()->style_bg, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(btn_play_prev, &ui_button_styles()->style_bg, LV_STATE_FOCUSED);
    lv_obj_add_style(btn_play_prev, &ui_button_styles()->style_bg, LV_STATE_DEFAULT);

    lv_obj_t *label_prev = lv_label_create(btn_play_prev);
    lv_label_set_text_static(label_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(label_prev, &lv_font_montserrat_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_prev, lv_color_make(0, 0, 0), LV_STATE_DEFAULT);
    lv_obj_center(label_prev);
    lv_obj_set_user_data(btn_play_prev, (void *) label_prev);
    lv_obj_add_event_cb(btn_play_prev, btn_prev_next_cb, LV_EVENT_CLICKED, (void *) false);

    /* 创建下一首控制按键 */
    lv_obj_t *btn_play_next = lv_btn_create(icon_in_obj);
    lv_obj_set_size(btn_play_next, 50, 50);
    lv_obj_set_style_radius(btn_play_next, 25, LV_STATE_DEFAULT);
    lv_obj_clear_flag(btn_play_next, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_align_to(btn_play_next, btn_play_pause, LV_ALIGN_OUT_RIGHT_MID, 40, 0);

    lv_obj_add_style(btn_play_next, &ui_button_styles()->style_focus_no_outline, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(btn_play_next, &ui_button_styles()->style_focus_no_outline, LV_STATE_FOCUSED);
    lv_obj_add_style(btn_play_next, &ui_button_styles()->style_bg, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(btn_play_next, &ui_button_styles()->style_bg, LV_STATE_FOCUSED);
    lv_obj_add_style(btn_play_next, &ui_button_styles()->style_bg, LV_STATE_DEFAULT);

    lv_obj_t *label_next = lv_label_create(btn_play_next);
    lv_label_set_text_static(label_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(label_next, &lv_font_montserrat_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_next, lv_color_make(0, 0, 0), LV_STATE_DEFAULT);
    lv_obj_center(label_next);
    lv_obj_set_user_data(btn_play_next, (void *) label_next);
    lv_obj_add_event_cb(btn_play_next, btn_prev_next_cb, LV_EVENT_CLICKED, (void *) true);

    /* 创建声音调节滑动条 —— 竖条，贴在右边缘 */
    //
    // 竖向还是横向**由尺寸决定**，不用额外的 orientation 接口：
    // lv_slider.c 里的 is_slider_horizontal() 就是 `width >= height`。
    // 所以这里给 12×100（高 > 宽）= 竖条，且数值向上增大。
    volume_slider = lv_slider_create(icon_in_obj);
    lv_obj_set_size(volume_slider, 12, 100);
    // 点击扩展区只能给 10：再大一点，左边就会伸进"下一首"按键的点击区
    // （它右边缘在 x=275），而本滑块建得更晚、z 序在上 —— 点在按键右下角
    // 会变成调音量。往中间收的同时必须把这个扩张一起收窄。
    lv_obj_set_ext_click_area(volume_slider, 10);
    lv_obj_align(volume_slider, LV_ALIGN_TOP_RIGHT, -20, 66);   // 离右边缘 20px，不贴边
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, g_sys_volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(volume_slider, volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *lab_vol_max = lv_label_create(icon_in_obj);
    lv_label_set_text_static(lab_vol_max, LV_SYMBOL_VOLUME_MAX);
    lv_obj_clear_flag(lab_vol_max, LV_OBJ_FLAG_CLICKABLE);   // 别挡着滑块的扩展点击区
    lv_obj_set_style_text_font(lab_vol_max, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    // 上=大、下=小。**先摆好滑块再 align_to** —— align_to 是拿调用那一刻的
    // 基准几何算绝对坐标，基准还没定尺寸的话位置就是错的。
    lv_obj_align_to(lab_vol_max, volume_slider, LV_ALIGN_OUT_TOP_MID, 0, -4);

    lv_obj_t *lab_vol_min = lv_label_create(icon_in_obj);
    lv_label_set_text_static(lab_vol_min, LV_SYMBOL_VOLUME_MID);
    lv_obj_clear_flag(lab_vol_min, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(lab_vol_min, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_align_to(lab_vol_min, volume_slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    /* 创建播放进度条 —— 屏幕底部，可拖动定位 */
    //
    // 取值范围就用 0..100 百分比：定位时再换算成毫秒和字节偏移，
    // 这样总时长解析失败（滑块根本没刻度）时也不会影响用户在音量上的操作。
    progress_slider = lv_slider_create(icon_in_obj);
    lv_obj_set_size(progress_slider, 240, 10);        // 往中间收：两端各留出约 40px
    lv_obj_set_ext_click_area(progress_slider, 10);   // 放大到 10 是刻意的：再大就会
                                                      // 盖到上面那排按键的点击区
                                                      // （它们底边在 y=185）
    lv_obj_align(progress_slider, LV_ALIGN_BOTTOM_MID, -10, -26);
    lv_slider_set_range(progress_slider, 0, 100);
    lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
    lv_obj_add_event_cb(progress_slider, progress_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(progress_slider, progress_slider_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(progress_slider, progress_slider_cb, LV_EVENT_PRESS_LOST, NULL);

    // 两个时间标签**贴着进度条的两端**（而不是各自贴屏幕边）—— 这样以后
    // 再调进度条宽度/位置，标签自动跟着走，不会各飞各的。
    //
    // 两个标签都必须**关掉 CLICKABLE**：标签默认是可点的，而它们正好压在
    // 进度条向下扩展出来的点击区里（进度条 y204..214 + ext10 = 194..224，
    // 标签在 216..233）。不关的话，从条的下边缘起手拖动会被标签吃掉点击，
    // 表现为"拖不动"。
    lab_time_now = lv_label_create(icon_in_obj);
    lv_obj_clear_flag(lab_time_now, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(lab_time_now, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lab_time_now, lv_color_hex(0x555555), LV_STATE_DEFAULT);
    lv_obj_align_to(lab_time_now, progress_slider, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);
    lv_label_set_text(lab_time_now, "0:00");

    lab_time_total = lv_label_create(icon_in_obj);
    lv_obj_clear_flag(lab_time_total, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(lab_time_total, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lab_time_total, lv_color_hex(0x555555), LV_STATE_DEFAULT);
    lv_obj_align_to(lab_time_total, progress_slider, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);
    lv_label_set_text(lab_time_total, "--:--");

    // 进度刷新定时器。跨页面只建一个；销毁在 btn_music_back_cb 里。
    if (s_progress_timer == NULL) {
        s_progress_timer = lv_timer_create(progress_timer_cb, 500, NULL);
    }

    /* 创建音乐列表 */ 
    music_list = lv_dropdown_create(icon_in_obj);
    lv_dropdown_clear_options(music_list);
    lv_dropdown_set_options_static(music_list, "扫描中...");
    // selector 必须写 0（= LV_PART_MAIN + 默认状态）。
    // 原来写的是 LV_STATE_ANY（0xFFFF）—— 那是"读样式"用的常量，当 set 的
    // selector 属于无效用法，字体设了等于没设，下拉框里的中文于是全是方框。
    lv_obj_set_style_text_font(music_list, &font_alipuhui20, 0);
    lv_obj_set_width(music_list, 200);
    lv_obj_align(music_list, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_add_event_cb(music_list, music_list_cb, LV_EVENT_VALUE_CHANGED, NULL);
    // 点开下拉框时，LVGL 会先把弹层建好（lv_dropdown_open），然后才调用户回调。
    // 借这个时机把弹层的字体设上，保证**第一次**展开就是中文字体。
    lv_obj_add_event_cb(music_list, music_list_open_cb, LV_EVENT_CLICKED, NULL);

    build_file_list(music_list);

    lvgl_port_unlock();
}

// 由 ui_router 在 LVGL 任务上调用（锁已持有），把下拉框同步到当前曲目。
// 见 ui_music.h 与本文件 _audio_player_callback 里的说明。
void ui_music_set_current_track(int index)
{
    // 页面已经关了。这条消息有可能是在页面被删之后才被抽干的 ——
    // 抽干周期 30ms，退出与"某首歌恰好放完"撞在一起就会这样。
    // music_list 在 btn_music_back_cb 里已被置成 NULL。
    if (music_list == NULL || index < 0) {
        return;
    }
    lv_dropdown_set_selected(music_list, (uint16_t)index);
}

// 返回主界面按钮事件处理函数。错误页也用这一个 —— 那时
// s_player_created 是 false，下面第一段自然跳过。
static void btn_music_back_cb(lv_event_t * e)
{
    // **顺序是关键：先停播放器，再拆界面。**
    //
    // 原来反着来：先 lv_obj_del(icon_in_obj)，再 audio_player_delete()。
    // 界面一删，music_list 就成野指针，而 audio task 的回调随时可能拿它
    // 去 lv_dropdown_set_selected()；更要命的是那会儿 audio task 可能正卡在
    // lvgl_port_lock() 上等锁，而本任务正持着锁等 audio_player_delete() 返回
    // —— 死锁 500ms 后组件强行释放 audio task 还在用的内存。
    // 本次已把回调里那句 LVGL 调用搬去消息总线（死锁的成因没有了），
    // 但"先停任务、再拆它引用的数据结构"这个顺序仍然必须守住。
    if (s_player_created) {
        s_music_alive = false;   // 先让回调闭嘴，别再发"下一首"
        audio_player_delete();
        s_player_created = false;
    }

    pa_en(0);

    // 界面句柄全部作废。它们指向的对象马上就被删掉，留任何一个都是野指针
    // —— 包括已经进了 ui_msg 队列、还没被抽干的那条 UI_MSG_MUSIC_TRACK_CHANGED。
    // ui_music_set_current_track() 正是靠 music_list 是不是 NULL 判断页面还在不在。
    s_music_alive = false;
    music_list = NULL;
    label_play_pause = NULL;
    btn_play_pause = NULL;
    volume_slider = NULL;
    music_title_label = NULL;
    btn_music_back = NULL;

    // 进度条那一组：定时器必须先删，否则它下一次触发时 progress_slider 已经
    // 随 icon_in_obj 一起被销毁，回调里 lv_slider_set_value 就是往已释放的
    // 对象里写（use-after-free）。
    if (s_progress_timer != NULL) {
        lv_timer_del(s_progress_timer);
        s_progress_timer = NULL;
    }
    progress_slider = NULL;
    lab_time_now = NULL;
    lab_time_total = NULL;
    s_cur_path[0] = '\0';
    s_duration_ms = 0;
    s_audio_start = 0;
    s_audio_bytes = 0;
    s_base_ms = 0;

    lv_obj_del(icon_in_obj);
    icon_in_obj = NULL;
    icon_flag = 0;

    // 本应用挂上的 SD 卡，退出时归还（谁挂的谁卸）。
    if (s_sd_mounted_by_music) {
        bsp_sdcard_unmount();
        s_sd_mounted_by_music = false;
    }
}

// 建画布 + 标题栏 + 返回按钮。从原 music_event_handler 里整段搬出来，
// 只为让错误页能复用（标题、配色、返回键逐字未改）。
static void music_build_frame(void)
{
    // 创建一个界面对象
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_radius(&style, 10);
    lv_style_set_bg_opa( &style, LV_OPA_COVER );
    lv_style_set_bg_color(&style, lv_color_hex(0xffffff));
    lv_style_set_border_width(&style, 0);
    lv_style_set_pad_all(&style, 0);
    lv_style_set_width(&style, 320);
    lv_style_set_height(&style, 240);

    icon_in_obj = lv_obj_create(lv_scr_act());
    lv_obj_add_style(icon_in_obj, &style, 0);

    // 创建标题背景
    lv_obj_t *music_title = lv_obj_create(icon_in_obj);
    lv_obj_set_size(music_title, 320, 40);
    lv_obj_set_style_pad_all(music_title, 0, 0);  // 设置间隙
    lv_obj_align(music_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(music_title, lv_color_hex(0xf87c30), 0);
    // 显示标题
    music_title_label = lv_label_create(music_title);
    lv_label_set_text(music_title_label, "音乐播放器");
    lv_obj_set_style_text_color(music_title_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(music_title_label, &font_alipuhui20, 0);
    lv_obj_align(music_title_label, LV_ALIGN_CENTER, 0, 0);
    // 创建后退按钮
    btn_music_back = lv_btn_create(music_title);
    lv_obj_align(btn_music_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_size(btn_music_back, 60, 30);
    lv_obj_set_style_border_width(btn_music_back, 0, 0); // 设置边框宽度
    lv_obj_set_style_pad_all(btn_music_back, 0, 0);  // 设置间隙
    lv_obj_set_style_bg_opa(btn_music_back, LV_OPA_TRANSP, LV_PART_MAIN); // 背景透明
    lv_obj_set_style_shadow_opa(btn_music_back, LV_OPA_TRANSP, LV_PART_MAIN); // 阴影透明
    lv_obj_add_event_cb(btn_music_back, btn_music_back_cb, LV_EVENT_CLICKED, NULL); // 添加按键处理函数

    lv_obj_t *label_back = lv_label_create(btn_music_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);  // 按键上显示左箭头符号
    lv_obj_set_style_text_font(label_back, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_back, lv_color_hex(0xffffff), 0);
    lv_obj_align(label_back, LV_ALIGN_CENTER, -10, 0);

    icon_flag = 2; // 标记已经进入第二个应用
}

// 音乐源不可用时的错误页：标题栏（里面已经有返回按钮）下面加一行说明。
static void music_show_error(void)
{
    lv_obj_t *label = lv_label_create(icon_in_obj);
    lv_label_set_text(label, (s_error_text != NULL) ? s_error_text : "音乐播放器初始化失败");
    lv_obj_set_style_text_font(label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, 300);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 80);
}

// 进入音乐播放应用
void music_event_handler(lv_event_t * e)
{
    music_build_frame();

    // 初始化mp3播放器
    if (!music_source_prepare() || !mp3_player_init()) {
        music_show_error();
        return;
    }
    s_music_alive = true;

    music_ui(); // 音乐播放器界面
}



