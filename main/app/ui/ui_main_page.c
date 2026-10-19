#include "ui_main_page.h"
#include "ui_common.h"
#include "ui_attitude.h"
#include "ui_music.h"
#include "ui_sdcard.h"
#include "ui_camera.h"
#include "ui_wifi.h"
#include "ui_calendar.h"
#include "ui_lockscreen.h"   // 第 2 页那个"回到锁屏"的挂锁图标
#include "ui_weather.h"
#include "ui_ota.h"
#include "ui_gallery.h"
#include "ui_web.h"
#include "ui_xiaozhi.h"
#include "esp_lvgl_port.h"

/******************************** 主界面  ******************************/
// 本文件从此只剩渲染。任务函数 main_page_task 与 ui_main_page_task_start
// 已搬到 app/tasks/main_page_task.c —— RTOS 关注点不该和 LVGL 混在一个文件里。
//
// 主界面现在是**两页并排、可横向滑动、按页吸附**：
//
//   lv_scr_act
//     ├── s_carrier       320×240，横向可滚、吸附到页
//     │     ├── s_page[0] 第 1 页 = main_obj（六个老应用，位置尺寸一字未改）
//     │     └── s_page[1] 第 2 页（日历/锁屏/天气/系统升级）
//     ├── sylbom_label    右上角蓝牙/WiFi 状态符号
//     └── s_dot[0..1]     底部页码点
//
// 右上角符号和页码点**挂在屏幕上而不是页里**：它们是状态栏，不该跟着某一页滑走。
// 代价是它们叠在 carrier 之上，所以任何应用页（icon_in_obj，也是 lv_scr_act 的
// 子对象、创建得更晚）照样能盖住它们。
//
// 为什么用 LVGL 自己的滚动，而不是自己写"左右平移动画 + 手势判断"：
//   1. 它自己分得清"这一下是点击还是拖动"。拖动之后 find_scroll_obj 选中的是
//      carrier，松手时 LVGL 就不会再往被拖的那个按钮上派 CLICKED —— 于是
//      "滑页"永远不会顺手把应用打开。自己实现的话，这层判断得在九个图标
//      回调里各加一遍。
//   2. 惯性、吸附、边缘回弹都是现成的。

#define MAIN_PAGE_COUNT     2
#define MAIN_PAGE_W         320
#define MAIN_PAGE_H         240

// 图标网格。前六个应用的位置尺寸与加页之前**逐像素一致**：
// 三列 x = 15/120/225（间距 105），两行 y = 50/147（间距 97），按钮 80×80。
#define MAIN_ICON_SIZE      80
#define MAIN_ICON_COL_X0    15
#define MAIN_ICON_COL_DX    105
#define MAIN_ICON_ROW_Y0    50
#define MAIN_ICON_ROW_DY    97

static lv_obj_t *s_carrier;
static lv_obj_t *s_page[MAIN_PAGE_COUNT];
static lv_obj_t *s_dot[MAIN_PAGE_COUNT];

/** 右上角的 WiFi 状态符号（连上才显示）。 */
static lv_obj_t *s_wifi_symbol = NULL;

//时间显示
extern  lv_obj_t * date_label;//菜单页面时间显示的label
extern  lv_obj_t * time_label;

extern  lv_timer_t *s_datetime_timer;// 菜单页面时间显示的定时器
// 值本身（now / timeinfo）留在 ui_wifi.c 的文件级量里，由 value_update_cb 自己取，
// 这里不再跨 TU 去读写它们。

// 建一个应用图标。九个图标的骨架一模一样，只有位置、底色、图片和点击回调不同；
// 复制九遍的话，将来改圆角或者改图标尺寸要改九处。
static void main_icon_add(lv_obj_t *page, int col, int row, uint32_t color,
                          const void *src, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(page);
    lv_obj_set_size(btn, MAIN_ICON_SIZE, MAIN_ICON_SIZE);
    lv_obj_set_pos(btn, MAIN_ICON_COL_X0 + col * MAIN_ICON_COL_DX,
                        MAIN_ICON_ROW_Y0 + row * MAIN_ICON_ROW_DY);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    // pad 5 是照抄原来那份 btn_style 的，不是手滑：图标 75×75 放进 80×80 的按钮
    // 再扣掉 5 的四周间隙，正好把图片边缘裁掉一点。改成 0 会让六个老图标的观感
    // 跟着变，本次只加应用、不动它们。
    lv_obj_set_style_pad_all(btn, 5, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *img = lv_img_create(btn);
    lv_img_set_src(img, src);
    lv_obj_center(img);
}

// 建一页。两页的底色、圆角、尺寸完全一样。
static lv_obj_t *main_page_create(int index)
{
    lv_obj_t *page = lv_obj_create(s_carrier);
    lv_obj_set_size(page, MAIN_PAGE_W, MAIN_PAGE_H);
    lv_obj_set_pos(page, index * MAIN_PAGE_W, 0);
    lv_obj_set_style_radius(page, 10, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x00BFFF), 0);
    lv_obj_set_style_bg_grad_color(page, lv_color_hex(0x00BF00), 0);
    lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    // 页面自己不可滚（内容正好一屏）。这**不会**让左右滑动失效：LVGL 找不到
    // 可滚动的对象时会顺着 SCROLL_CHAIN 继续往上找，最终找到 carrier
    // （find_scroll_obj 里 "don't want to chain the scroll to the parent" 那段）。
    // 反过来，若不关掉，将来哪一页的控件多出几个像素，就会先变成"页面内部
    // 能拖歪"，而且那种错位不会被吸附拉回来。
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

// 页码点：当前页白、另一页灰。
static void main_dots_update(void)
{
    // lv_obj_get_scroll_x 返回的是"已经滚过去的距离"（内部取的是 -scroll.x），
    // 滚到第 2 页时是 +320。加半页再整除 = 四舍五入到最近的一页。
    int32_t sx = lv_obj_get_scroll_x(s_carrier);
    int cur = (int)((sx + MAIN_PAGE_W / 2) / MAIN_PAGE_W);
    if (cur < 0) cur = 0;
    if (cur >= MAIN_PAGE_COUNT) cur = MAIN_PAGE_COUNT - 1;

    for (int i = 0; i < MAIN_PAGE_COUNT; i++) {
        lv_obj_set_style_bg_color(s_dot[i],
                                  i == cur ? lv_color_hex(0xffffff) : lv_color_hex(0x555555), 0);
    }
}

static void main_scroll_cb(lv_event_t * e)
{
    main_dots_update();
}

void lv_main_page(void)
{
    lvgl_port_lock(0);

    // 原这里有一句 lv_obj_del(lckfb_logo)：开机 logo 与它的创建者
    // （ui_boot.c 的 lv_gui_start）一起被删掉之后，lckfb_logo 恒定是 NULL，
    // 而 lv_obj_del(NULL) 会一路走到 lv_obj_invalidate(NULL) →
    // _lv_obj_get_ext_draw_size(NULL) 读 obj->spec_attr 崩溃（LoadProhibited）。
    // 既然没有 logo 可删，这句直接去掉。

    // 创建主界面基本对象
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0); // 修改背景为黑色

    // ---- 载体：横向可滑、按页吸附 ----
    s_carrier = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_carrier, MAIN_PAGE_W, MAIN_PAGE_H);
    lv_obj_set_pos(s_carrier, 0, 0);
    lv_obj_set_style_radius(s_carrier, 0, 0);
    lv_obj_set_style_border_width(s_carrier, 0, 0);
    lv_obj_set_style_pad_all(s_carrier, 0, 0);
    lv_obj_set_style_bg_opa(s_carrier, LV_OPA_TRANSP, 0);   // 底色交给各页自己画
    lv_obj_set_scroll_dir(s_carrier, LV_DIR_HOR);           // 只横滑：竖着划不该把菜单拖歪
    lv_obj_set_scroll_snap_x(s_carrier, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_carrier, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_carrier, main_scroll_cb, LV_EVENT_SCROLL, NULL);

    s_page[0] = main_page_create(0);
    s_page[1] = main_page_create(1);

    main_obj = s_page[0];

    // 右上角状态符号：只保留 WiFi，且**默认隐藏**，连上之后才显示。
    // （蓝牙已整体移除，这里不再画 LV_SYMBOL_BLUETOOTH。）
    if (s_wifi_symbol != NULL) {
        lv_obj_del(s_wifi_symbol);
    }
    s_wifi_symbol = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_wifi_symbol, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_wifi_symbol, lv_color_hex(0xffffff), 0);
    lv_label_set_text(s_wifi_symbol, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi_symbol, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_flag(s_wifi_symbol, LV_OBJ_FLAG_HIDDEN);
    
    // ---- 时间/日期：控件与刷新定时器**都由主界面独占创建** ----
    // 值由 ui_wifi.c 的 value_update_cb 自己取（time / localtime_r）并格式化，
    // 对时成功后的 ui_wifi_show_datetime() 只负责刷新文字，既不建也不删控件 ——
    // 上一版的接缝在那里删掉这两个 label 之后没再建回来（建 label 那段被注释掉了），
    // 于是"对时成功后时间日期反而消失"。所有权收在一处，就不会再有半截状态。
    //
    // 原来这里还有一句 `if (main_obj == NULL) { lvgl_port_unlock(); return; }`，
    // 而 main_obj 在十几行之前就无条件赋成了 s_page[0]，永远不成立；留着反而会
    // 在读代码时给出"上面那段删了 label 之后可以不建"的错误暗示，故一并去掉。
    if (s_datetime_timer != NULL) {
        lv_timer_del(s_datetime_timer);
        s_datetime_timer = NULL;
    }
    if (date_label != NULL) {
        lv_obj_del(date_label);
        date_label = NULL;
    }
    if (time_label != NULL) {
        lv_obj_del(time_label);
        time_label = NULL;
    }

    // 显示年月日
    date_label = lv_label_create(lv_scr_act());    
    lv_obj_set_style_text_font(date_label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0xffffff), 0); 
    lv_obj_align(date_label, LV_ALIGN_TOP_LEFT, 10, 5);

    // 显示时间  小时:分钟:秒钟
    time_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(time_label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0xffffff), 0);

    value_update_cb(NULL);

    lv_obj_align_to(time_label, date_label, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    s_datetime_timer = lv_timer_create(value_update_cb, 1000, NULL);  // 每秒更新一次时间


    // ---- 第 1 页：六个常用应用 ----
    LV_IMG_DECLARE(img_att_icon);
    main_icon_add(s_page[0], 0, 0, 0x30a830, &img_att_icon, att_event_handler);

    LV_IMG_DECLARE(img_music_icon);
    main_icon_add(s_page[0], 1, 0, 0xf87c30, &img_music_icon, music_event_handler);

    LV_IMG_DECLARE(img_gallery_icon);
    main_icon_add(s_page[0], 2, 0, 0x1e7f8c, &img_gallery_icon, gallery_event_handler);

    LV_IMG_DECLARE(img_camera_icon);
    main_icon_add(s_page[0], 0, 1, 0xd8b010, &img_camera_icon, camera_event_handler);

    LV_IMG_DECLARE(img_sd_icon);
    main_icon_add(s_page[0], 1, 1, 0x008b8b, &img_sd_icon, sdcard_event_handler);

    LV_IMG_DECLARE(img_wifiset_icon);
    main_icon_add(s_page[0], 2, 1, 0xcd5c5c, &img_wifiset_icon, wifiset_event_handler);

    // ---- 第 2 页：天气 / 日历 / 网页 / 升级 / 锁屏 ----
    LV_IMG_DECLARE(img_weather_icon);
    main_icon_add(s_page[1], 0, 0, 0x2c6fb5, &img_weather_icon, weather_event_handler);

    LV_IMG_DECLARE(img_calendar_icon);
    main_icon_add(s_page[1], 1, 0, 0x2d7dd2, &img_calendar_icon, calendar_event_handler);

    // 网页文件管理（手机上管理 SD 卡）
    LV_IMG_DECLARE(img_web_icon);
    main_icon_add(s_page[1], 2, 0, 0x2e6b4f, &img_web_icon, web_event_handler);

    // 系统升级（OTA）
    LV_IMG_DECLARE(img_update_icon);
    main_icon_add(s_page[1], 0, 1, 0x3f7d4f, &img_update_icon, ota_event_handler);

    // 把锁屏压回主界面上面（放最后）
    LV_IMG_DECLARE(img_lock_icon);
    main_icon_add(s_page[1], 1, 1, 0x4a5568, &img_lock_icon, lock_event_handler);

    // 小智 AI 语音助手
    LV_IMG_DECLARE(img_xiaozhi_icon);
    main_icon_add(s_page[1], 2, 1, 0x6a4f9e, &img_xiaozhi_icon, xiaozhi_event_handler);

    // ---- 底部页码点 ----
    for (int i = 0; i < MAIN_PAGE_COUNT; i++) {
        s_dot[i] = lv_obj_create(lv_scr_act());
        lv_obj_set_size(s_dot[i], 6, 6);
        lv_obj_set_style_radius(s_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_dot[i], 0, 0);
        lv_obj_set_style_pad_all(s_dot[i], 0, 0);
        // 页码点只报位置，不吃触摸 —— 点在它上面应当照样能滑动、照样能点到底下的图标
        lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_SCROLLABLE);
        // 居中排开：n 个点整体居中，间距 12
        lv_obj_align(s_dot[i], LV_ALIGN_BOTTOM_MID,
                     i * 12 - (MAIN_PAGE_COUNT - 1) * 12 / 2, -5);
    }
    main_dots_update();

    lvgl_port_unlock();
}

/**
 * @brief 刷新右上角的 WiFi 状态符号：连上才显示，断开就隐藏。
 *        由 ui_wifi.c 的每秒定时器调用（运行在 LVGL 任务上，无需再持锁）。
 * @param on true = 已联网
 */
void ui_main_page_set_wifi_connected(bool on)
{
    if (s_wifi_symbol == NULL) {
        return;
    }
    if (on) {
        lv_obj_clear_flag(s_wifi_symbol, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_wifi_symbol, LV_OBJ_FLAG_HIDDEN);
    }
}
