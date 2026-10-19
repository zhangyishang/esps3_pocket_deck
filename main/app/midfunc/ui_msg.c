#include "ui_msg.h"
#include "esp_log.h"

static const char *TAG = "ui_msg";

// LVGL 任务的 FreeRTOS 任务名 —— 即 esp_lvgl_port.c 里
// xTaskCreate(lvgl_port_task, "LVGL task", ...) 的第二个参数。
//
// 这是本文件与图形库之间**唯一**的耦合点，故意只用一字符串而不是 include
// esp_lvgl_port.h，否则 midfunc/ 就依赖图形库了。名字若哪天变了，
// ui_msg_bind_ui_task() 会在首次抽干时兜底补记，不会静默失效。
#define UI_MSG_LVGL_TASK_NAME   "LVGL task"

// 队列深度 2：够容纳"一条正在被抽干 + 一条在排队"。更深没有意义 ——
// 抽干在 LVGL 任务上以 30ms 周期跑，积压只说明消费端跟不上，而积压的
// 建页面消息本身也已经过时了。
#define UI_MSG_QUEUE_DEPTH      2

// 队列内存是"深度 × 单条大小"，而这条消息还要在每个**发送者的栈上**临时存在一份。
// 所以尺寸预算是双向的：队列占内部 RAM，栈占任务栈。把上限钉死在编译期，
// 别等改大 UI_MSG_BLOB_MAX 之后靠人记。
_Static_assert(sizeof(ui_msg_t) <= 512,
               "ui_msg_t 超过 512 字节：队列占用与发送者栈开销都需重新评估");

static QueueHandle_t s_queue;
static TaskHandle_t  s_ui_task;

void ui_msg_init(void)
{
    // 常驻队列：建一次，永不删除。
    // 本项目踩过 vQueueDelete(xQueueWifiAccount) 与阻塞在 xQueueReceive 的
    // 接收者共存的坑（ui_wifi.c），一条永不删除的队列把这类生命周期问题
    // 整类消除。
    s_queue = xQueueCreate(UI_MSG_QUEUE_DEPTH, sizeof(ui_msg_t));
    configASSERT(s_queue);

#if defined(INCLUDE_xTaskGetHandle) && (INCLUDE_xTaskGetHandle == 1)
    s_ui_task = xTaskGetHandle(UI_MSG_LVGL_TASK_NAME);
#endif
    if (s_ui_task == NULL) {
        ESP_LOGW(TAG, "没找到任务 \"%s\"，改由首次抽干时补记", UI_MSG_LVGL_TASK_NAME);
    }
}

void ui_msg_bind_ui_task(void)
{
    if (s_ui_task == NULL) {
        // 本函数由 ui_router 的抽干回调调用，而抽干回调在 lv_timer_handler()
        // 之内执行，所以当前任务就是 LVGL 任务。
        s_ui_task = xTaskGetCurrentTaskHandle();
        ESP_LOGI(TAG, "已补记 LVGL 任务句柄");
    }
}

bool ui_msg_send(const ui_msg_t *msg, TickType_t wait_ticks)
{
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "队列未建立（漏了 ui_msg_init？），丢弃 id=%d", (int)msg->id);
        return false;
    }

    if (wait_ticks != 0 && s_ui_task != NULL &&
        xTaskGetCurrentTaskHandle() == s_ui_task) {
        wait_ticks = 0;
    }

    if (xQueueSend(s_queue, msg, wait_ticks) != pdTRUE) {
        ESP_LOGW(TAG, "队列满，丢弃消息 id=%d", (int)msg->id);
        return false;
    }
    return true;
}

bool ui_msg_recv(ui_msg_t *out, TickType_t wait_ticks)
{
    if (s_queue == NULL) {
        return false;
    }
    return xQueueReceive(s_queue, out, wait_ticks) == pdTRUE;
}
