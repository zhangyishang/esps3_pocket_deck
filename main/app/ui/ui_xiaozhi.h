#pragma once

#include "lvgl.h"
#include "ui_msg.h"

/**
 * @brief 小智语音助手入口（主界面图标点击时调用）。
 * @param e LVGL 事件（未使用）
 */
void xiaozhi_event_handler(lv_event_t *e);

/**
 * @brief 更新对话状态显示。
 * @param st xz_state_t
 */
void ui_xiaozhi_show_state(int st);

/**
 * @brief 显示一行字幕。
 * @param is_user true = 用户说的（STT），false = 助手回复
 * @param text    UTF-8 文本
 */
void ui_xiaozhi_show_text(bool is_user, const char *text);

/**
 * @brief 显示错误。
 * @param code xz_error_t
 */
void ui_xiaozhi_show_error(int code);

/**
 * @brief 显示官方云要求的设备激活码（用户到 xiaozhi.me 输入）。
 * @param code 激活码
 */
void ui_xiaozhi_show_activation(const char *code);

/**
 * @brief 显示一行诊断文字（不接串口也能看到卡在哪一步）。
 * @param text 诊断内容
 */
void ui_xiaozhi_show_debug(const char *text);

/**
 * @brief 删除小智页面。
 */
void ui_xiaozhi_close_canvas(void);
