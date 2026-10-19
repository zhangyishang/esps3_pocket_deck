#include "wifi_net.h"
#include "app_events.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "string.h"

static const char *TAG = "app_ui";

#define EXAMPLE_ESP_MAXIMUM_RETRY  3 // wifi最大重连次数
// wifi事件组句柄
esp_event_handler_instance_t instance_any_id = NULL;
esp_event_handler_instance_t instance_got_ip = NULL;
esp_netif_t *sta_netif = NULL;

static int s_retry_num = 0;// 本次"连接尝试"的次数

// 初始化状态机。
#define WIFI_INIT_IDLE     0   // 没初始化，或已经 deinit
#define WIFI_INIT_INITING  1   // 有人正在初始化
#define WIFI_INIT_READY    2   // 完成，可以扫描/连接

static volatile int s_wifi_state = WIFI_INIT_IDLE; //wifi的状态
static portMUX_TYPE s_wifi_state_mux = portMUX_INITIALIZER_UNLOCKED; //wifi的互斥锁


/**
  * @brief  网络连接 事件处理函数
  * @param  
  * @retval  
  */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    // 判断事件类型：WiFi事件 且 为STA启动事件
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) 
    {
        // 设置事件组中的"WiFi启动"标志位，通知其他任务WiFi已启动
        xEventGroupSetBits(s_wifi_event_group, WIFI_START_BIT);
    } 
     // 判断事件类型：WiFi事件 且 为STA断开连接事件
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) 
    {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();// 重新发起WiFi连接
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT); // 超过最大重试次数，设置事件组中的"WiFi连接失败"标志位
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } 
    // 判断事件类型：IP事件 且 为STA获取到IP地址事件
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) 
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT); // 设置事件组中的"WiFi已连接"标志位，通知其他任务连接成功
    }
}

/**
  * @brief  一次性初始化 WiFi 模块（幂等，多任务并发安全）
  * @param  无
  * @retval ESP_OK 初始化成功或已就绪；ESP_ERR_INVALID_STATE 等待初始化超时仍未就绪
  */
esp_err_t wifiset_init_once(void)
{
    // 若状态已是 READY，直接返回，避免加锁开销
    if (s_wifi_state == WIFI_INIT_READY) {
        return ESP_OK;
    }

     // 进入临界区，保护状态判断与修改的原子性
    portENTER_CRITICAL(&s_wifi_state_mux);
    if (s_wifi_state == WIFI_INIT_READY) 
    {
        portEXIT_CRITICAL(&s_wifi_state_mux);
        return ESP_OK;
    }
    // 判断当前调用者是否是第一个来初始化的人，防止多次初始化
    int i_am_the_one = (s_wifi_state == WIFI_INIT_IDLE);
    if (i_am_the_one) 
    {
        s_wifi_state = WIFI_INIT_INITING;
    }
    //退出临界区
    portEXIT_CRITICAL(&s_wifi_state_mux);

    if (!i_am_the_one) 
    {
        for (int i = 0; i < 200 && s_wifi_state == WIFI_INIT_INITING; i++) 
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return (s_wifi_state == WIFI_INIT_READY) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    // 若事件组尚未创建，则创建 WiFi 事件组
    if (s_wifi_event_group == NULL)
     {
        s_wifi_event_group = xEventGroupCreate();
    }
    
    ESP_ERROR_CHECK(esp_netif_init());// 初始化网络接口层（TCP/IP 协议栈）

    esp_err_t loop_err = esp_event_loop_create_default();   // 创建默认事件循环；若已存在则忽略 INVALID_STATE 错误
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) 
    {
        ESP_ERROR_CHECK(loop_err);
    }

    sta_netif = esp_netif_create_default_wifi_sta(); // 创建默认的 WiFi STA 网络接口，并断言创建成功
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_state = WIFI_INIT_READY;
    ESP_LOGI(TAG, "WiFi 初始化完成");
    return ESP_OK;
}

// 扫描附近wifi
void wifi_scan(wifi_ap_record_t ap_info[], uint16_t *ap_number)
{
    if (wifiset_init_once() != ESP_OK) {
        *ap_number = 0;   // 没起来就别让调用方拿着上一次的个数去读缓冲
        return;
    }

    uint16_t ap_count = 0;

    memset(ap_info, 0, *ap_number * sizeof(wifi_ap_record_t));

    esp_wifi_scan_start(NULL, true);

    ESP_LOGI(TAG, "Max AP number ap_info can hold = %u", *ap_number);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));  // 获取扫描到的wifi数量
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(ap_number, ap_info)); // 获取真实的获取到wifi数量和信息
    ESP_LOGI(TAG, "Total APs scanned = %u, actual AP number ap_info holds = %u", ap_count, *ap_number);
}

// 每次发起一次新的连接尝试前调一次：重试预算跟着"一次尝试"走，而不是"一次开机"。
void wifi_net_reset_retry(void)
{
    s_retry_num = 0;
}

//判断WiFi是否链接
bool wifi_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

// 清除wifi初始化内容
void wifiset_deinit(void)
{

    if (s_wifi_event_group != NULL) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }

    if (instance_any_id != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
        instance_any_id = NULL;
    }

    if (instance_got_ip != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
        instance_got_ip = NULL;
    }

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
    }

    err = esp_wifi_deinit();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGE(TAG, "esp_wifi_deinit failed: %s", esp_err_to_name(err));
    }

    if (sta_netif != NULL) {
        esp_wifi_clear_default_wifi_driver_and_handlers(sta_netif);
        esp_netif_destroy(sta_netif);
        sta_netif = NULL;
    }

    portENTER_CRITICAL(&s_wifi_state_mux);
    s_wifi_state = WIFI_INIT_IDLE;
    portEXIT_CRITICAL(&s_wifi_state_mux);

    s_retry_num = 0;
}
