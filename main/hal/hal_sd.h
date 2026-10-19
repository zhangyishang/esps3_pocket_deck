#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

/***********************************************************/
/**********************    SD卡 ↓   *********************/
/* SD card GPIOs */
#define SD_CMD_IO      (48) 
#define SD_CLK_IO      (47)
#define SD_DAT0_IO     (21)

#define SD_MOUNT_POINT     "/sdcard"
#define SD_BASE            SD_MOUNT_POINT"/music"  // 音乐文件夹
#define SD_PHOTO_DIR       SD_MOUNT_POINT"/photo"  // 相机拍下的照片（.jpg）

/**
 * @brief 挂载 SD 卡（幂等：已挂载则直接返回 ESP_OK）。
 * @return ESP_OK 成功；其他为 FatFs / sdmmc 挂载错误码
 */
esp_err_t bsp_sdcard_mount(void);

/**
 * @brief 卸载 SD 卡（未挂载时直接返回 ESP_OK）。
 * @return ESP_OK 成功；其他为卸载错误码
 */
esp_err_t bsp_sdcard_unmount(void);

/**
 * @brief 查询 SD 卡当前是否已挂载。
 * @return true 已挂载
 */
bool bsp_sdcard_is_mounted(void);

/**
 * @brief 获取 sdmmc 卡对象。
 * @return 卡对象指针；未挂载或已卸载时为 NULL
 */
sdmmc_card_t *hal_sdcard_get_card(void);

/**
 * @brief 加锁独占 SD 卡（递归锁，同一任务可嵌套）。
 *        多任务（音乐播放、相机拍照、网页上传、图库）访问 SD 前必须持有它。
 * @param timeout_ms 超时毫秒；传 UINT32_MAX 表示一直等
 * @return ESP_OK 成功；ESP_ERR_TIMEOUT 超时
 */
esp_err_t hal_sdcard_lock(uint32_t timeout_ms);

/**
 * @brief 释放 SD 卡锁。
 */
void hal_sdcard_unlock(void);
/**********************    SD卡 ↑  *********************/
/**********************************************************/
