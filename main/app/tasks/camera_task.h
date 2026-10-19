#pragma once

/**
 * @brief 启动摄像头预览任务（首次调用时创建帧完成信号量）。
 */
void ui_camera_task_start(void);

/**
 * @brief 通知采集任务：上一帧已渲染完，可以取下一帧。
 */
void ui_camera_frame_release(void);

/**
 * @brief 请求拍摄当前帧（真正的编码与写卡在采集任务里做）。
 */
void ui_camera_request_capture(void);
