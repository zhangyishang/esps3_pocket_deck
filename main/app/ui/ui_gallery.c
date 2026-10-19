#include "ui_gallery.h"
#include "ui_common.h"
#include "gallery_task.h"
#include "ui_msg.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

/** 列表每行高度与可见行数。 */
#define GALLERY_ROW_H     40
#define GALLERY_ROW_NUM   5

/** 当前正在显示的全屏图（含它占的 PSRAM 缓冲）。 */
static gallery_frame_t *s_disp = NULL;

/** 列表容器与标题（页面重建时跟着换）。 */
static lv_obj_t *s_list_box = NULL;
static lv_obj_t *s_title    = NULL;

/** 全屏图层。 */
static lv_obj_t *s_img      = NULL;
static lv_img_dsc_t s_img_dsc;

/**
 * @brief 把字节数格式化成 "123 KB" / "1.2 MB"。
 * @param buf  输出缓冲
 * @param cap  容量
 * @param size 字节数
 */
static void fmt_size(char *buf, size_t cap, uint32_t size)
{
    if (size >= 1024 * 1024) {
        snprintf(buf, cap, "%.1f MB", size / 1024.0f / 1024.0f);
    } else {
        snprintf(buf, cap, "%u KB", (unsigned)(size / 1024 + 1));
    }
}

/**
 * @brief 关闭全屏查看：删掉图层并释放像素缓冲，回到列表。
 */
static void close_view(void)
{
    if (s_img != NULL) {
        lv_obj_del(s_img);
        s_img = NULL;
    }
    if (s_disp != NULL) {
        if (s_disp->pixels != NULL) {
            heap_caps_free(s_disp->pixels);
        }
        heap_caps_free(s_disp);
        s_disp = NULL;
    }
}

/**
 * @brief 点击全屏图：回到列表。
 * @param e LVGL 事件（未使用）
 */
static void view_cb(lv_event_t *e)
{
    close_view();
}

/**
 * @brief 列表项点击：打开对应图片。
 *        下标记在 user_data 里，**存的是 index + 1** —— 因为 0 会和 NULL 撞车。
 * @param e LVGL 事件
 */
static void item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e) - 1;
    if (idx >= 0) {
        ui_gallery_open(idx);
    }
}

/**
 * @brief 返回按钮：正在看大图时先退回列表；已经在列表里才退出应用。
 * @param e LVGL 事件（未使用）
 */
static void back_cb(lv_event_t *e)
{
    if (s_img != NULL) {
        close_view();
        return;
    }
    close_view();
    icon_flag = 0;
    if (icon_in_obj != NULL) {
        lv_obj_del(icon_in_obj);
        icon_in_obj = NULL;
    }
    s_list_box = NULL;
    s_title = NULL;
}

/**
 * @brief 图库应用入口：建页面并开始扫描。
 * @param e LVGL 事件（未使用）
 */
void gallery_event_handler(lv_event_t *e)
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

    // 返回按钮
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
    s_title = lv_label_create(icon_in_obj);
    lv_obj_set_style_text_font(s_title, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xffffff), 0);
    lv_label_set_text(s_title, "正在扫描…");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 8);

    // 可滚动的列表容器
    s_list_box = lv_obj_create(icon_in_obj);
    lv_obj_set_size(s_list_box, 300, GALLERY_ROW_H * GALLERY_ROW_NUM);
    lv_obj_align(s_list_box, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(s_list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_box, 0, 0);
    lv_obj_set_style_pad_all(s_list_box, 0, 0);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);

    s_disp = NULL;
    s_img = NULL;

    icon_flag = SCREEN_GALLERY;
    ui_gallery_task_start();
}

/**
 * @brief 显示扫描结果，填充列表。
 * @param list 文件列表（函数接管并释放）
 */
void ui_gallery_show_list(gallery_list_t *list)
{
    if (list == NULL) {
        return;
    }
    if (s_list_box == NULL || s_title == NULL) {
        heap_caps_free(list);
        return;
    }

    lv_obj_clean(s_list_box);

    char head[32];
    snprintf(head, sizeof(head), "图库 · 共 %d 张", list->total);
    lv_label_set_text(s_title, head);

    for (int i = 0; i < list->count; i++) {
        lv_obj_t *row = lv_btn_create(s_list_box);
        lv_obj_set_size(row, 292, GALLERY_ROW_H - 4);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1f2b38), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_add_event_cb(row, item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_style_text_font(name, &font_alipuhui20, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0xffffff), 0);
        lv_label_set_text(name, list->items[i].name);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        char sz[16];
        fmt_size(sz, sizeof(sz), list->items[i].size);
        lv_obj_t *tail = lv_label_create(row);
        lv_obj_set_style_text_font(tail, &font_alipuhui20, 0);
        lv_obj_set_style_text_color(tail, lv_color_hex(0x8fa6bb), 0);
        lv_label_set_text(tail, sz);
        lv_obj_align(tail, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    heap_caps_free(list);
}

/**
 * @brief 全屏显示一张图。
 * @param frame 解码好的图（函数接管并释放）
 */
void ui_gallery_show_frame(gallery_frame_t *frame)
{
    if (frame == NULL || frame->pixels == NULL) {
        return;
    }
    if (icon_in_obj == NULL) {
        heap_caps_free(frame->pixels);
        heap_caps_free(frame);
        return;
    }

    // 让出旧图
    close_view();

    s_disp = frame;
    s_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_img_dsc.header.always_zero = 0;
    s_img_dsc.header.reserved = 0;
    s_img_dsc.header.w = frame->w;
    s_img_dsc.header.h = frame->h;
    s_img_dsc.data_size = (uint32_t)frame->w * frame->h * 2;
    s_img_dsc.data = frame->pixels;

    s_img = lv_img_create(icon_in_obj);
    lv_img_set_src(s_img, &s_img_dsc);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(s_img);
    // 点一下大图 = 回到列表
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_img, view_cb, LV_EVENT_CLICKED, NULL);
}

/**
 * @brief 显示失败提示。
 * @param code gallery_result_t
 */
void ui_gallery_show_failed(int code)
{
    const char *text;
    switch (code) {
    case GALLERY_NO_SD:       text = "没有 SD 卡"; break;
    case GALLERY_EMPTY:       text = "还没有图片"; break;
    case GALLERY_NO_MEM:      text = "内存不足";   break;
    case GALLERY_DECODE_FAIL: text = "图片打不开"; break;
    default:                  text = "出错了";     break;
    }

    if (s_title != NULL) {
        lv_label_set_text(s_title, text);
    }
}

/**
 * @brief 删除图库画布并释放显示缓冲。
 */
void ui_gallery_close_canvas(void)
{
    lvgl_port_lock(0);
    close_view();
    if (icon_in_obj != NULL) {
        lv_obj_del(icon_in_obj);
        icon_in_obj = NULL;
    }
    s_list_box = NULL;
    s_title = NULL;
    lvgl_port_unlock();
}
