#include "sdcard_task.h"
#include "ui_msg.h"
#include "hal_sd.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "app_ui";

/**
 * @brief SD 卡任务：挂载 SD 卡，成功后打印卡信息并通知 UI 建页（携带容量 GB），
 *        失败则通知 UI 报错并关页，然后自删除。
 * @param arg 未使用
 */
static void task_process_sdcard(void *arg)
{
    esp_err_t ret;

    ret = bsp_sdcard_mount();
    if(ret != ESP_OK){
        ESP_LOGE(TAG, "Failed to mount filesystem.");
        {
            ui_msg_t msg = { .id = UI_MSG_SDCARD_MOUNT_FAILED };
            ui_msg_send(&msg, portMAX_DELAY);
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
        {
            ui_msg_t msg = { .id = UI_MSG_SDCARD_CLOSE };
            ui_msg_send(&msg, portMAX_DELAY);
        }
    }else{
        sdmmc_card_print_info(stdout, hal_sdcard_get_card());

        ui_msg_t msg = { .id = UI_MSG_SDCARD_BUILD,
                         .v.u64 = (((uint64_t)hal_sdcard_get_card()->csd.capacity) *
                                   hal_sdcard_get_card()->csd.sector_size) >> 30 };
        ui_msg_send(&msg, portMAX_DELAY);
    }

    vTaskDelete(NULL);
}

/**
 * @brief 启动 SD 卡任务（3KB 栈，优先级 5，绑核 1）。
 */
void ui_sdcard_task_start(void)
{
    xTaskCreatePinnedToCore(task_process_sdcard, "task_process_sdcard", 3 * 1024, NULL, 5, NULL, 1);
}
