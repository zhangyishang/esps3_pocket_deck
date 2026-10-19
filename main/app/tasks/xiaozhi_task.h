#pragma once

#include <stdbool.h>

/**
 * @brief 启动小智语音助手（异步连接 WebSocket 服务端）。
 *        结果通过 UI_MSG_XZ_STATE / UI_MSG_XZ_FAILED 上报。
 */
void ui_xiaozhi_task_start(void);

/**
 * @brief 停止小智助手（断开连接、释放 Opus、回 UI_MSG_XZ_CLOSE）。
 */
void ui_xiaozhi_task_stop(void);

/**
 * @brief 按住说话：开始录音并上传。
 */
void ui_xiaozhi_record_begin(void);

/**
 * @brief 松开：结束录音，等服务端回话。
 */
void ui_xiaozhi_record_end(void);

/**
 * @brief 查询当前连接是否就绪。
 * @return true 可以说话
 */
bool ui_xiaozhi_is_ready(void);
/**
 * @brief 设置播放音量（0~100），同时应用到 codec。
 * @param vol 音量，超出范围会被裁剪
 */
void ui_xiaozhi_set_volume(int vol);

/**
 * @brief 读取当前音量。
 * @return 0~100
 */
int ui_xiaozhi_get_volume(void);
