#pragma once

#include "app_events.h"
#include <stdbool.h>

/**
 * @brief 启动 WiFi 连接任务（阻塞等待 UI 投递的凭据并逐条处理）。
 */
void ui_wifi_connect_task_start(void);

/**
 * @brief 启动 WiFi 扫描任务（扫完把结果送 UI，并确保连接任务存在）。
 */
void ui_wifi_app_connect_task_start(void);

/**
 * @brief WiFi 扫描任务入口（xTaskCreate 需要非 static 符号）。
 * @param arg 未使用
 */
void app_wifi_connect(void *arg);

/**
 * @brief 启动对时任务（等联网成功后做 SNTP 同步，再通知 UI 显示时间）。
 */
void ui_wifi_get_time_task_start(void);

/**
 * @brief 幂等地保证"凭据队列 + 消费它的连接任务"这一对存在。
 */
void wifi_connect_ensure(void);

/**
 * @brief 投递一份 WiFi 凭据（所有要连接的地方都应走这里，不要直接 xQueueSend）。
 * @param acc 凭据
 * @return true 已入队；false 参数为空或队列创建失败
 */
bool wifi_connect_send(const wifi_account_t *acc);

/**
 * @brief 上电自动连接：读存档里最近的网络并投递；没有存档时什么都不做。
 */
void wifi_autoconnect_boot(void);
