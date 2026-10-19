#include <stdio.h>
#include <assert.h>
#include "mw_lvgl.h"
#include "hal_display.h"
#include "hal_touch.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
// esp_lcd_panel_draw_bitmap —— 我们自己接刷屏要直接调它
#include "esp_lcd_panel_ops.h"
// heap_caps_get_largest_free_block —— 刷屏失败时要把当时的内存状况打出来
#include "esp_heap_caps.h"

static const char *TAG = "esp32_s3_szp";

/* 字体：内嵌兜底 + 可从 SD 卡换成运行时 TTF（见 hal/hal_font.c）。 */
#include "hal_font.h"

/***********************************************************/
/****************    LCD显示屏 ↓   *************************/

// 定义液晶屏句柄
static esp_lcd_touch_handle_t tp;   // 触摸屏句柄
static lv_disp_t *disp;      // 指向液晶屏
static lv_indev_t *disp_indev = NULL; // 指向触摸屏

static void lvgl_flush_cb_checked(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *map)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(hal_display_panel_handle(),
                                              area->x1, area->y1,
                                              area->x2 + 1, area->y2 + 1,
                                              map);
    if (err != ESP_OK) {
        // 把当时的内存状况一起打出来 —— 这条日志是判断"到底差多少"的唯一依据。
        // 失败点几乎总是 SPI 驱动要不到那块私有的内部 DMA 中转缓冲
        // （spi_master.c 的 setup_dma_priv_buffer），所以最有用的是"最大连续块"。
        ESP_LOGE(TAG, "刷屏失败(%s)：本帧放弃。内部RAM free=%u largest=%u",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        lv_disp_flush_ready(drv);
    }
}

// 液晶屏初始化+添加LVGL接口
static lv_disp_t *bsp_display_lcd_init(void)
{
    /* 初始化液晶屏 */
    bsp_display_new(); // 液晶屏驱动初始化
    lcd_set_color(0xffff); // 设置整屏背景白色
    esp_lcd_panel_disp_on_off(hal_display_panel_handle(), true); // 打开液晶屏显示

    /* 液晶屏添加LVGL接口 */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = hal_display_io_handle(),
        .panel_handle = hal_display_panel_handle(),
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_DRAW_BUF_HEIGHT,   // LVGL缓存大小 
        .double_buffer = true, // 是否开启双缓存
        .hres = BSP_LCD_H_RES, // 液晶屏的宽
        .vres = BSP_LCD_V_RES, // 液晶屏的高
        .monochrome = false,  // 是否单色显示器
        /* Rotation的值必须和液晶屏初始化里面设置的 翻转 和 镜像 一样 */
        .rotation = {
            .swap_xy = true,  // 是否翻转
            .mirror_x = true, // x方向是否镜像
            .mirror_y = false, // y方向是否镜像
        },
        .flags = {
            .buff_dma = false , // 是否使用DMA 注意：dma与spiram不能同时为true
            .buff_spiram = true, // 是否使用PSRAM 注意：dma与spiram不能同时为true
        }
    };

    return lvgl_port_add_disp(&disp_cfg);
}

// 触摸屏初始化+添加LVGL接口
static lv_indev_t *bsp_display_indev_init(lv_disp_t *disp)
{
    /* 初始化触摸屏 */
    ESP_ERROR_CHECK(bsp_touch_new(&tp));
    assert(tp);

    /* 添加LVGL接口 */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = tp,
    };

    return lvgl_port_add_touch(&touch_cfg);
}

// 开发板显示初始化
void bsp_lvgl_start(void)
{
    /* 初始化LVGL */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    //LVGL的任务栈大小
    lvgl_cfg.task_stack = 6 * 1024;
    lvgl_port_init(&lvgl_cfg);

    /* 初始化液晶屏 并添加LVGL接口 */
    disp = bsp_display_lcd_init();

    if (disp != NULL && disp->driver != NULL) {
        disp->driver->flush_cb = lvgl_flush_cb_checked;
    } else {
        ESP_LOGE(TAG, "拿不到 disp->driver，刷屏失败将不会被处理");
    }

    /* 初始化触摸屏 并添加LVGL接口 */
    disp_indev = bsp_display_indev_init(disp);

    /* 让"没显式设字体"的控件也用中文字体。
     *
     * 走不了"改主题字体"这条路：esp_lvgl_port 不创建 theme，
     * lv_disp_get_theme(disp) 返回 NULL（实测过，源码里连 lv_theme_ 都没出现）。
     * 而 LV_FONT_DEFAULT 是 Kconfig 编译期常量（CONFIG_LV_FONT_DEFAULT_MONTSERRAT_14），
     * 只能是内置字体。
     *
     * 正确做法是利用**样式继承**：text_font 是可继承属性，设到屏幕和两个顶层
     * 图层上，它们的子孙对象会自动继承。于是 lv_dropdown 的弹层（挂在 top layer）
     * 也能拿到中文字体。
     *
     * 这里先设内嵌字体；稍后 hal_font_init() 如果能从 SD 卡加载 TTF，
     * 会再调一次 hal_font_reapply() 换成运行时字体（任意中文都能显示）。 */
    lvgl_port_lock(0);
    hal_font_reapply();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "已将 %s 设为屏幕/顶层图层的默认字体（靠样式继承）",
             hal_font_is_from_sd() ? "SD 字体" : "内嵌 font_alipuhui20");

    /* 打开液晶屏背光 */
    bsp_display_backlight_on();

}
