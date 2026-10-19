#include "ui_xiaozhi.h"
#include "ui_common.h"
#include "xiaozhi_task.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <string.h>

/** 页面上的控件。 */
static lv_obj_t *s_state_label = NULL;   /* 顶部状态 */
static lv_obj_t *s_stt_label   = NULL;   /* 用户说的话 */
static lv_obj_t *s_reply_label = NULL;   /* 助手回复 */
static lv_obj_t *s_talk_btn    = NULL;   /* 按住说话 */
static lv_obj_t *s_vol_label   = NULL;   /* 音量数字 */
static int        s_volume     = 50;     /* 当前音量 0~100，默认 50 */

/**
 * @brief 把状态码翻译成中文。
 * @param st xz_state_t
 * @return 文案
 */
static const char *state_text(int st)
{
    switch (st) {
    case XZ_STATE_CONNECTING: return "连接中…";
    case XZ_STATE_READY:      return "按住说话";
    case XZ_STATE_RECORDING:  return "录音中…松开结束";
    case XZ_STATE_THINKING:   return "小智正在思考…";
    case XZ_STATE_SPEAKING:   return "小智正在说话…";
    default:                  return "未连接";
    }
}

/**
 * @brief 刷新状态文字与按钮颜色。
 * @param st xz_state_t
 */
static void refresh_state(int st)
{
    if (s_state_label != NULL) {
        lv_label_set_text(s_state_label, state_text(st));
    }
    if (s_talk_btn != NULL) {
        uint32_t color = (st == XZ_STATE_RECORDING) ? 0xd8534f
                       : (st == XZ_STATE_READY)     ? 0x2c6fb5
                                                    : 0x3a4756;
        lv_obj_set_style_bg_color(s_talk_btn, lv_color_hex(color), 0);
    }
}

/**
 * @brief 返回按钮：退出小智并断开连接。
 * @param e LVGL 事件（未使用）
 */
static void back_cb(lv_event_t *e)
{
    ui_xiaozhi_task_stop();
    icon_flag = 0;
    if (icon_in_obj != NULL) {
        lv_obj_del(icon_in_obj);
        icon_in_obj = NULL;
    }
    s_state_label = NULL;
    s_stt_label = NULL;
    s_reply_label = NULL;
    s_talk_btn = NULL;
    s_vol_label = NULL;
}

/**
 * @brief 按住：开始录音。
 * @param e LVGL 事件（未使用）
 */
static void talk_pressed_cb(lv_event_t *e)
{
    ui_xiaozhi_record_begin();
}

/**
 * @brief 松开：结束录音。
 * @param e LVGL 事件（未使用）
 */
static void talk_released_cb(lv_event_t *e)
{
    ui_xiaozhi_record_end();
}

/**
 * @brief 把当前音量应用到 codec，并刷新数字显示。
 */
static void apply_volume(void)
{
    ui_xiaozhi_set_volume(s_volume);
    if (s_vol_label != NULL) {
        lv_label_set_text_fmt(s_vol_label, "%d", s_volume);
    }
}

/**
 * @brief 音量减 10（LVGL 回调）。
 * @param e LVGL 事件（未使用）
 */
static void vol_minus_cb(lv_event_t *e)
{
    (void)e;
    if (s_volume > 0) {
        s_volume = (s_volume >= 10) ? (s_volume - 10) : 0;
        apply_volume();
    }
}

/**
 * @brief 音量加 10（LVGL 回调）。
 * @param e LVGL 事件（未使用）
 */
static void vol_plus_cb(lv_event_t *e)
{
    (void)e;
    if (s_volume < 100) {
        s_volume = (s_volume <= 90) ? (s_volume + 10) : 100;
        apply_volume();
    }
}

/**
 * @brief 小智应用入口：建页面并开始连接。
 * @param e LVGL 事件（未使用）
 */
void xiaozhi_event_handler(lv_event_t *e)
{
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_radius(&style, 10);
    lv_style_set_bg_opa(&style, LV_OPA_COVER);
    lv_style_set_bg_color(&style, lv_color_hex(0x101820));
    lv_style_set_border_width(&style, 0);
    lv_style_set_pad_all(&style, 0);
    lv_style_set_width(&style, 320);
    lv_style_set_height(&style, 240);

    icon_in_obj = lv_obj_create(lv_scr_act());
    lv_obj_add_style(icon_in_obj, &style, 0);

    // 返回
    lv_obj_t *btn_back = lv_btn_create(icon_in_obj);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_size(btn_back, 44, 30);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_set_style_pad_all(btn_back, 0, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lb_back = lv_label_create(btn_back);
    lv_label_set_text(lb_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lb_back, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lb_back, lv_color_hex(0xffffff), 0);
    lv_obj_align(lb_back, LV_ALIGN_CENTER, 0, 0);

    // 标题
    lv_obj_t *title = lv_label_create(icon_in_obj);
    lv_obj_set_style_text_font(title, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_label_set_text(title, "小智");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* ===== 音量控制：右上角 [- ] 50 [ +] ===== */
    {
        lv_obj_t *btn_minus = lv_btn_create(icon_in_obj);
        lv_obj_set_size(btn_minus, 28, 26);
        lv_obj_align(btn_minus, LV_ALIGN_TOP_RIGHT, -76, 6);
        lv_obj_set_style_radius(btn_minus, 6, 0);
        lv_obj_set_style_bg_color(btn_minus, lv_color_hex(0x3a4756), 0);
        lv_obj_set_style_border_width(btn_minus, 0, 0);
        lv_obj_set_style_pad_all(btn_minus, 0, 0);
        lv_obj_add_event_cb(btn_minus, vol_minus_cb, LV_EVENT_CLICKED, NULL);
        {
            lv_obj_t *lb = lv_label_create(btn_minus);
            lv_label_set_text(lb, LV_SYMBOL_MINUS);
            lv_obj_set_style_text_color(lb, lv_color_hex(0xffffff), 0);
            lv_obj_align(lb, LV_ALIGN_CENTER, 0, 0);
        }

        s_vol_label = lv_label_create(icon_in_obj);
        lv_obj_set_style_text_font(s_vol_label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_vol_label, lv_color_hex(0x7fd0ff), 0);
        lv_label_set_text_fmt(s_vol_label, "%d", s_volume);
        lv_obj_align(s_vol_label, LV_ALIGN_TOP_RIGHT, -42, 8);
        lv_obj_set_width(s_vol_label, 30);
        lv_obj_set_style_text_align(s_vol_label, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *btn_plus = lv_btn_create(icon_in_obj);
        lv_obj_set_size(btn_plus, 28, 26);
        lv_obj_align(btn_plus, LV_ALIGN_TOP_RIGHT, -6, 6);
        lv_obj_set_style_radius(btn_plus, 6, 0);
        lv_obj_set_style_bg_color(btn_plus, lv_color_hex(0x3a4756), 0);
        lv_obj_set_style_border_width(btn_plus, 0, 0);
        lv_obj_set_style_pad_all(btn_plus, 0, 0);
        lv_obj_add_event_cb(btn_plus, vol_plus_cb, LV_EVENT_CLICKED, NULL);
        {
            lv_obj_t *lb = lv_label_create(btn_plus);
            lv_label_set_text(lb, LV_SYMBOL_PLUS);
            lv_obj_set_style_text_color(lb, lv_color_hex(0xffffff), 0);
            lv_obj_align(lb, LV_ALIGN_CENTER, 0, 0);
        }
    }

    // 用户说的话
    s_stt_label = lv_label_create(icon_in_obj);
    lv_obj_set_style_text_font(s_stt_label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_stt_label, lv_color_hex(0x7fd0ff), 0);
    lv_obj_set_width(s_stt_label, 280);
    lv_obj_align(s_stt_label, LV_ALIGN_TOP_LEFT, 16, 46);
    lv_label_set_text(s_stt_label, "");

    // 助手回复
    s_reply_label = lv_label_create(icon_in_obj);
    lv_obj_set_style_text_font(s_reply_label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_reply_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_width(s_reply_label, 280);
    lv_obj_align(s_reply_label, LV_ALIGN_TOP_LEFT, 16, 86);
    lv_label_set_text(s_reply_label, "");

    // 按住说话按钮
    s_talk_btn = lv_btn_create(icon_in_obj);
    lv_obj_set_size(s_talk_btn, 200, 48);
    lv_obj_align(s_talk_btn, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_radius(s_talk_btn, 24, 0);
    lv_obj_set_style_bg_color(s_talk_btn, lv_color_hex(0x3a4756), 0);
    lv_obj_set_style_border_width(s_talk_btn, 0, 0);
    lv_obj_add_event_cb(s_talk_btn, talk_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_talk_btn, talk_released_cb, LV_EVENT_RELEASED, NULL);

    s_state_label = lv_label_create(s_talk_btn);
    lv_obj_set_style_text_font(s_state_label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0xffffff), 0);
    lv_label_set_text(s_state_label, "连接中…");
    lv_obj_align(s_state_label, LV_ALIGN_CENTER, 0, 0);

    icon_flag = SCREEN_XIAOZHI;
    ui_xiaozhi_task_start();
}

/**
 * @brief 更新状态显示。
 * @param st xz_state_t
 */
void ui_xiaozhi_show_state(int st)
{
    refresh_state(st);
}

/**
 * @brief 显示一行字幕。
 * @param is_user true = 用户说的
 * @param text    文本
 */
void ui_xiaozhi_show_text(bool is_user, const char *text)
{
    if (text == NULL) {
        return;
    }
    if (is_user) {
        if (s_stt_label != NULL) {
            lv_label_set_text(s_stt_label, text);
        }
    } else {
        if (s_reply_label != NULL) {
            lv_label_set_text(s_reply_label, text);
        }
    }
}

/**
 * @brief 显示错误。
 * @param code xz_error_t
 */
void ui_xiaozhi_show_error(int code)
{
    const char *msg;
    switch (code) {
    case XZ_ERR_NO_WIFI:  msg = "请先连接 WiFi"; break;
    case XZ_ERR_NETWORK:  msg = "连不上服务端 / 激活超时"; break;
    case XZ_ERR_OPUS:     msg = "音频编码器初始化失败"; break;
    case XZ_ERR_NO_MEM:   msg = "内存不足"; break;
    default:              msg = "出错了"; break;
    }
    if (s_stt_label != NULL) {
        lv_label_set_text(s_stt_label, msg);
    }
}

/**
 * @brief 显示官方云要求的设备激活码。
 *        用户需要到 xiaozhi.me 输入这个 6 位码把设备绑定到账号下。
 * @param code 激活码
 */
void ui_xiaozhi_show_activation(const char *code)
{
    if (code == NULL || code[0] == '\0') {
        return;
    }
    /* 验证码要占掉整个上半屏：清掉右边那行诊断，把码放大显示。 */
    if (s_reply_label != NULL) {
        lv_label_set_text(s_reply_label, "");
    }
    if (s_stt_label != NULL) {
        lv_obj_set_style_text_font(s_stt_label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_stt_label, lv_color_hex(0xffd24a), 0);
        lv_label_set_text_fmt(s_stt_label, "验证码  %s\n到 xiaozhi.me 输入", code);
    }
    if (s_state_label != NULL) {
        lv_label_set_text(s_state_label, "等待激活…");
    }
}

/**
 * @brief 显示一行诊断文字（不接串口也能看到卡在哪一步）。
 * @param text 诊断内容
 */
void ui_xiaozhi_show_debug(const char *text)
{
    if (text == NULL || s_reply_label == NULL) {
        return;
    }
    lv_label_set_text(s_reply_label, text);
}

/**
 * @brief 删除小智页面。
 */
void ui_xiaozhi_close_canvas(void)
{
    lvgl_port_lock(0);
    if (icon_in_obj != NULL) {
        lv_obj_del(icon_in_obj);
        icon_in_obj = NULL;
    }
    s_state_label = NULL;
    s_stt_label = NULL;
    s_reply_label = NULL;
    s_talk_btn = NULL;
    lvgl_port_unlock();
}
