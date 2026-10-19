#include <stdio.h>
#include "hal_spiffs.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_spiffs.h"

static const char *TAG = "esp32_s3_szp";

/***************    SPIFFS文件系统 ↓   *********************/

/**
 * @brief 挂载 SPIFFS 分区（分区标签 "storage"），并打印容量与占用。
 * @return ESP_OK 成功；其他为注册或查询的错误码
 */
esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE,
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    ESP_ERROR_CHECK(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}
