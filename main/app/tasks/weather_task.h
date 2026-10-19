#pragma once

#include <stdbool.h>

/**
 * @brief 启动一次天气抓取任务（一次性任务，抓完即退）。
 * @return true 任务已创建；false 上一次抓取还没结束，或创建失败
 */
bool ui_weather_task_start(void);
