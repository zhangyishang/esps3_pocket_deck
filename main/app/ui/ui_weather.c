#include "ui_weather.h"
#include "ui_common.h"
// 天气抓取任务；也带来 weather_result_t / weather_data_t 的定义。
#include "weather_task.h"
#include "ui_msg.h"
#include <stdbool.h>
#include <stdio.h>      // snprintf

/******************************** 第 12 个图标 天气 应用程序 ******************************/
//
// 页面结构沿用"画布 + 40 高标题栏 + 左上返回键"那一套（与 ui_calendar.c 一致），
// 只是正文换成天气卡片，右上多一个刷新键。
//
// 数据流：本页只负责"发请求"和"显示结果"，抓取/解析/翻译全在 tasks/weather_task.c。
// 刷新按钮做的事就是再调一次 ui_weather_task_start()。

// 页面用的句柄。**都必须能变成 NULL** —— 消息是在 LVGL 任务上被抽干的，而用户完全
// 可能在"正在获取…"的时候按了返回：那时画布已经删掉，这几个指针就成了悬空的。
// 这是本项目反复踩到的那一类问题（见 ui_camera.c 的说明），所以一律先判空。
static lv_obj_t *s_city_label;
static lv_obj_t *s_temp_label;
static lv_obj_t *s_cond_label;
static lv_obj_t *s_detail_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_refresh_btn;

// 是否正在获取。挡住连点刷新，也用来决定要不要把界面切回"加载中"。
static bool s_loading;

/* ---------------------------- 返回 ---------------------------- */

static void weather_back_cb(lv_event_t * e)
{
    lv_obj_del(icon_in_obj);
    icon_in_obj = NULL;
    icon_flag = 0;

    // 句柄作废。**注意 s_loading 不清零**：抓取任务可能还在跑，它的结果消息马上就到，
    // 那时 ui_weather_show() 会因为 s_city_label == NULL 直接返回。
    // 把 s_loading 留着，反而让"还没收尾的那次抓取"在任务侧被 s_running 挡住重入。
    s_city_label = NULL;
    s_temp_label = NULL;
    s_cond_label = NULL;
    s_detail_label = NULL;
    s_hint_label = NULL;
    s_refresh_btn = NULL;
}

/* ---------------------------- 刷新 ---------------------------- */

// 进入"正在获取…"状态：清空上一次的数据，显示提示。
//
// **刻意不动刷新键的可用状态。**
// 一开始是"加载中就把刷新键置灰"，但这会引出一个死角：如果任务创建失败
// （内存不足），就再没有消息来把这个状态解开了，而能解救它的刷新键又正好是灰的。
// 现在改成不禁用 —— 连点由 s_loading 挡住（下面 weather_refresh_cb 第一句），
// 真正的并发由任务侧的 s_running 挡住。按钮永远是活的，界面就不会卡死。
static void weather_enter_loading(void)
{
    s_loading = true;

    if (s_temp_label != NULL) {
        lv_label_set_text(s_temp_label, "--");
    }
    if (s_cond_label != NULL) {
        lv_label_set_text(s_cond_label, "");
    }
    if (s_detail_label != NULL) {
        lv_label_set_text(s_detail_label, "");
    }
    if (s_hint_label != NULL) {
        lv_label_set_text(s_hint_label, "正在获取…");
        lv_obj_clear_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void weather_refresh_cb(lv_event_t * e)
{
    if (s_loading) {
        return;     // 已经在获取了，连点无效
    }

    // **先切状态再启动任务**：任务一创建就可能立刻发回结果，
    // 顺序反了会让"加载中"覆盖掉刚到的那条结果。
    weather_enter_loading();

    // 返回 false 表示上一次抓取还没结束 —— 那一次的结果马上就到，会替我们把
    // 界面填上，所以这里**保持 s_loading 为真**（确实还在加载），什么都不用做。
    (void)ui_weather_task_start();
}

/* ---------------------------- 结果 ---------------------------- */

// 抓取成功。data 指向 ui_msg_t 的 .blob。
void ui_weather_show(const void *data)
{
    if (s_city_label == NULL) {
        return;     // 页面已经关了
    }
    const weather_data_t *d = (const weather_data_t *)data;

    lv_label_set_text(s_city_label, d->city);
    lv_label_set_text(s_temp_label, d->temp);
    lv_label_set_text(s_cond_label, d->cond);

    // 湿度和风速拼一行。用 snprintf 而不是两次 lv_label_set_text_fmt：
    // 后者会把整行重设两遍，中间那一瞬间会闪。
    char line[48];
    snprintf(line, sizeof(line), "湿度 %s    风速 %s", d->humid, d->wind);
    lv_label_set_text(s_detail_label, line);

    if (s_hint_label != NULL) {
        lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    }
    s_loading = false;
}

// 抓取失败。result 是 weather_result_t。
void ui_weather_failed(int result)
{
    if (s_city_label == NULL) {
        return;
    }

    const char *text;
    switch (result) {
    case WEATHER_NO_WIFI:       text = "请先连接 WiFi"; break;
    case WEATHER_CONNECT_FAIL:  text = "网络连接失败"; break;
    case WEATHER_HTTP_STATUS:   text = "服务器返回异常"; break;
    case WEATHER_PARSE_FAIL:    text = "返回数据无法识别"; break;
    default:                    text = "获取失败"; break;
    }

    if (s_hint_label != NULL) {
        lv_label_set_text(s_hint_label, text);
        lv_obj_clear_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_temp_label != NULL) {
        lv_label_set_text(s_temp_label, "--");
    }
    s_loading = false;
}

/* ---------------------------- 建页 ---------------------------- */

void weather_event_handler(lv_event_t * e)
{
    // ---- 画布：与 ui_calendar.c / ui_ota.c 同一套骨架 ----
    lv_obj_t *canvas = lv_obj_create(lv_scr_act());
    lv_obj_set_size(canvas, 320, 240);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_style_radius(canvas, 10, 0);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_COVER, 0);
    // 天空蓝到白的竖向渐变，和"天气"这个主题搭
    lv_obj_set_style_bg_color(canvas, lv_color_hex(0x4a90d9), 0);
    lv_obj_set_style_bg_grad_color(canvas, lv_color_hex(0xdce9f7), 0);
    lv_obj_set_style_bg_grad_dir(canvas, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(canvas, 0, 0);
    lv_obj_set_style_pad_all(canvas, 0, 0);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    icon_in_obj = canvas;

    // ---- 标题栏 ----
    lv_obj_t *bar = lv_obj_create(canvas);
    lv_obj_set_size(bar, 320, 40);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x2c6fb5), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "天气");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &font_alipuhui20, 0);
    lv_obj_center(title);

    lv_obj_t *btn_back = lv_btn_create(bar);
    lv_obj_set_size(btn_back, 60, 30);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_set_style_pad_all(btn_back, 0, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back, weather_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(label_back, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_back, lv_color_hex(0xffffff), 0);
    lv_obj_align(label_back, LV_ALIGN_CENTER, -10, 0);

    // 右上角刷新键。用符号而不是"刷新"两个字：标题栏只有 40 高，
    // 右边的空间还要留给可能更长的标题。
    s_refresh_btn = lv_btn_create(bar);
    lv_obj_set_size(s_refresh_btn, 60, 30);
    lv_obj_align(s_refresh_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_border_width(s_refresh_btn, 0, 0);
    lv_obj_set_style_pad_all(s_refresh_btn, 0, 0);
    lv_obj_set_style_bg_opa(s_refresh_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_refresh_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(s_refresh_btn, weather_refresh_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label_refresh = lv_label_create(s_refresh_btn);
    lv_label_set_text(label_refresh, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(label_refresh, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_refresh, lv_color_hex(0xffffff), 0);
    lv_obj_align(label_refresh, LV_ALIGN_CENTER, 10, 0);

    // ---- 正文（标题栏以下 200 像素）----

    // 城市名。先给个占位符，等结果回来再填。
    s_city_label = lv_label_create(canvas);
    lv_label_set_text(s_city_label, "定位中…");
    lv_obj_set_style_text_font(s_city_label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_city_label, lv_color_hex(0xffffff), 0);
    lv_obj_align(s_city_label, LV_ALIGN_TOP_MID, 0, 52);

    // 温度：页面上的主角，用大号字体。
    // lv_font_montserrat_24 的字表里有 U+00B0（°），所以 "+29°C" 一个 label 就够。
    s_temp_label = lv_label_create(canvas);
    lv_label_set_text(s_temp_label, "--");
    lv_obj_set_style_text_font(s_temp_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_temp_label, lv_color_hex(0x12314f), 0);
    lv_obj_align(s_temp_label, LV_ALIGN_TOP_MID, 0, 80);

    // 天气状况（中文）
    s_cond_label = lv_label_create(canvas);
    lv_label_set_text(s_cond_label, "");
    lv_obj_set_style_text_font(s_cond_label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_cond_label, lv_color_hex(0x1d4f7c), 0);
    lv_obj_align(s_cond_label, LV_ALIGN_TOP_MID, 0, 122);

    // 湿度 / 风速
    s_detail_label = lv_label_create(canvas);
    lv_label_set_text(s_detail_label, "");
    lv_obj_set_style_text_font(s_detail_label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_detail_label, lv_color_hex(0x3a5f80), 0);
    lv_obj_align(s_detail_label, LV_ALIGN_TOP_MID, 0, 160);

    // 状态提示（正在获取… / 失败原因）。成功时藏起来。
    s_hint_label = lv_label_create(canvas);
    lv_label_set_text(s_hint_label, "");
    lv_obj_set_style_text_font(s_hint_label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x8a4a4a), 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -14);

    icon_flag = SCREEN_WEATHER;

    // 进页面立刻抓一次 —— 这才是"打开就能看天气"该有的样子，
    // 而不是让用户先点一下刷新。
    //
    // 返回 false 表示上一次抓取还在跑（用户"出去又马上进来"就会这样）。
    // 那种情况下保持 s_loading 为真：那条迟到的结果消息会落到**这个新页面**上
    // （ui_weather_show 只判句柄非空，不管消息是哪一次请求发的）。
    weather_enter_loading();
    (void)ui_weather_task_start();
}
