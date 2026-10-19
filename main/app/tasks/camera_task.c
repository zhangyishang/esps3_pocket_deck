#include "camera_task.h"
#include "ui_msg.h"
#include "app_events.h"
#include "hal_camera.h"
#include "hal_pca9557.h"
#include "hal_sd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "img_converters.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "camera_task";

/**
 * @brief 帧完成信号量（二值）：UI 渲染完一帧后 give，采集任务 take 后才取下一帧。
 */
static SemaphoreHandle_t s_frame_done;

/**
 * @brief 拍照请求标志：UI 置位，采集任务在归还帧之前消费。
 */
static volatile bool s_capture_req;

/**
 * @brief 请求拍摄当前帧。
 */
void ui_camera_request_capture(void)
{
    s_capture_req = true;
}

/**
 * @brief 通知采集任务：上一帧已渲染完，可以取下一帧。
 */
void ui_camera_frame_release(void)
{
    if (s_frame_done != NULL) {
        xSemaphoreGive(s_frame_done);
    }
}

/**
 * @brief 将一帧图像编码为 JPEG 并写入 SD 卡。
 * @param frame 摄像头帧缓冲
 * @return CAMERA_CAPTURE_OK 成功；CAMERA_CAPTURE_ENCODE_FAIL 编码失败；
 *         CAMERA_CAPTURE_WRITE_FAIL 写文件失败
 */
static int camera_write_jpeg(camera_fb_t *frame)
{
    mkdir(SD_PHOTO_DIR, 0777);

    char path[64];
    struct stat st;
    int idx = 0;
    for (int i = 1; i <= 9999; i++) {
        snprintf(path, sizeof(path), SD_PHOTO_DIR "/IMG_%04d.jpg", i);
        if (stat(path, &st) != 0) {
            idx = i;
            break;
        }
    }
    if (idx == 0) {
        ESP_LOGE(TAG, "照片目录已满（IMG_0001..IMG_9999 都在）");
        return CAMERA_CAPTURE_WRITE_FAIL;
    }

    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    if (!frame2jpg(frame, 80, &jpg, &jpg_len) || jpg == NULL) {
        ESP_LOGE(TAG, "frame2jpg 失败");
        return CAMERA_CAPTURE_ENCODE_FAIL;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "打不开 %s", path);
        free(jpg);
        return CAMERA_CAPTURE_WRITE_FAIL;
    }

    size_t written = fwrite(jpg, 1, jpg_len, f);
    fclose(f);
    free(jpg);

    if (written != jpg_len) {
        ESP_LOGE(TAG, "写 %s 只写了 %u/%u 字节", path, (unsigned)written, (unsigned)jpg_len);
        return CAMERA_CAPTURE_WRITE_FAIL;
    }

    ESP_LOGI(TAG, "照片已保存: %s (%u 字节)", path, (unsigned)jpg_len);
    return CAMERA_CAPTURE_OK;
}

/**
 * @brief 把一帧保存到 SD 卡（按需挂载，写完后按需卸载）。
 * @param frame 摄像头帧缓冲
 * @return CAMERA_CAPTURE_OK 成功；CAMERA_CAPTURE_NO_SD 未挂载 SD；其他为写入错误码
 */
static int camera_save_frame(camera_fb_t *frame)
{
    bool mounted_here = false;

    if (!bsp_sdcard_is_mounted()) {
        if (bsp_sdcard_mount() != ESP_OK) {
            ESP_LOGW(TAG, "SD 卡没挂上，照片存不了");
            return CAMERA_CAPTURE_NO_SD;
        }
        mounted_here = true;
    }

    int result = camera_write_jpeg(frame);

    if (mounted_here) {
        bsp_sdcard_unmount();
    }
    return result;
}

/**
 * @brief 摄像头预览任务：取帧 → 送 UI → 等渲染完成 → 按需拍照 → 归还帧。
 *        只要 icon_flag 仍为 4（摄像头页）就持续采集；退出时反初始化摄像头并通知 UI。
 * @param arg 未使用
 */
static void task_process_camera(void *arg)
{
    while (icon_flag == 4)
    {
        camera_fb_t *frame = esp_camera_fb_get();
        ui_msg_t msg = { .id = UI_MSG_CAMERA_FRAME, .ptr = frame };
        ui_msg_send(&msg, portMAX_DELAY);

        xSemaphoreTake(s_frame_done, portMAX_DELAY);

        if (s_capture_req) {
            s_capture_req = false;
            if (frame != NULL) {
                int result = camera_save_frame(frame);
                ui_msg_t done = { .id = UI_MSG_CAMERA_CAPTURE_DONE };
                done.v.i[0] = result;
                ui_msg_send(&done, 0);
            }
        }

        esp_camera_fb_return(frame);
    }

    esp_camera_deinit();
    {
        ui_msg_t msg = { .id = UI_MSG_CAMERA_CLOSE };
        ui_msg_send(&msg, portMAX_DELAY);
    }
    dvp_pwdn(1);
    vTaskDelete(NULL);
}

/**
 * @brief 启动摄像头预览任务（首次调用时创建帧完成信号量）。
 */
void ui_camera_task_start(void)
{
    if (s_frame_done == NULL) {
        s_frame_done = xSemaphoreCreateBinary();
        configASSERT(s_frame_done);
    }
    xTaskCreatePinnedToCore(task_process_camera, "task_process_camera",
                            8 * 1024, NULL, 5, NULL, 1);
}
