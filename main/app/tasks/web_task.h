#pragma once

/**
 * @brief 启动网页文件管理服务（异步：结果经 UI_MSG_WEB_STATE 上报）。
 */
void ui_web_task_start(void);

/**
 * @brief 停止网页文件管理服务（异步：结果经 UI_MSG_WEB_STATE 上报）。
 */
void ui_web_task_stop(void);
