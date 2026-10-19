#include "app_ui.h"
#include "main_page_task.h"
#include "ota_task.h"        // OTA 头文件（回滚自检在本文件；升级动作在 ota_task.c）
#include "hal_i2c.h"
#include "hal_pca9557.h"
#include "hal_spiffs.h"
#include "hal_audio.h"
#include "hal_font.h"        // hal_font_init()：从 SD 卡加载运行时字体
#include "mw_lvgl.h"
#include "ui_msg.h"
#include "ui_router.h"
#include "ui_common.h"       // ui_is_alive()：回滚自检的判据之一
#include "wifi_net.h"        // wifi_is_connected()
#include "wifi_store.h"      // wifi_store_count()
#include "esp_ota_ops.h"     // 回滚自检
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

static const char *TAG = "MAIN";

#define OTA_CONFIRM_DELAY_MS 60000

static void ota_confirm_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(OTA_CONFIRM_DELAY_MS));

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (running == NULL ||
        esp_ota_get_state_partition(running, &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        vTaskDelete(NULL);
        return;
    }

    bool ui_ok = ui_is_alive();

    // 只有"存档里本来就有网络"时才把连不上当成失败
    bool have_creds = (wifi_store_count() > 0);
    bool net_ok = have_creds ? wifi_is_connected() : true;

    if (ui_ok && net_ok) {
        ESP_LOGW(TAG, "OTA 自检通过（UI=%d 网=%d 有存档=%d），确认新固件",
                 (int)ui_ok, (int)net_ok, (int)have_creds);
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "确认失败: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "OTA 自检未通过（UI=%d 网=%d 有存档=%d），"
                      "不做确认 —— 下次重启会自动回滚",
                 (int)ui_ok, (int)net_ok, (int)have_creds);
    }

    vTaskDelete(NULL);
}

// 栈 3KB 足够：它只是延时 + 读两个状态量。
static void ota_confirm_start(void)
{
    xTaskCreatePinnedToCore(ota_confirm_task, "ota_confirm", 3 * 1024, NULL, 4, NULL, 0);
}

// 打印内存使用情况
void displayMemoryUsage() {
    size_t totalDRAM = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t freeDRAM = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t usedDRAM = totalDRAM - freeDRAM;

    size_t totalPSRAM = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t freePSRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t usedPSRAM = totalPSRAM - freePSRAM;

    size_t DRAM_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t PSRAM_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    float dramUsagePercentage = (float)usedDRAM / totalDRAM * 100;
    float psramUsagePercentage = (float)usedPSRAM / totalPSRAM * 100;

    ESP_LOGI(TAG, "DRAM Total: %zu bytes, Used: %zu bytes, Free: %zu bytes,  DRAM_Largest_block: %zu bytes", totalDRAM, usedDRAM, freeDRAM, DRAM_largest_block);
    ESP_LOGI(TAG, "DRAM Used: %.2f%%", dramUsagePercentage);
    ESP_LOGI(TAG, "PSRAM Total: %zu bytes, Used: %zu bytes, Free: %zu bytes, PSRAM_Largest_block: %zu bytes", totalPSRAM, usedPSRAM, freePSRAM, PSRAM_largest_block);
    ESP_LOGI(TAG, "PSRAM Used: %.2f%%", psramUsagePercentage);
} 

// 后台加载 SD 卡上的运行时字体（任意中文都能显示）。失败就用内嵌字体。
static void font_init_task(void *arg)
{
    hal_font_init();
    vTaskDelete(NULL);
}

// 主函数
void app_main(void)
{
    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    bsp_i2c_init();  // I2C初始化
    pca9557_init();  // IO扩展芯片初始化
    bsp_lvgl_start(); // 初始化液晶屏lvgl接口
    //bsp_spiffs_mount(); // SPIFFS文件系统初始化
    bsp_codec_init(); // 音频初始化

    /* 后台尝试从 SD 卡加载运行时字体（任意中文都能显示）。
     * 放任务里跑：读 TTF 头 + 建 LRU 缓存要几百毫秒，不能卡住开机。
     * 失败也没关系 —— 继续用内嵌的 GB2312 字体。 */
    /* SD 卡运行时字体已禁用（会崩在 lv_tiny_ttf）—— 见 hal_font.c 顶部说明 */
    (void)font_init_task;



    ui_msg_init();     // 应用消息总线：队列常驻，永不删除
    ui_router_start(); // 在 LVGL 任务上抽干消息

    ui_main_page_task_start(); // 主界面任务

    ota_confirm_start(); // 启动 OTA 回滚自检任务

    while (true) {
        displayMemoryUsage();
        vTaskDelay(pdMS_TO_TICKS(5 * 1000));
    }
}

