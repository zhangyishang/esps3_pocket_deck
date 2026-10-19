#pragma once

#include "lvgl.h"

/**
 * @brief 网页文件管理应用入口（主界面图标点击时调用）。
 * @param e LVGL 事件（未使用）
 */
void web_event_handler(lv_event_t *e);

/**
 * @brief 更新服务状态显示（由 ui_router 在收到 UI_MSG_WEB_STATE 时调用）。
 * @param is_start 1 启动 / 0 停止
 * @param err      esp_err_t
 * @param ip       设备 IP 的点分十进制文本（未联网时为空串）
 */
void ui_web_show_state(int is_start, int err, const char *ip);

/**
 * @brief 删除网页应用画布。
 */
void ui_web_close_canvas(void);
