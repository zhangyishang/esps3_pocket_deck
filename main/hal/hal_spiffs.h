#pragma once

#include "esp_err.h"

/***********************************************************/
/***************    SPIFFS文件系统 ↓   *********************/
#define SPIFFS_BASE             "/spiffs"

/**
 * @brief 挂载 SPIFFS 分区（标签 "storage"）到 SPIFFS_BASE。
 * @return ESP_OK 成功；其他为注册或查询的错误码
 */
esp_err_t bsp_spiffs_mount(void);

/***************    SPIFFS文件系统 ↑  *********************/
/**********************************************************/
