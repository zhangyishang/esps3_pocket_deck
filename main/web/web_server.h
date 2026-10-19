#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief 启动 HTTP 文件管理服务（端口 80）。
 *        必须在 WiFi 已连上之后调用。
 * @return ESP_OK 成功；其他为 esp_http_server 的错误码
 */
esp_err_t web_server_start(void);

/**
 * @brief 停止 HTTP 服务。
 */
void web_server_stop(void);

/**
 * @brief 查询服务是否在运行。
 * @return true 正在运行
 */
bool web_server_is_running(void);
