#include "ui_web.h"
#include "ui_common.h"
#include "web_task.h"
#include "web_server.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

/** 页面上的两个文本标签。 */
static lv_obj_t *s_url_label  = NULL;
static lv_obj_t *s_tip_label  = NULL;
static lv_obj_t *s_btn_toggle = NULL;
static lv_obj_t *s_btn_text   = NULL;

/** 当前服务是否在运行（本页面的本地状态）。 */
static bool s_running = false;

/**
 * @brief 刷新按钮文字。
 */
static void refresh_button(void)
{
    if (s_btn_text != NULL) {
        lv_label_set_text(s_btn_text, s_running ? "停止服务" : "启动服务");
    }
}

/**
 * @brief 切换按钮：启动或停止网页服务。
 * @param e LVGL 事件（未使用）
 */
static void toggle_cb(lv_event_t *e)
{
    if (s_running) {
        ui_web_task_stop();
    } else {
        ui_web_task_start();
    }
}

/**
 * @brief 返回按钮：退出应用（顺手停服务，并自己删画布，否则会泄漏）。
 * @param e LVGL 事件（未使用）
 */
static void back_cb(lv_event_t *e)
{
    if (web_server_is_running()) {
        ui_web_task_stop();
    }
    icon_flag = 0;
    if (icon_in_obj != NULL) {
        lv_obj_del(icon_in_obj);
        icon_in_obj = NULL;
    }
    s_url_label = NULL;
    s_tip_label = NULL;
    s_btn_toggle = NULL;
    s_btn_text = NULL;
}

/**
 * @brief 网页文件管理应用入口。
 * @param e LVGL 事件（未使用）
 */
void web_event_handler(lv_event_t *e)
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
    lv_label_set_text(title, "网页文件管理");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // 访问地址（大字）
    s_url_label = lv_label_create(icon_in_obj);
    lv_obj_set_style_text_font(s_url_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_url_label, lv_color_hex(0x7fd0ff), 0);
    lv_label_set_text(s_url_label, "http://---");
    lv_obj_align(s_url_label, LV_ALIGN_TOP_MID, 0, 52);

    // 提示
    s_tip_label = lv_label_create(icon_in_obj);
    lv_obj_set_style_text_font(s_tip_label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_tip_label, lv_color_hex(0x8fa6bb), 0);
    lv_obj_set_style_text_align(s_tip_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_tip_label, 280);
    lv_label_set_text(s_tip_label, "手机连同一个 WiFi 后\n打开上面地址即可管理 SD 卡");
    lv_obj_align(s_tip_label, LV_ALIGN_TOP_MID, 0, 88);

    // 启停按钮
    s_btn_toggle = lv_btn_create(icon_in_obj);
    lv_obj_set_size(s_btn_toggle, 140, 44);
    lv_obj_align(s_btn_toggle, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(s_btn_toggle, 10, 0);
    lv_obj_set_style_bg_color(s_btn_toggle, lv_color_hex(0x2c6fb5), 0);
    lv_obj_set_style_border_width(s_btn_toggle, 0, 0);
    lv_obj_add_event_cb(s_btn_toggle, toggle_cb, LV_EVENT_CLICKED, NULL);

    s_btn_text = lv_label_create(s_btn_toggle);
    lv_obj_set_style_text_font(s_btn_text, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_btn_text, lv_color_hex(0xffffff), 0);
    lv_label_set_text(s_btn_text, "启动服务");
    lv_obj_align(s_btn_text, LV_ALIGN_CENTER, 0, 0);

    s_running = web_server_is_running();
    refresh_button();

    icon_flag = SCREEN_WEB;
}

/**
 * @brief 更新状态显示。
 * @param is_start 1 启动 / 0 停止
 * @param err      esp_err_t
 * @param ip       设备 IP（点分十进制文本，未联网时为空串）
 */
void ui_web_show_state(int is_start, int err, const char *ip)
{
    if (is_start) {
        if (err == 0) {
            s_running = true;
            char url[40];
            strlcpy(url, "http://", sizeof(url));
            strlcat(url, (ip != NULL && ip[0] != '\0') ? ip : "---", sizeof(url));
            if (s_url_label != NULL) {
                lv_label_set_text(s_url_label, url);
            }
            if (s_tip_label != NULL) {
                lv_label_set_text(s_tip_label, "手机连同一个 WiFi 后\n打开上面地址即可管理 SD 卡");
            }
        } else {
            s_running = false;
            if (s_url_label != NULL) {
                lv_label_set_text(s_url_label, "启动失败");
            }
            if (s_tip_label != NULL) {
                lv_label_set_text(s_tip_label, err == ESP_ERR_INVALID_STATE
                                  ? "还没连上 WiFi，请先进 WiFi 应用连接"
                                  : "服务启动失败");
            }
        }
    } else {
        s_running = false;
        if (s_url_label != NULL) {
            lv_label_set_text(s_url_label, "http://---");
        }
        if (s_tip_label != NULL) {
            lv_label_set_text(s_tip_label, "服务已停止");
        }
    }
    refresh_button();
}

/**
 * @brief 删除网页应用画布。
 */
void ui_web_close_canvas(void)
{
    lvgl_port_lock(0);
    if (icon_in_obj != NULL) {
        lv_obj_del(icon_in_obj);
        icon_in_obj = NULL;
    }
    s_url_label = NULL;
    s_tip_label = NULL;
    s_btn_toggle = NULL;
    s_btn_text = NULL;
    lvgl_port_unlock();
}
