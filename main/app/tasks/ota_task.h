#pragma once

#include <stdbool.h>

/**
 * @brief 启动一次"检查更新"任务（拉版本清单并比对，结果发消息给 UI，抓完即退）。
 * @return true 任务已创建；false 上一次还没结束，或创建失败
 */
bool ui_ota_check_start(void);

/**
 * @brief 启动一次"下载并升级"任务（用上一次检查的结果下载、写入备份槽并重启）。
 * @return true 任务已创建；false 上一次还没结束，或创建失败
 */
bool ui_ota_upgrade_start(void);

/**
 * @brief 查询是否正忙（检查中或下载中），供 UI 决定按钮是否响应。
 * @return true 正在检查或升级
 */
bool ui_ota_busy(void);
