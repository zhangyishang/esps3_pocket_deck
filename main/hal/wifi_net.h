#pragma once

#include "esp_wifi_types.h"
#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief 幂等初始化 WiFi（netif / esp_wifi / 事件处理器 / 模式 / 启动），只做一次。
 * @return ESP_OK 表示现在可以扫描或连接；其他为初始化错误码
 */
esp_err_t wifiset_init_once(void);

/**
 * @brief 扫描附近 AP（内部会先保证 WiFi 已初始化）。
 * @param ap_info   输出：AP 记录数组
 * @param ap_number 输入容量 / 输出实际数量（初始化失败时置 0）
 */
void wifi_scan(wifi_ap_record_t ap_info[], uint16_t *ap_number);

/**
 * @brief 反初始化 WiFi（停止并释放 netif / 事件处理器）。
 */
void wifiset_deinit(void);

/**
 * @brief 重置底层重连计数器，每次发起新连接尝试前调用。
 */
void wifi_net_reset_retry(void);

/**
 * @brief 查询当前是否已连上 AP（未初始化时返回 false）。
 * @return true 已连接
 */
bool wifi_is_connected(void);
