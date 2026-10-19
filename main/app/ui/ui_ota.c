#include "ui_ota.h"
#include "ui_common.h"
#include "ui_msg.h"        // ota_found_t / ota_result_t
#include "ota_task.h"      // ui_ota_check_start / ui_ota_upgrade_start
#include "esp_app_desc.h"  // esp_app_get_description()：本机版本
#include "esp_ota_ops.h"   // esp_ota_get_running_partition()：跑在哪个槽
#include <stdio.h>
#include <string.h>

/******************************** 系统升级 应用程序 ******************************/
//
// 页面是一个四状态的简单状态机：
//
//   IDLE(可检查)  --点检查-->  CHECKING  --消息-->  LATEST(已是最新) / FOUND(发现新版)
//                                                         |
//                                                   确认框 升级/取消
//                                                         |
//   IDLE  <--失败/取消--  DOWNLOADING(进度条)  --成功--> 3 秒后自动重启
//
// 所有网络与 flash 操作都在 tasks/ota_task.c 里，本页只负责显示和转发点击。

// 页面上的控件。**退出时必须全部置空** —— 消息在 LVGL 任务上抽干，
// 用户按下返回之后画布就没了，任何一个没置空的指针都会在下一条消息到达时炸。
static lv_obj_t *s_hint;       // 底部状态文字
static lv_obj_t *s_btn;        // 主按钮（检查更新）
static lv_obj_t *s_bar;        // 进度条
static lv_obj_t *s_pct;        // 进度百分比文字
static lv_obj_t *s_mask;       // 确认框遮罩（按需创建/删除）

// 是否正在检查/下载。挡住重复点击 —— 任务侧也有 s_running 挡一层，
// 但界面这层要负责"按钮该不该变灰"。
static bool s_busy;

/* ---------------------------- 小工具 ---------------------------- */

static void set_hint(const char *text, uint32_t color)
{
    if (s_hint == NULL) {
        return;
    }
    lv_label_set_text(s_hint, text);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(color), 0);
}

// 删掉确认框。句柄置空是必须的 —— 下面几个回调都会用到它。
static void mask_close(void)
{
    if (s_mask != NULL) {
        lv_obj_del(s_mask);
        s_mask = NULL;
    }
}

static void back_cb(lv_event_t * e)
{
    mask_close();
    lv_obj_del(icon_in_obj);
    icon_in_obj = NULL;
    icon_flag = 0;

    // 全部作废。**s_busy 不清** —— 任务可能还在跑，它的结果消息马上就到，
    // 那些回调会因为句柄为空直接返回。
    s_hint = NULL;
    s_btn = NULL;
    s_bar = NULL;
    s_pct = NULL;
    s_mask = NULL;
}

/* ---------------------------- 状态切换 ---------------------------- */

static void enter_idle(const char *hint, uint32_t color)
{
    s_busy = false;
    if (s_btn != NULL) {
        // 按钮和进度条**占同一块竖直区域**，互斥显示 ——
        // 否则进度条(150..164)、百分比(168..194)会和按钮(148..188)叠在一起。
        lv_obj_clear_flag(s_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_state(s_btn, LV_STATE_DISABLED);
        lv_label_set_text(lv_obj_get_child(s_btn, 0), "检查更新");
    }
    if (s_bar != NULL) {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    }
    if (s_pct != NULL) {
        lv_obj_add_flag(s_pct, LV_OBJ_FLAG_HIDDEN);
    }
    set_hint(hint, color);
}

static void enter_checking(void)
{
    s_busy = true;
    if (s_btn != NULL) {
        lv_obj_add_state(s_btn, LV_STATE_DISABLED);
        lv_label_set_text(lv_obj_get_child(s_btn, 0), "正在检查…");
    }
    set_hint("正在连接服务器…", 0x556677);
}

/* ---------------------------- 点击处理 ---------------------------- */

static void do_check(void)
{
    if (s_busy) {
        return;
    }
    // **先切状态再启动任务**：任务一创建就可能立刻发回结果，
    // 顺序反了会让"正在检查…"覆盖掉刚到的那条消息。
    enter_checking();
    if (!ui_ota_check_start()) {
        // 任务没起来（上一次还没结束）。保持 busy 是对的 ——
        // 上一次的结果马上就到，会替我们解开这个状态。
    }
}

static void btn_check_cb(lv_event_t * e)
{
    do_check();
}

// 确认框里的"升级"
static void btn_upgrade_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    mask_close();
    if (s_busy) {
        return;
    }
    s_busy = true;
    if (s_btn != NULL) {
        // 藏起来而不是置灰：进度条要占用这块位置（见 enter_idle 的说明）
        lv_obj_add_flag(s_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_bar != NULL) {
        lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    }
    if (s_pct != NULL) {
        lv_obj_clear_flag(s_pct, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_pct, "0%");
    }
    set_hint("正在下载…不要断电", 0x8a5a00);

    if (!ui_ota_upgrade_start()) {
        // 起不来就退回可操作状态，别让用户卡在"正在升级…"
        enter_idle("升级任务启动失败，请重试", 0xb03030);
    }
}

static void btn_cancel_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    mask_close();
    enter_idle("已取消", 0x556677);
}

// 建确认框。**每次发现新版本重建一个**，而不是建好藏起来 ——
// 建一次藏起来的话，"版本号"那几个 label 的布局要跟着文字重新算，
// 反而更容易出错（本项目被 lv_obj_align_to 的快照语义坑过）。
static void mask_open(const ota_found_t *f)
{
    mask_close();

    s_mask = lv_obj_create(icon_in_obj);
    lv_obj_set_size(s_mask, 320, 240);
    lv_obj_set_pos(s_mask, 0, 0);
    lv_obj_set_style_radius(s_mask, 0, 0);
    lv_obj_set_style_border_width(s_mask, 0, 0);
    lv_obj_set_style_pad_all(s_mask, 0, 0);
    lv_obj_set_style_bg_color(s_mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_mask, LV_OPA_60, 0);
    lv_obj_clear_flag(s_mask, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_mask);
    lv_obj_set_size(card, 280, 150);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xffffff), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "发现新版本");
    lv_obj_set_style_text_font(title, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1d3f66), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    // 版本号 + 大小。**先填文字再定位**（lv_obj_align_to 是快照语义）
    char line[48];
    snprintf(line, sizeof(line), "%s   %.2f MB", f->version, f->size / 1048576.0);
    lv_obj_t *ver = lv_label_create(card);
    lv_label_set_text(ver, line);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ver, lv_color_hex(0x12314f), 0);
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *tip = lv_label_create(card);
    lv_label_set_text(tip, "升级过程不要断电");
    lv_obj_set_style_text_font(tip, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(tip, lv_color_hex(0x8a4a4a), 0);
    lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, 68);

    // 两个按钮并排
    lv_obj_t *b_cancel = lv_btn_create(card);
    lv_obj_set_size(b_cancel, 100, 34);
    lv_obj_align(b_cancel, LV_ALIGN_BOTTOM_LEFT, 4, 0);
    lv_obj_set_style_bg_color(b_cancel, lv_color_hex(0x8899aa), 0);
    lv_obj_add_event_cb(b_cancel, btn_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_cancel = lv_label_create(b_cancel);
    lv_label_set_text(l_cancel, "取消");
    lv_obj_set_style_text_font(l_cancel, &font_alipuhui20, 0);
    lv_obj_center(l_cancel);

    lv_obj_t *b_ok = lv_btn_create(card);
    lv_obj_set_size(b_ok, 100, 34);
    lv_obj_align(b_ok, LV_ALIGN_BOTTOM_RIGHT, -4, 0);
    lv_obj_set_style_bg_color(b_ok, lv_color_hex(0x2c6fb5), 0);
    lv_obj_add_event_cb(b_ok, btn_upgrade_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_ok = lv_label_create(b_ok);
    lv_label_set_text(l_ok, "立即升级");
    lv_obj_set_style_text_font(l_ok, &font_alipuhui20, 0);
    lv_obj_center(l_ok);
}

/* ---------------------------- 消息回调 ---------------------------- */

void ui_ota_found(const void *data)
{
    if (s_hint == NULL) {
        return;     // 页面已经关了
    }
    const ota_found_t *f = (const ota_found_t *)data;
    s_busy = false;
    if (s_btn != NULL) {
        lv_obj_clear_state(s_btn, LV_STATE_DISABLED);
        lv_label_set_text(lv_obj_get_child(s_btn, 0), "检查更新");
    }
    // 这里不用 enter_idle：确认框盖在上面，底部提示保持简洁即可
    set_hint("发现新版本", 0x1d7a3a);
    mask_open(f);
}

void ui_ota_latest(void)
{
    if (s_hint == NULL) {
        return;
    }
    enter_idle("已是最新版本", 0x1d7a3a);
}

void ui_ota_progress(int pct)
{
    if (s_hint == NULL) {
        return;
    }
    if (s_bar != NULL) {
        lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
    }
    if (s_pct != NULL) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(s_pct, buf);
    }
}

void ui_ota_failed(int code)
{
    if (s_hint == NULL) {
        return;
    }
    mask_close();

    const char *text;
    switch (code) {
    case OTA_NO_WIFI:        text = "请先连接 WiFi"; break;
    case OTA_FETCH_FAIL:     text = "连不上更新服务器"; break;
    case OTA_PARSE_FAIL:     text = "版本信息格式不对"; break;
    case OTA_NO_PARTITION:   text = "分区表里没有 OTA 槽"; break;
    case OTA_TOO_BIG:        text = "固件比分区还大"; break;
    case OTA_OPEN_FAIL:      text = "下载固件失败"; break;
    case OTA_NO_MEM:         text = "内存不足"; break;
    case OTA_BEGIN_FAIL:     text = "写入分区失败"; break;
    case OTA_READ_FAIL:      text = "下载中断"; break;
    case OTA_WRITE_FAIL:     text = "写 flash 失败"; break;
    case OTA_SIZE_MISMATCH:  text = "固件大小不符"; break;
    case OTA_VERIFY_FAIL:    text = "固件校验不过"; break;
    case OTA_SETBOOT_FAIL:   text = "切换启动分区失败"; break;
    case OTA_VERSION_MISMATCH: text = "固件版本与清单不符"; break;
    case OTA_BUSY:           text = "上一次还没结束"; break;
    default:                 text = "升级失败"; break;
    }
    // 失败后回到可操作状态 —— 按钮必须能再点，否则用户被困在失败页
    enter_idle(text, 0xb03030);
}

/* ---------------------------- 建页 ---------------------------- */

void ota_event_handler(lv_event_t * e)
{
    // ---- 画布 ----
    lv_obj_t *canvas = lv_obj_create(lv_scr_act());
    lv_obj_set_size(canvas, 320, 240);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_style_radius(canvas, 10, 0);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(canvas, lv_color_hex(0xf2f6fa), 0);
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
    lv_label_set_text(title, "系统升级");
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
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lb_back = lv_label_create(btn_back);
    lv_label_set_text(lb_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lb_back, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lb_back, lv_color_hex(0xffffff), 0);
    lv_obj_align(lb_back, LV_ALIGN_CENTER, -10, 0);

    // ---- 版本信息 ----
    // 版本号来自工程根的 version.txt（IDF 把它编进 esp_app_desc）。
    // **工程根没有 version.txt 时这里会是 git describe 的结果**（形如 5e969b7-dirty），
    // 那种字符串没法做版本比较 —— 这也是把它显示出来的意义：一眼就能看出对不对。
    const esp_app_desc_t *me = esp_app_get_description();

    lv_obj_t *lb_cap = lv_label_create(canvas);
    lv_label_set_text(lb_cap, "当前版本");
    lv_obj_set_style_text_font(lb_cap, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(lb_cap, lv_color_hex(0x667788), 0);
    lv_obj_align(lb_cap, LV_ALIGN_TOP_MID, 0, 50);

    lv_obj_t *lb_ver = lv_label_create(canvas);
    lv_label_set_text(lb_ver, me->version);
    lv_obj_set_style_text_font(lb_ver, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lb_ver, lv_color_hex(0x12314f), 0);
    lv_obj_align(lb_ver, LV_ALIGN_TOP_MID, 0, 74);

    // 运行分区：一眼看出现在是 ota_0 还是 ota_1 —— 升级前后对比它最直观
    char pline[48];
    snprintf(pline, sizeof(pline), "运行分区 %s",
             esp_ota_get_running_partition()->label);
    lv_obj_t *lb_part = lv_label_create(canvas);
    lv_label_set_text(lb_part, pline);
    lv_obj_set_style_text_font(lb_part, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(lb_part, lv_color_hex(0x667788), 0);
    lv_obj_align(lb_part, LV_ALIGN_TOP_MID, 0, 108);

    // ---- 进度条（默认隐藏）----
    // 放在按钮上方，位置固定 —— 出现/消失时不会挤动别的东西。
    s_bar = lv_bar_create(canvas);
    lv_obj_set_size(s_bar, 240, 14);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 150);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x2c6fb5), LV_PART_INDICATOR);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);

    s_pct = lv_label_create(canvas);
    lv_label_set_text(s_pct, "0%");
    lv_obj_set_style_text_font(s_pct, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_pct, lv_color_hex(0x2c6fb5), 0);
    lv_obj_align(s_pct, LV_ALIGN_TOP_MID, 0, 168);
    lv_obj_add_flag(s_pct, LV_OBJ_FLAG_HIDDEN);

    // ---- 检查更新按钮 ----
    // 与进度条**互斥显示**（同一块竖直区域 148..188）
    s_btn = lv_btn_create(canvas);
    lv_obj_set_size(s_btn, 150, 40);
    lv_obj_align(s_btn, LV_ALIGN_TOP_MID, 0, 148);
    lv_obj_set_style_radius(s_btn, 8, 0);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(0x2c6fb5), 0);
    lv_obj_add_event_cb(s_btn, btn_check_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lb_btn = lv_label_create(s_btn);
    lv_label_set_text(lb_btn, "检查更新");
    lv_obj_set_style_text_font(lb_btn, &font_alipuhui20, 0);
    lv_obj_center(lb_btn);

    // ---- 底部状态 ----
    s_hint = lv_label_create(canvas);
    lv_label_set_text(s_hint, "点上面的按钮检查更新");
    lv_obj_set_style_text_font(s_hint, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x556677), 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    icon_flag = SCREEN_OTA;

    // 进页面**只检查、不自动升级**。自动升级是不可接受的：
    // 用户可能刚打开这个页面，什么都没看清就开始刷固件了。
    //
    // 但 s_busy 要取**任务侧的真实状态**，不能无脑置 false：
    // 用户完全可能在下到一半时按返回、过一会儿又进来 —— 那时任务还在跑，
    // 界面必须仍然显示成"忙"。否则按钮是可点的，用户会以为上一次没开始。
    s_busy = ui_ota_busy();
    if (s_busy) {
        if (s_btn != NULL) {
            lv_obj_add_state(s_btn, LV_STATE_DISABLED);
            lv_label_set_text(lv_obj_get_child(s_btn, 0), "正在进行…");
        }
        set_hint("上一次操作还没结束", 0x8a5a00);
    }
}
