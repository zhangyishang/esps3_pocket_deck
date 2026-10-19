#include "main_page_task.h"
#include "app_events.h"
#include "ui_msg.h"
#include "wifi_connect_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

/**
 * @brief 开机任务：先让 UI 构建锁屏页，再发起 WiFi 自动连接，然后自删除。
 * @param pvParameters 未使用
 */
static void main_page_task(void *pvParameters)
{
    ui_msg_t msg = { .id = UI_MSG_LOCKSCREEN_BUILD };
    ui_msg_send(&msg, portMAX_DELAY);

    wifi_autoconnect_boot();

    vTaskDelete(NULL);
}

/**
 * @brief 启动开机任务（4KB 栈，优先级 5，绑核 0）。
 */
void ui_main_page_task_start(void)
{
    xTaskCreatePinnedToCore(main_page_task, "main_page_task", 4*1024, NULL, 5, NULL, 0);
}
