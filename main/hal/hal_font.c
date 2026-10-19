/* SD 卡运行时字体总开关。
 * 关掉的原因：lv_tiny_ttf 在这个 LVGL 8.3 上会把崩溃带进 _lv_txt_get_next_line，
 * 开机 2.5 秒就重启。内嵌的 font_alipuhui20 已经有 7,114 个汉字（GB2312 全集），
 * 日常够用 —— 等以后确认 lv_tiny_ttf 稳定了再打开。 */
#define XZ_ENABLE_SD_FONT   0
#include "hal_font.h"
#include "hal_sd.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
// lvgl.h 不会自动带 tiny_ttf（它按 LV_USE_TINY_TTF 开关决定是否包含 extras），
// 这里显式引入。声明本身不受宏保护，所以即使当前编译参数里还没有
// LV_USE_TINY_TTF 也能通过语法检查；真正的实现在 lv_tiny_ttf.c 里，
// 由 CONFIG_LV_USE_TINY_TTF 控制编译。
#include "extra/libs/tiny_ttf/lv_tiny_ttf.h"
#include <stdbool.h>
#include <sys/stat.h>

static const char *TAG = "hal_font";

/** SD 卡上的字体路径（LVGL 的 POSIX 驱动盘符是 S:，根目录 /sdcard/）。 */
#define FONT_LVGL_PATH   "S:/fonts/cjk.ttf"
#define FONT_VFS_PATH    "/sdcard/fonts/cjk.ttf"

/** 字号。与内嵌字体一致（20px），布局不会变。 */
#define FONT_SIZE        20

/** 字形位图缓存条数。CJK 20px 一个字形约 200 字节，256 条约 50KB。 */
#define FONT_CACHE_SIZE  256

/** 小于这个字节数的文件直接当"不是字体"处理。 */
#define FONT_MIN_BYTES   (200 * 1024)

/** 内嵌的阿里巴巴普惠体 20px（GB2312 全集，兜底用）。 */
extern const lv_font_t font_alipuhui20;

/** 当前 UI 字体。默认内嵌；SD 字体加载成功后指向它。 */
static const lv_font_t *s_ui_font = &font_alipuhui20;

/** 从 SD 加载出来的字体对象（用于以后 lv_tiny_ttf_destroy）。 */
static lv_font_t *s_sd_font = NULL;

/**
 * @brief 取当前应该用的 UI 字体。
 * @return 字体指针（永不为 NULL）
 */
const lv_font_t *hal_font_ui(void)
{
    return s_ui_font;
}

/**
 * @brief 查询当前用的是不是 SD 卡字体。
 * @return true 正在用 SD 卡字体
 */
bool hal_font_is_from_sd(void)
{
    return s_sd_font != NULL;
}

/**
 * @brief 把当前字体应用到屏幕和两个顶层图层。
 *        text_font 是**可继承属性** —— 设在这里，子孙控件自动继承。
 *        注意 esp_lvgl_port 不创建 theme（lv_disp_get_theme 返回 NULL），
 *        所以只能走样式继承这条路。
 */
void hal_font_reapply(void)
{
    lv_obj_set_style_text_font(lv_scr_act(),   s_ui_font, 0);
    lv_obj_set_style_text_font(lv_layer_top(), s_ui_font, 0);
    lv_obj_set_style_text_font(lv_layer_sys(), s_ui_font, 0);
}

/**
 * @brief 尝试从 SD 卡加载 TTF 运行时字体。
 * @return ESP_OK 已切换；ESP_ERR_NOT_FOUND 没有字体文件；其他为失败原因
 */
esp_err_t hal_font_init(void)
{
#if !XZ_ENABLE_SD_FONT
    ESP_LOGW(TAG, "SD 字体已禁用，使用内嵌 font_alipuhui20");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    /* 1) 确保 SD 挂着 */
    if (hal_sdcard_lock(3000) != ESP_OK) {
        ESP_LOGW(TAG, "SD 卡忙，先用内嵌字体");
        return ESP_ERR_TIMEOUT;
    }
    bool mounted = bsp_sdcard_is_mounted();
    if (!mounted) {
        mounted = (bsp_sdcard_mount() == ESP_OK);
    }
    hal_sdcard_unlock();

    if (!mounted) {
        ESP_LOGW(TAG, "没有 SD 卡，继续用内嵌字体（GB2312）");
        return ESP_ERR_NOT_FOUND;
    }

    /* 2) 看看文件在不在、大小像不像个中文字体 */
    struct stat st;
    if (stat(FONT_VFS_PATH, &st) != 0) {
        ESP_LOGW(TAG, "没有 %s，继续用内嵌字体", FONT_VFS_PATH);
        return ESP_ERR_NOT_FOUND;
    }
    if ((long)st.st_size < FONT_MIN_BYTES) {
        ESP_LOGW(TAG, "%s 只有 %ld 字节，不像中文字体，忽略",
                 FONT_VFS_PATH, (long)st.st_size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 3) 建运行时字体。lv_tiny_ttf 用 stb_truetype 按需光栅化，
     *    形位图走 LRU 缓存 —— 所以第一次碰到某个字会慢一点（约 2~4ms），
     *    之后命中缓存就和点阵字体一样快。 */
    lvgl_port_lock(0);
    lv_font_t *font = lv_tiny_ttf_create_file_ex(FONT_LVGL_PATH, FONT_SIZE,
                                                 FONT_CACHE_SIZE);
    if (font == NULL) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lv_tiny_ttf_create_file_ex 失败（文件可能不是 TTF，"
                      "stb_truetype 不支持 OTF/CFF）");
        return ESP_FAIL;
    }

    s_sd_font = font;
    s_ui_font = font;
    hal_font_reapply();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "已从 SD 加载字体: %s  %d px  缓存 %d 条  (%.1f KB)",
             FONT_VFS_PATH, FONT_SIZE, FONT_CACHE_SIZE, st.st_size / 1024.0);
    ESP_LOGI(TAG, "现在任意中文都能显示（不再受 Flash 里那 7,000 字限制）");
    return ESP_OK;
}
