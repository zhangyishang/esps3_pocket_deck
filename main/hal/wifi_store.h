#pragma once
#include <stdbool.h>
#include <stddef.h>

#define WIFI_STORE_MAX 5

typedef struct {
    char ssid[33];
    char password[64];
} wifi_saved_t;

/**
 * @brief 读取存档里现有的网络个数（直接读 NVS，不在 RAM 缓存）。
 * @return 网络个数；没存过或出错返回 0
 */
int wifi_store_count(void);

/**
 * @brief 取第 index 个已保存的网络（0 = 最近一次成功连接的）。
 * @param index 下标
 * @param out   输出
 * @return true 成功；false 越界或存档为空
 */
bool wifi_store_get(int index, wifi_saved_t *out);

/**
 * @brief 取最近一次成功连接的网络。
 * @param out 输出
 * @return true 成功；false 存档为空
 */
bool wifi_store_last(wifi_saved_t *out);

/**
 * @brief 按 SSID 查询密码。
 * @param ssid SSID
 * @param buf  输出缓冲（找不到时置为空串）
 * @param len  缓冲长度
 * @return true 找到；false 找不到
 */
bool wifi_store_get_password(const char *ssid, char *buf, size_t len);

/**
 * @brief 保存一个网络（upsert：已存在则更新密码并移到最前，满了挤掉最后一个）。
 * @param ssid     SSID
 * @param password 密码
 * @return true 成功；false ssid 为空或 NVS 读写失败
 */
bool wifi_store_save(const char *ssid, const char *password);

/**
 * @brief 删除一个网络（幂等）。
 * @param ssid SSID
 * @return true 成功（找不到也算成功）
 */
bool wifi_store_forget(const char *ssid);
