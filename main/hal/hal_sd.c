#include <stdio.h>
#include "hal_sd.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "esp32_s3_szp";

/*********************    SD卡  ↓   *********************/
static sdmmc_card_t *sdmmc_card = NULL;

static bool s_mounted = false;

/**
 * @brief SD 卡访问互斥量（递归：允许同一任务嵌套加锁）。
 */
static SemaphoreHandle_t s_sd_mutex = NULL;

/**
 * @brief 获取（必要时创建）SD 卡互斥量。
 * @return 互斥量句柄；创建失败为 NULL
 */
static SemaphoreHandle_t sd_mutex_get(void)
{
    if (s_sd_mutex == NULL) {
        s_sd_mutex = xSemaphoreCreateRecursiveMutex();
        configASSERT(s_sd_mutex);
    }
    return s_sd_mutex;
}

/**
 * @brief 加锁，独占 SD 卡（可递归）。
 * @param timeout_ms 超时（毫秒）
 * @return ESP_OK 成功；ESP_ERR_TIMEOUT 超时
 */
esp_err_t hal_sdcard_lock(uint32_t timeout_ms)
{
    SemaphoreHandle_t m = sd_mutex_get();
    if (m == NULL) {
        return ESP_ERR_NO_MEM;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTakeRecursive(m, ticks) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/**
 * @brief 释放 SD 卡锁。
 */
void hal_sdcard_unlock(void)
{
    if (s_sd_mutex != NULL) {
        xSemaphoreGiveRecursive(s_sd_mutex);
    }
}

/**
 * @brief 挂载 SD 卡（幂等）。
 * @return ESP_OK 成功；其他为 FatFs / sdmmc 挂载错误码
 */
esp_err_t bsp_sdcard_mount(void)
{
    hal_sdcard_lock(UINT32_MAX);
    if (s_mounted) {
        hal_sdcard_unlock();
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Mounting SD card");
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 8 * 1024
    };

    sdmmc_host_t sdmmc_host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = SD_CLK_IO;
    slot_config.cmd = SD_CMD_IO;
    slot_config.d0 = SD_DAT0_IO;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem Starting");

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &sdmmc_host, &slot_config, &mount_config, &sdmmc_card);

    if (ret == ESP_OK) {
        s_mounted = true;
    } else {
        sdmmc_card = NULL;
    }

    hal_sdcard_unlock();
    return ret;
}

/**
 * @brief 卸载 SD 卡（未挂载时直接返回 ESP_OK）。
 * @return ESP_OK 成功；其他为卸载错误码
 */
esp_err_t bsp_sdcard_unmount(void)
{
    hal_sdcard_lock(UINT32_MAX);
    if (!s_mounted) {
        hal_sdcard_unlock();
        return ESP_OK;
    }
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, sdmmc_card);
    if (ret == ESP_OK) {
        s_mounted = false;
        sdmmc_card = NULL;
    }

    hal_sdcard_unlock();
    return ret;
}

/**
 * @brief 查询 SD 卡是否已挂载。
 * @return true 已挂载
 */
bool bsp_sdcard_is_mounted(void)
{
    return s_mounted;
}

/**
 * @brief 获取 sdmmc 卡对象。
 * @return 卡对象指针；未挂载时为 NULL
 */
sdmmc_card_t *hal_sdcard_get_card(void)
{
    return sdmmc_card;
}
/**********************    SD卡 ↑  ************************/
/**********************************************************/
