#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief 尝试从 SD 卡加载运行时字体（TTF，任意字符都能显示）。
 *        路径固定为 /sdcard/fonts/cjk.ttf，字号 20。
 *        失败不致命 —— 继续用内嵌的 GB2312 字体。
 * @return ESP_OK 已切换到 SD 字体；其他表示未加载（用内嵌字体）
 */
esp_err_t hal_font_init(void);

/**
 * @brief 取当前应该用的 UI 字体（SD 字体加载成功就是它，否则是内嵌字体）。
 * @return 字体指针（永不为 NULL）
 */
const lv_font_t *hal_font_ui(void);

/**
 * @brief 把 hal_font_ui() 应用到屏幕与两个顶层图层（靠样式继承传下去）。
 *        必须在 LVGL 任务上下文或已持 LVGL 锁时调用。
 */
void hal_font_reapply(void);

/**
 * @brief 查询当前用的是不是 SD 卡字体。
 * @return true 正在用 SD 卡字体
 */
bool hal_font_is_from_sd(void);
