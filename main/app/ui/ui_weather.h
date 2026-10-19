#pragma once

#include "lvgl.h"

// 天气应用的入口（主界面图标点进来）。
void weather_event_handler(lv_event_t * e);

void ui_weather_show(const void *data);

// 抓取失败：按结果码显示对应提示（由 ui_router 在 LVGL 任务上调用）。
void ui_weather_failed(int result);
