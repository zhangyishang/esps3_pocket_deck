#pragma once

#include "app_events.h"

/**
 * @brief 启动姿态监测任务（每 200ms 采样一次并送 UI）。
 */
void ui_attitude_task_start(void);
