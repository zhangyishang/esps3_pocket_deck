#include "wifi_net.h"
#include "wifi_store.h"
#include "ui_msg.h"
#include "app_events.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "string.h"
#include <stdlib.h>
#include <time.h>
#include "string.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "wifi_connect_task.h"

static const char *TAG = "app_ui";

#define DEFAULT_SCAN_LIST_SIZE   5

/**
 * @brief 扫描结果缓冲（文件级 static，生命周期长于 app_wifi_connect 任务，
 *        消息通过 .ptr 指向它，UI 侧只读不释放）。
 */
static wifi_ap_record_t s_ap_info[DEFAULT_SCAN_LIST_SIZE];

/**
 * @brief 把连接结果发成一条 UI 消息。
 * @param id     UI_MSG_WIFI_CONN_RESULT（更新文字）或 UI_MSG_WIFI_CONN_CLOSE（关页）
 * @param result 结果码（wifi_result_t）
 * @param ssid   本次连接的目标 SSID，随 .blob 传递；NULL 表示不携带
 */
static void wifi_conn_notify(ui_msg_id_t id, int result, const char *ssid)
{
    ui_msg_t msg = { .id = id };
    msg.v.i[0] = result;
    if (ssid != NULL) {
        strlcpy((char *)msg.blob, ssid, sizeof(msg.blob));
    }
    ui_msg_send(&msg, portMAX_DELAY);
}

/**
 * @brief WiFi 连接任务：阻塞等待 UI 投递的凭据，发起连接，把结果通知 UI；
 *        成功后写入凭据存档并启动对时任务，然后自删除。
 * @param arg 未使用
 */
static void wifi_connect(void *arg)
{
    wifi_account_t wifi_account = {0};
    wifi_config_t wifi_config = {
    .sta = {
        .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        .sae_h2e_identifier = "",
        },
    };

    if(xQueueWifiAccount == NULL)
    {
        xQueueWifiAccount = xQueueCreate(2, sizeof(wifi_account_t));
    }

    while (true)
    {
        if(xQueueReceive(xQueueWifiAccount, &wifi_account, portMAX_DELAY))
        {
            if (wifi_account.back_flag == 1)
            {
                ESP_LOGI(TAG, "User canceled WiFi connection");
                wifi_conn_notify(UI_MSG_WIFI_CONN_RESULT, WIFI_RESULT_OK, NULL);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                wifi_conn_notify(UI_MSG_WIFI_CONN_CLOSE, WIFI_RESULT_OK, NULL);
                icon_flag = 0;
                break;
            }

            ESP_LOGI(TAG, "ready to connect to WiFi SSID:%s", wifi_account.wifi_ssid);
            strlcpy((char *)wifi_config.sta.ssid, wifi_account.wifi_ssid,
                    sizeof(wifi_config.sta.ssid));
            strlcpy((char *)wifi_config.sta.password, wifi_account.wifi_password,
                    sizeof(wifi_config.sta.password));

            if (s_wifi_event_group != NULL) {
                xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
            }

            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
            wifi_net_reset_retry();
            esp_wifi_connect();

            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                pdFALSE,
                                pdFALSE,
                                pdMS_TO_TICKS(10000));

            if (bits & WIFI_CONNECTED_BIT)
            {
                ESP_LOGI(TAG, "connected to ap SSID:%s", wifi_config.sta.ssid);

                wifi_store_save((const char *)wifi_config.sta.ssid,
                                (const char *)wifi_config.sta.password);

                wifi_conn_notify(UI_MSG_WIFI_CONN_RESULT, WIFI_RESULT_OK,
                                 (const char *)wifi_config.sta.ssid);

                if (wifi_account.back_flag != WIFI_BACK_FLAG_AUTO) {
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                    wifi_conn_notify(UI_MSG_WIFI_CONN_CLOSE, WIFI_RESULT_OK, NULL);

                    icon_flag = 0;
                }

                ui_wifi_get_time_task_start();
                break;
            }
            else if (bits & WIFI_FAIL_BIT)
            {
                ESP_LOGI(TAG, "Failed to connect to SSID:%s", wifi_config.sta.ssid);
                wifi_conn_notify(UI_MSG_WIFI_CONN_RESULT, WIFI_RESULT_FAIL, NULL);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                wifi_conn_notify(UI_MSG_WIFI_CONN_CLOSE, WIFI_RESULT_FAIL, NULL);
            }
            else
            {
                ESP_LOGE(TAG, "UNEXPECTED EVENT");
                wifi_conn_notify(UI_MSG_WIFI_CONN_RESULT, WIFI_RESULT_UNEXPECTED, NULL);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                wifi_conn_notify(UI_MSG_WIFI_CONN_CLOSE, WIFI_RESULT_UNEXPECTED, NULL);
                wifiset_deinit();
                break;
            }
        }
    }

    if (xQueueWifiAccount != NULL)
    {
        vQueueDelete(xQueueWifiAccount);
        xQueueWifiAccount = NULL;
    }
    vTaskDelete(NULL);
}

/**
 * @brief 启动 WiFi 连接任务（4KB 栈，优先级 5，绑核 1）。
 */
void ui_wifi_connect_task_start(void)
{
    xTaskCreatePinnedToCore(wifi_connect, "wifi_connect", 4 * 1024, NULL, 5, NULL, 1);
}

/**
 * @brief 幂等地保证"凭据队列 + 消费它的 wifi_connect 任务"这一对存在。
 *        队列已存在说明任务仍在运行，此时不能再起第二个。
 */
void wifi_connect_ensure(void)
{
    if (xQueueWifiAccount == NULL) {
        xQueueWifiAccount = xQueueCreate(2, sizeof(wifi_account_t));
        ui_wifi_connect_task_start();
    }
}

/**
 * @brief 投递一份 WiFi 凭据（所有要连接的地方都应走这里）。
 * @param acc 凭据
 * @return true 已入队；false 参数为空或队列创建失败
 */
bool wifi_connect_send(const wifi_account_t *acc)
{
    if (acc == NULL) {
        return false;
    }

    wifi_connect_ensure();
    if (xQueueWifiAccount == NULL) {
        return false;
    }
    return xQueueSend(xQueueWifiAccount, acc, portMAX_DELAY) == pdTRUE;
}

/**
 * @brief 上电自动连接：读凭据存档，若存在则初始化 WiFi 并把凭据投递给既有的连接任务。
 *        没有存档时完全不碰 WiFi。
 */
void wifi_autoconnect_boot(void)
{
    wifi_saved_t last;
    if (wifi_store_count() == 0 || !wifi_store_last(&last)) {
        return;
    }

    if (wifiset_init_once() != ESP_OK) {
        ESP_LOGE(TAG, "上电自动连接：WiFi 初始化失败，放弃本次自动连接");
        return;
    }

    wifi_account_t acc = {0};
    strlcpy(acc.wifi_ssid, last.ssid, sizeof(acc.wifi_ssid));
    strlcpy(acc.wifi_password, last.password, sizeof(acc.wifi_password));
    acc.back_flag = WIFI_BACK_FLAG_AUTO;

    ESP_LOGI(TAG, "上电自动连接：%s", acc.wifi_ssid);
    wifi_connect_send(&acc);
}

/**
 * @brief WiFi 扫描任务：延迟 200ms 后扫描附近 AP，把结果（.ptr 指向 s_ap_info）
 *        送 UI，确保连接任务存在后自删除。
 * @param arg 未使用
 */
void app_wifi_connect(void *arg)
{
    vTaskDelay(200 / portTICK_PERIOD_MS);

    uint16_t ap_number = DEFAULT_SCAN_LIST_SIZE;
    wifi_scan(s_ap_info, &ap_number);

    {
        ui_msg_t msg = { .id = UI_MSG_WIFI_AP_LIST };
        msg.ptr    = s_ap_info;
        msg.v.u[0] = ap_number;
        ui_msg_send(&msg, portMAX_DELAY);
    }

    wifi_connect_ensure();
    vTaskDelete(NULL);
}

/**
 * @brief 启动 WiFi 扫描任务（4KB 栈，优先级 5，绑核 0）。
 */
void ui_wifi_app_connect_task_start(void)
{
    xTaskCreatePinnedToCore(app_wifi_connect, "app_wifi_connect", 4*1024, NULL, 5, NULL, 0);
}

/**
 * @brief 对时任务：等 WiFi 连上后启动 SNTP 同步系统时间，设置时区，
 *        通知 UI 显示日期时间，然后自删除。
 * @param pvParameters 未使用
 */
static void get_time_task(void *pvParameters)
{
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    /* 用阿里云的 NTP：国内可达性比 cn.pool.ntp.org 好得多。
     * 原来只有 cn.pool.ntp.org 一个源，解析失败/超时就永远对不上时。 */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_netif_sntp_init(&config);

    /* 重试要有上限：原来 retry_count 被注释掉，是死循环。
     * 15 次 × 2 秒 = 最多 30 秒，超时就往下走（不能因为对时失败卡住 UI）。 */
    const int retry_count = 15;
    int retry = 0;
    esp_err_t sync_ret = ESP_ERR_TIMEOUT;
    while (retry < retry_count) {
        sync_ret = esp_netif_sntp_sync_wait(2000 / portTICK_PERIOD_MS);
        if (sync_ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "等待 SNTP 对时… (%d/%d)", ++retry, retry_count);
    }
    if (sync_ret == ESP_OK) {
        ESP_LOGI(TAG, "SNTP 对时成功");
    } else {
        ESP_LOGE(TAG, "SNTP 对时超时（%d 次），先继续，稍后可重试", retry);
    }

    esp_netif_sntp_deinit();

    /* 无论对时成功与否，时区和 UI 通知都要走 —— 否则界面永远停在
     * "Waiting for system time to be set..."。 */
    setenv("TZ", "CST-8", 1);
    tzset();

    {
        ui_msg_t msg = { .id = UI_MSG_WIFI_DATETIME_SHOW };
        ui_msg_send(&msg, portMAX_DELAY);
    }

    xEventGroupSetBits(s_wifi_event_group, WIFI_GET_SNTP_BIT);

    vTaskDelete(NULL);
}

/**
 * @brief 启动对时任务（3KB 栈，优先级 5，绑核 0）。
 */
void ui_wifi_get_time_task_start(void)
{
    xTaskCreatePinnedToCore(get_time_task, "get_time_task", 3 * 1024, NULL, 5, NULL, 0);
}
