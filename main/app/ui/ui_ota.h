#pragma once

#include "lvgl.h"

// 系统升级应用。
void ota_event_handler(lv_event_t * e);

// 检查完成，有新版本。data 指向 ui_msg_t 的 .blob（一个 ota_found_t）。
void ui_ota_found(const void *data);
// 检查完成，已是最新。
void ui_ota_latest(void);
// 下载进度，0~100。
void ui_ota_progress(int pct);
// 失败。code 是 ota_result_t。
void ui_ota_failed(int code);
