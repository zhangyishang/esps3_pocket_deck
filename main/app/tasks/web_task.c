#include "web_task.h"
#include "ui_msg.h"
#include "web_server.h"
#include "wifi_net.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "web_task";

/**
 * @brief 读取 STA 的 IPv4 地址（网络字节序）。
 * @return IPv4；未联网返回 0
 */
static uint32_t sta_ipv4(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        return 0;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return 0;
    }
    return info.ip.addr;
}

/**
 * @brief 把网络字节序的 IPv4 格式化成点分十进制。
 * @param ipv4 IPv4（网络字节序；0 表示未联网）
 * @param buf  输出
 * @param cap  容量
 */
static void fmt_ip(uint32_t ipv4, char *buf, size_t cap)
{
    if (ipv4 == 0) {
        snprintf(buf, cap, "-");
        return;
    }
    const unsigned char *p = (const unsigned char *)&ipv4;
    snprintf(buf, cap, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
}

/**
 * @brief 上报服务状态给 UI。
 * @param is_start 1 启动 / 0 停止
 * @param err      结果
 * @param ipv4     IPv4（网络字节序，可为 0）
 */
static void report(int is_start, esp_err_t err, uint32_t ipv4)
{
    ui_msg_t msg = { .id = UI_MSG_WEB_STATE };
    msg.v.i[0] = is_start;
    msg.v.i[1] = (int)err;
    // IP 走 .blob 文本通道：.v.i[1] 与 .v.u[1] 是同一块 4 字节，不能各写各的
    fmt_ip(ipv4, (char *)msg.blob, sizeof(msg.blob));
    ui_msg_send(&msg, portMAX_DELAY);
}

/**
 * @brief 启动任务的入口：起服务并把 IP 一起上报。
 * @param arg 未使用
 */
static void task_start(void *arg)
{
    esp_err_t err = ESP_OK;
    uint32_t ip = 0;

    if (!wifi_is_connected()) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        err = web_server_start();
        ip = sta_ipv4();
    }

    ESP_LOGI(TAG, "启动%s", err == ESP_OK ? "成功" : "失败");
    report(1, err, ip);
    vTaskDelete(NULL);
}

/**
 * @brief 停止任务的入口。
 * @param arg 未使用
 */
static void task_stop(void *arg)
{
    web_server_stop();
    report(0, ESP_OK, 0);
    vTaskDelete(NULL);
}

/**
 * @brief 异步启动网页服务（4KB 栈，优先级 5，绑核 0）。
 */
void ui_web_task_start(void)
{
    xTaskCreatePinnedToCore(task_start, "web_start", 4 * 1024, NULL, 5, NULL, 0);
}

/**
 * @brief 异步停止网页服务（4KB 栈，优先级 5，绑核 0）。
 */
void ui_web_task_stop(void)
{
    xTaskCreatePinnedToCore(task_stop, "web_stop", 4 * 1024, NULL, 5, NULL, 0);
}
