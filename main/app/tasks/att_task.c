#include "att_task.h"
#include "ui_msg.h"
#include "app_events.h"
#include "hal_qmi8658.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "math.h"

/**
 * @brief 向 UI 消息队列发送一条姿态相关消息。
 * @param id     UI 消息 ID
 * @param x      姿态角 X（整数）
 * @param y      姿态角 Y（整数）
 * @param z      姿态角 Z（整数）
 * @param motion 运动状态标志
 */
static void ui_att_post(ui_msg_id_t id, int x, int y, int z, int motion)
{
    ui_msg_t msg = { .id = id };
    msg.v.att.x      = x;
    msg.v.att.y      = y;
    msg.v.att.z      = z;
    msg.v.att.motion = motion;
    ui_msg_send(&msg, portMAX_DELAY);
}

/**
 * @brief 姿态监测任务：初始化 QMI8658，成功后每 200ms 采样一次三轴角度与运动状态
 *        并送 UI；初始化失败则提示 1 秒后关页；退出时关闭芯片并关页，然后自删除。
 * @param arg 未使用
 */
static void task_process_att(void *arg)
{
    esp_err_t ret = qmi8658_init();
    if (ret != ESP_OK){
        ui_att_post(UI_MSG_ATT_INIT_FAILED, 0, 0, 0, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        ui_att_post(UI_MSG_ATT_CLOSE_CANVAS, 0, 0, 0, 0);
    }
    else{
        ui_att_post(UI_MSG_ATT_BUILD, 0, 0, 0, 0);

        while (icon_flag == 1) {
            t_sQMI8658 QMI8658 = {0};
            int att_x, att_y, att_z;

            qmi8658_fetch_angleFromAcc(&QMI8658);
            att_x = round(QMI8658.AngleX);
            att_y = round(QMI8658.AngleY);
            att_z = round(QMI8658.AngleZ);

            uint8_t status = qmi8658_fetch_motion();

            ui_att_post(UI_MSG_ATT_UPDATE, att_x, att_y, att_z, status);

            vTaskDelay(200 / portTICK_PERIOD_MS);
        }

        qmi8658_close();
        ui_att_post(UI_MSG_ATT_CLOSE_CANVAS, 0, 0, 0, 0);
    }

    vTaskDelete(NULL);
}

/**
 * @brief 启动姿态任务（3KB 栈，优先级 5，绑核 1）。
 */
void ui_attitude_task_start(void)
{
    xTaskCreatePinnedToCore(task_process_att, "task_process_att", 3 * 1024, NULL, 5, NULL, 1);
}
