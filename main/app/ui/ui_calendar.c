#include "ui_calendar.h"
#include "ui_common.h"
#include <time.h>

/******************************** 第8个图标 日历 应用程序 ******************************/

#define CAL_COLS        7
#define CAL_ROWS        6
#define CAL_CELLS       (CAL_COLS * CAL_ROWS)   // 42 = 6 周，够任何月份

#define CAL_CELL_W      30
#define CAL_CELL_H      22

// 卡片几何。右侧贴边留 4px，左边 104px 全部让给背景图。
#define CAL_CARD_X      104
#define CAL_CARD_Y      4
#define CAL_CARD_W      212
#define CAL_CARD_H      232

// 卡片内各行基线。
#define CAL_Y_TITLE     6
#define CAL_Y_MONTH_EN  30
#define CAL_Y_WEEK_CN   54
#define CAL_Y_WEEK_EN   76
#define CAL_Y_DAYS      100


#define CAL_HL_SIZE     22      // 「今天」的高亮圆，正好一个单元格高

// 配色。
#define CAL_CARD_BG     0xfdfbf6
#define CAL_INK         0x4a3b2f
#define CAL_INK_DIM     0xb0a397
#define CAL_ACCENT      0xd94f3d

static lv_obj_t *s_title;                    // "2026 年 10 月"
static lv_obj_t *s_day[CAL_CELLS];           // 42 个日期 label，当月之外填空串
static lv_obj_t *s_today_hl;                 // 「今天」的高亮圆，压在日期数字下面

static int s_year;                           // 正在显示的年份
static int s_month;                          // 正在显示的月份 1..12

static int s_today_y, s_today_m, s_today_d;  // 进页面那一刻的「今天」


// 星期一打头，和参考图一致（一 二 三 四 五 六 日 / M T W T F S S）
static const char *const S_WEEK_CN[7] = {"一", "二", "三", "四", "五", "六", "日"};
static const char *const S_WEEK_EN[7] = {"M", "T", "W", "T", "F", "S", "S"};

static bool is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m)
{
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && is_leap(y)) return 29;
    return d[m - 1];
}

// 当月 1 号落在第几列（周一=0 … 周日=6）。
// 用 mktime 而不是自己写蔡勒公式：闰年、格里高利历的边界交给 libc，
// 手写公式在 1900 / 2100 这类世纪年上很容易错。
static int first_col(int y, int m)
{
    struct tm t = {0};
    t.tm_year  = y - 1900;
    t.tm_mon   = m - 1;
    t.tm_mday  = 1;
    t.tm_hour  = 12;        // 取正午：离 0 点和 23 点都远，绕开时区/夏令时边界
    t.tm_isdst = -1;
    if (mktime(&t) == (time_t)-1) return 0;
    return (t.tm_wday + 6) % 7;   // tm_wday 里 0=周日，这里要周一=0
}

// 把 s_year / s_month 画到卡片上。翻月时只调它，不重建控件。
//
// 「今天」的白字也在这里设：**不能只在建页面时设一次** —— 翻到别的月份再翻回来，
// 或者翻走后那格还留着白字，都会露馅。
static void calendar_render(void)
{
    if (s_title == NULL) return;    // 页面没建（或已关闭）

    lv_label_set_text_fmt(s_title, "%d 年 %d 月", s_year, s_month);

    const int total = days_in_month(s_year, s_month);
    const int start = first_col(s_year, s_month);
    const bool is_cur = (s_year == s_today_y && s_month == s_today_m);

    // 只在显示今天所在的那个月时画红圈：翻到别的月份还圈一天，会让人以为
    // 那天有什么特别。
    const int today_idx = is_cur ? (start + s_today_d - 1) : -1;

    for (int i = 0; i < CAL_CELLS; i++) {
        int day = i - start + 1;
        if (day < 1 || day > total) {
            lv_label_set_text(s_day[i], "");        // 空串而不是 "0"：不留痕迹
            continue;
        }
        lv_label_set_text_fmt(s_day[i], "%d", day);
        lv_obj_set_style_text_color(s_day[i],
                                    lv_color_hex(i == today_idx ? 0xffffff : CAL_INK), 0);
    }

    if (today_idx >= 0) {
        lv_obj_clear_flag(s_today_hl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_today_hl,
                       (today_idx % CAL_COLS) * CAL_CELL_W + (CAL_CELL_W - CAL_HL_SIZE) / 2,
                       CAL_Y_DAYS + (today_idx / CAL_COLS) * CAL_CELL_H);
    } else {
        lv_obj_add_flag(s_today_hl, LV_OBJ_FLAG_HIDDEN);
    }
}

// ‹ › 翻月。年份一起进位/退位，否则 1 月再往前会变成 0 月。
static void btn_month_cb(lv_event_t * e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);

    s_month += step;
    if (s_month < 1)  { s_month = 12; s_year--; }
    if (s_month > 12) { s_month = 1;  s_year++; }

    if (s_year < 1970) { s_year = 1970; s_month = 1; }
    if (s_year > 2099) { s_year = 2099; s_month = 12; }

    calendar_render();
}

// 返回主界面：拆画布 + 句柄置空 + 复位图标标记。
static void btn_calback_cb(lv_event_t * e)
{
    // 判空：lv_obj_del(NULL) 会一路走到 lv_obj_invalidate(NULL) 里崩掉
    if (icon_in_obj != NULL) {
        lv_obj_del(icon_in_obj);
        icon_in_obj = NULL;
    }

    s_title    = NULL;
    s_today_hl = NULL;
    for (int i = 0; i < CAL_CELLS; i++) s_day[i] = NULL;

    icon_flag = 0;
}

// 建一个 30×22 的单元格，文字居中。
// text 传空串（不是 NULL）：日期格的内容由 calendar_render() 填，
// 而这里的空串只是"先立个占位"。
static lv_obj_t *cell_create(lv_obj_t * parent, int col, int row, int y,
                             const lv_font_t * font, uint32_t color, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_width(lbl, CAL_CELL_W);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_pos(lbl, col * CAL_CELL_W, y + row * CAL_CELL_H);
    lv_label_set_text(lbl, text);
    return lbl;
}

void calendar_event_handler(lv_event_t * e)
{
    // ---- 「今天」。SNTP 没对时之前 time() 停在 1970，这里就会画 1970 年 1 月 ——
    //      与主界面那个时钟的表现一致，不另外处理。
    time_t t = time(NULL);
    struct tm tm_now;
    localtime_r(&t, &tm_now);
    s_today_y = tm_now.tm_year + 1900;
    s_today_m = tm_now.tm_mon + 1;
    s_today_d = tm_now.tm_mday;

    // 每次进页面都回到当月，而不是记住上次翻到哪儿：翻到 3 个月后再进来发现
    // 不是当月，反而要重新翻回去。
    s_year  = s_today_y;
    s_month = s_today_m;

    // ---- 全屏画布（icon_in_obj 是各应用共用的约定名，返回回调要删它）----
    icon_in_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(icon_in_obj, 320, 240);
    lv_obj_set_pos(icon_in_obj, 0, 0);
    lv_obj_set_style_radius(icon_in_obj, 10, 0);
    lv_obj_set_style_border_width(icon_in_obj, 0, 0);
    lv_obj_set_style_pad_all(icon_in_obj, 0, 0);
    lv_obj_clear_flag(icon_in_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 背景占位：天空→草地渐变
    LV_IMG_DECLARE(lockscreen_map);
    lv_obj_t *bg = lv_img_create(icon_in_obj);
    lv_img_set_src(bg, &lockscreen_map);
    lv_obj_align(bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);

    // ---- 返回按钮：不占标题栏，直接浮在背景上，把纵向空间全留给卡片 ----
    lv_obj_t *btn_back = lv_btn_create(icon_in_obj);
    lv_obj_set_size(btn_back, 44, 30);
    lv_obj_set_pos(btn_back, 4, 4);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_set_style_pad_all(btn_back, 0, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back, btn_calback_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(label_back, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_back, lv_color_hex(0xffffff), 0);
    lv_obj_align(label_back, LV_ALIGN_CENTER, -6, 0);

    // ---- 卡片 ----
    lv_obj_t *card = lv_obj_create(icon_in_obj);
    lv_obj_set_size(card, CAL_CARD_W, CAL_CARD_H);
    lv_obj_set_pos(card, CAL_CARD_X, CAL_CARD_Y);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(CAL_CARD_BG), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);          // 子控件坐标 = 相对卡片左上角
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_shadow_width(card, 10, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);

    // ---- 标题行：‹  2026 年 10 月  › ----
    s_title = lv_label_create(card);
    lv_obj_set_width(s_title, CAL_CARD_W);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_title, &font_alipuhui20, 0);   // 中文+数字同一字体
    lv_obj_set_style_text_color(s_title, lv_color_hex(CAL_INK), 0);
    lv_obj_set_pos(s_title, 0, CAL_Y_TITLE);

    // 翻月键。step 通过 user_data 传进去：LVGL 的事件回调没有别的入口
    static const struct { int step; const char *sym; lv_align_t align; int x; } NAV[2] = {
        {-1, LV_SYMBOL_LEFT,  LV_ALIGN_TOP_LEFT,   2},
        {+1, LV_SYMBOL_RIGHT, LV_ALIGN_TOP_RIGHT, -2},
    };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn = lv_btn_create(card);
        lv_obj_set_size(btn, 26, 26);
        lv_obj_align(btn, NAV[i].align, NAV[i].x, CAL_Y_TITLE - 2);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(CAL_ACCENT), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, btn_month_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)NAV[i].step);

        lv_obj_t *sym = lv_label_create(btn);
        lv_label_set_text(sym, NAV[i].sym);
        lv_obj_set_style_text_font(sym, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(sym, lv_color_hex(CAL_INK), 0);
        lv_obj_center(sym);
    }


    // ---- 两行星期表头。周六周日用浅色，和日期区的观感呼应 ----
    for (int col = 0; col < CAL_COLS; col++) {
        uint32_t c = (col >= 5) ? CAL_INK_DIM : CAL_INK;
        cell_create(card, col, 0, CAL_Y_WEEK_CN, &font_alipuhui20, c, S_WEEK_CN[col]);
        cell_create(card, col, 0, CAL_Y_WEEK_EN, &lv_font_montserrat_20, c, S_WEEK_EN[col]);
    }

    s_today_hl = lv_obj_create(card);
    lv_obj_set_size(s_today_hl, CAL_HL_SIZE, CAL_HL_SIZE);
    lv_obj_set_style_radius(s_today_hl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_today_hl, 0, 0);
    lv_obj_set_style_pad_all(s_today_hl, 0, 0);
    lv_obj_set_style_bg_opa(s_today_hl, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_today_hl, lv_color_hex(CAL_ACCENT), 0);
    lv_obj_clear_flag(s_today_hl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_today_hl, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 42 个日期格 ----
    for (int i = 0; i < CAL_CELLS; i++) {
        s_day[i] = cell_create(card, i % CAL_COLS, i / CAL_COLS, CAL_Y_DAYS,
                               &lv_font_montserrat_20, CAL_INK, "");
    }

    calendar_render();

    icon_flag = SCREEN_CALENDAR;
}

