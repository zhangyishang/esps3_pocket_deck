#pragma once

#include "lvgl.h"
#include "ui_msg.h"

/**
 * @brief 图库应用入口（主界面图标点击时调用）：建页面并开始扫描 SD 卡。
 * @param e LVGL 事件（未使用）
 */
void gallery_event_handler(lv_event_t *e);

/**
 * @brief 显示扫描结果（由 ui_router 在收到 UI_MSG_GALLERY_LIST 时调用，
 *        该函数会接管并释放 list）。
 * @param list 文件列表
 */
void ui_gallery_show_list(gallery_list_t *list);

/**
 * @brief 全屏显示一张图（由 ui_router 在收到 UI_MSG_GALLERY_FRAME 时调用，
 *        该函数会接管并释放 frame）。
 * @param frame 解码好的图
 */
void ui_gallery_show_frame(gallery_frame_t *frame);

/**
 * @brief 显示失败提示（由 ui_router 在收到 UI_MSG_GALLERY_FAILED 时调用）。
 * @param code gallery_result_t
 */
void ui_gallery_show_failed(int code);

/**
 * @brief 删除图库画布并释放显示缓冲（由 ui_router 在收到 UI_MSG_GALLERY_CLOSE 时调用）。
 */
void ui_gallery_close_canvas(void);
