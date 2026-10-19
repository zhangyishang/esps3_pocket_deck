#pragma once

#include "lvgl.h"
#include <stdbool.h>

/**
 * @brief 构建主界面（两页可横滑 + 图标 + 时间日期 + 状态符号）。
 */
void lv_main_page(void);

/**
 * @brief 刷新右上角的 WiFi 状态符号：连上才显示，断开就隐藏。
 *        由 ui_wifi.c 的每秒定时器调用（运行在 LVGL 任务上，无需再持锁）。
 * @param on true = 已联网
 */
void ui_main_page_set_wifi_connected(bool on);
