#include "ui_wifi.h"
#include "ui_common.h"
#include "ui_main_page.h"   // ui_main_page_set_wifi_connected：右上角 WiFi 状态符号
// 事件回调要启动联网/对时任务 —— 启动点留在 UI 侧，任务体在 tasks/。
// 注意方向是 ui/ → tasks/，这不破坏分层（约束只要求 tasks/ 与 midfunc/
// 不反向依赖图形库）。
// #include "wifi_tips_task.h"  ← 该文件与其任务已删除，见 ui_wifi.h 顶部说明
// #include "wifi_time_task.h"  ← 已并入 tasks/wifi_connect_task.h
// // 网络层（event_handler / wifi_scan / wifiset_deinit）第 8 步搬到 tasks/wifi_net.c。
#include "wifi_net.h"
// wifiset_event_handler 要启动扫描任务。方向仍是 ui/ → tasks/。
#include "wifi_connect_task.h"
// 存档：列表里「已保存」的那几行要读它，忘记网络要写它。
// hal/ 这一层是 LVGL-free 的，方向仍是 ui/ → hal/。
#include "wifi_store.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "string.h"
#include <time.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static const char *TAG = "app_ui";

/******************************** 第5个图标 WiFi设置 应用程序*****************************************************************************/

lv_obj_t *wifi_scan_page;     // wifi扫描页面 obj
lv_obj_t *wifi_password_page; // wifi密码页面 obj
lv_obj_t *wifi_connect_page;  // wifi连接页面 obj
lv_obj_t *obj_scan_title;     // wifi扫描页面标题
lv_obj_t *label_wifi_scan;    // wif扫描页面 label
lv_obj_t *wifi_list;          // wifi列表  list
lv_obj_t *label_wifi_connect; // wifi连接页面label 
lv_obj_t *ta_pass_text;       // 密码输入文本框 textarea
lv_obj_t *roller_num;         // 数字roller
lv_obj_t *roller_letter_low;  // 小写字母roller
lv_obj_t *roller_letter_up;   // 大写字母roller
lv_obj_t *label_wifi_name;    // wifi名称label

lv_obj_t * date_label;//菜单页面时间显示的label
lv_obj_t * time_label;

lv_timer_t *s_datetime_timer;// 菜单页面时间显示的定时器
time_t now;                     // 菜单页面时间显示的时间戳
struct tm timeinfo;             //菜单页面时间显示的时间结构体

lv_obj_t *label_conn_status;   // 标题栏右侧的常驻状态栏（已连接 / 未连接 / 上次失败 / 上次异常）


lv_obj_t *wifi_forget_page;
lv_obj_t *label_forget_ssid;   // 覆盖层上的「忘记 <SSID>」文字

#define WIFI_ROW_MAX 16 //wifi最大显示列数
typedef struct {
    char      ssid[33];
    bool      saved;   // true = 来自存档（点一下直接用存档里的密码连）；false = 本次扫到的
    lv_obj_t *btn;     // 「忘记网络」时要按行删掉的就是它
} wifi_row_t;
static wifi_row_t s_rows[WIFI_ROW_MAX];
static int        s_row_count;

// 下面几个定义在文件后半部分，而建行的 wifi_row_add / 建标题栏的
// wifi_page_build_title 要用它们，所以先声明。
static void list_btn_cb(lv_event_t * e);
static void list_btn_long_cb(lv_event_t * e);
static void btn_backmain_cb(lv_event_t * e);
static void wifi_password_page_build(const char *wifi_name);

// 连接结果的跨会话记忆，专供状态栏使用。
// 只在 LVGL 任务上被读写（写：ui_wifi_conn_show_result；读：wifi_status_label_update），
// 因此不需要锁，也不该被 task 层直接访问 —— 结果是通过 UI_MSG_WIFI_CONN_RESULT 送过来的。
#define CONN_RESULT_NONE    (-1)   // 本次开机还没尝试过连接（与 wifi_result_t 的三个取值都不冲突）
static int  s_last_conn_result = CONN_RESULT_NONE;
static char s_conn_ssid[33];       // 最近一次连接成功的 SSID；空串表示当前没有已连接记录

static void wifi_status_label_update(void)
{
    if (label_conn_status == NULL) {
        return;   // 页面不在屏幕上（已进二级页或已回主界面）
    }
    bool connected = false;
    if (s_wifi_event_group != NULL) {
        // 10 个 tick 与 wifiset_event_handler 里的判断用同一个超时值：到了这里
        // 该置的位早就置好了，非阻塞读只是为了让两处行为一致。
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                               pdFALSE, pdFALSE, 10);
        connected = (bits & WIFI_CONNECTED_BIT) != 0;
    }

    if (connected) {
        lv_label_set_text(label_conn_status, "已连接");
    } else if (s_last_conn_result == WIFI_RESULT_FAIL) {
        lv_label_set_text(label_conn_status, "上次失败");
    } else if (s_last_conn_result == WIFI_RESULT_UNEXPECTED) {
        lv_label_set_text(label_conn_status, "上次异常");
    } else {
        lv_label_set_text(label_conn_status, "未连接");
    }
}

// 行尾标记。已连接的那一行标「已连接」，其余已保存的标「已保存」，扫到的没有标记。
// 读 s_conn_ssid 是**快照**语义：它在建行那一刻取一次，之后自动连接又连上别的
// 网络也不会回来改（标题栏的状态是实时的，以它为准）。
static const char *wifi_row_suffix(const char *ssid, bool saved)
{
    if (!saved) {
        return NULL;
    }
    if (s_conn_ssid[0] != '\0' && strcmp(ssid, s_conn_ssid) == 0) {
        return "已连接";
    }
    return "已保存";
}

// 往列表里加一行，并把行号（i + 1）写进按钮的 user_data。
// **所有建行的地方都必须走这里**，否则 user_data 与 s_rows 的对应关系就断了。
static lv_obj_t *wifi_row_add(const char *ssid, bool saved)
{
    if (wifi_list == NULL || s_row_count >= WIFI_ROW_MAX) {
        return NULL;
    }

    const char *suffix = wifi_row_suffix(ssid, saved);

    // 标记直接拼进文字里，不用第二个 label：lv_list_add_btn 的按钮内部是 flex
    // 布局，往里再塞一个右对齐 label 会被 flex 顶开，反而要跟布局打架。
    // 反正行的身份走 user_data，文字长什么样都不影响回调（见 s_rows 的说明）。
    char text[64];
    if (suffix != NULL) {
        snprintf(text, sizeof(text), "%s    %s", ssid, suffix);
    } else {
        strlcpy(text, ssid, sizeof(text));
    }

    lv_obj_t *btn = lv_list_add_btn(wifi_list, LV_SYMBOL_WIFI, text);
    int idx = s_row_count++;

    strlcpy(s_rows[idx].ssid, ssid, sizeof(s_rows[idx].ssid));
    s_rows[idx].saved = saved;
    s_rows[idx].btn   = btn;

    lv_obj_set_user_data(btn, (void *)(uintptr_t)(idx + 1));
    lv_obj_add_event_cb(btn, list_btn_cb, LV_EVENT_SHORT_CLICKED, NULL);
    if (saved) {
        // 长按弹「忘记」。LV_EVENT_LONG_PRESSED 只在列表没有正在滚动时发出
        // （lv_indev.c 里那句 scroll_obj == NULL 的判断），所以滚动列表时不会误触发。
        lv_obj_add_event_cb(btn, list_btn_long_cb, LV_EVENT_LONG_PRESSED, NULL);
    }
    return btn;
}

// 建列表本体（**只建一次**）。入口处要先把「已保存」的行填进去、扫描结果回来时
// 再追加本次扫到的，两批行必须落在同一个列表里 —— 这就是"两处入口界面一致"的落点。
static void wifi_list_create_once(void)
{
    if (wifi_list != NULL) {
        return;
    }
    wifi_list = lv_list_create(wifi_scan_page);
    // 高 170（y 40~210）而不是原来的 200：底部 210~240 让给日期/时间。
    lv_obj_set_size(wifi_list, 320, 170);
    lv_obj_align(wifi_list, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_border_width(wifi_list, 0, 0);
    lv_obj_set_style_text_font(wifi_list, &font_alipuhui20, 0);
    lv_obj_set_scrollbar_mode(wifi_list, LV_SCROLLBAR_MODE_OFF); // 隐藏wifi_list滚动条
}

// 删除一级页（wifi_scan_page）并作废挂在它上面的全部文件级句柄。
// **所有删除一级页的地方都必须走这里**：lv_obj_del 只销毁对象，
// 不会把 C 侧的句柄置空，留下任何一个悬空句柄，下一次 wifi_status_label_update()
// 或 btn_do_scan_cb 就会往已释放的内存里写 —— 这正是本次要修的 use-after-free。
// 调用方必须已持有 LVGL 锁。
static void wifi_scan_page_delete(void)
{
    if (wifi_scan_page != NULL) {
        lv_obj_del(wifi_scan_page);
        wifi_scan_page = NULL;
    }

    label_conn_status   = NULL;
    obj_scan_title      = NULL;
    label_wifi_scan     = NULL;
    wifi_list           = NULL;
    // 覆盖层挂在 wifi_scan_page 之下，上面那句 lv_obj_del 已经连带删掉了它
    wifi_forget_page    = NULL;
    label_forget_ssid   = NULL;
    s_row_count = 0;
}

// wifi事件组。**已去掉 static**：从第 7 步起 tasks/wifi_time_task.c 要等它的
// WIFI_CONNECTED_BIT，声明的归属搬到了 midfunc/app_events.h。
// 四个 WIFI_*_BIT 定义也一并搬过去了 —— 同一个位域分配不能拆散在两个文件里。
EventGroupHandle_t s_wifi_event_group = NULL;

// wifi账号队列
// **已去掉 static**：队列元素类型 wifi_account_t 与队列本身都要被 tasks/ 看到，
// 声明搬到了 midfunc/app_events.h，本处保留定义（与 s_wifi_event_group 同样处理）。
QueueHandle_t xQueueWifiAccount = NULL;

// 密码roller的遮罩显示效果
static void mask_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);

    static int16_t mask_top_id = -1;
    static int16_t mask_bottom_id = -1;

    if(code == LV_EVENT_COVER_CHECK) {
        lv_event_set_cover_res(e, LV_COVER_RES_MASKED);
    }
    else if(code == LV_EVENT_DRAW_MAIN_BEGIN) {
        /* add mask */
        const lv_font_t * font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        lv_coord_t line_space = lv_obj_get_style_text_line_space(obj, LV_PART_MAIN);
        lv_coord_t font_h = lv_font_get_line_height(font);

        lv_area_t roller_coords;
        lv_obj_get_coords(obj, &roller_coords);

        lv_area_t rect_area;
        rect_area.x1 = roller_coords.x1;
        rect_area.x2 = roller_coords.x2;
        rect_area.y1 = roller_coords.y1;
        rect_area.y2 = roller_coords.y1 + (lv_obj_get_height(obj) - font_h - line_space) / 2;

        lv_draw_mask_fade_param_t * fade_mask_top = lv_mem_buf_get(sizeof(lv_draw_mask_fade_param_t));
        lv_draw_mask_fade_init(fade_mask_top, &rect_area, LV_OPA_TRANSP, rect_area.y1, LV_OPA_COVER, rect_area.y2);
        mask_top_id = lv_draw_mask_add(fade_mask_top, NULL);

        rect_area.y1 = rect_area.y2 + font_h + line_space - 1;
        rect_area.y2 = roller_coords.y2;

        lv_draw_mask_fade_param_t * fade_mask_bottom = lv_mem_buf_get(sizeof(lv_draw_mask_fade_param_t));
        lv_draw_mask_fade_init(fade_mask_bottom, &rect_area, LV_OPA_COVER, rect_area.y1, LV_OPA_TRANSP, rect_area.y2);
        mask_bottom_id = lv_draw_mask_add(fade_mask_bottom, NULL);

    }
    else if(code == LV_EVENT_DRAW_POST_END) {
        lv_draw_mask_fade_param_t * fade_mask_top = lv_draw_mask_remove_id(mask_top_id);
        lv_draw_mask_fade_param_t * fade_mask_bottom = lv_draw_mask_remove_id(mask_bottom_id);
        lv_draw_mask_free_param(fade_mask_top);
        lv_draw_mask_free_param(fade_mask_bottom);
        lv_mem_buf_release(fade_mask_top);
        lv_mem_buf_release(fade_mask_bottom);
        mask_top_id = -1;
        mask_bottom_id = -1;
    }
}

// 数字键 处理函数
static void btn_num_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Btn-Num Clicked");
        char buf[2]; // 接收roller的值
        lv_roller_get_selected_str(roller_num, buf, sizeof(buf));
        lv_textarea_add_text(ta_pass_text, buf);
    }
}

// 小写字母确认键 处理函数
static void btn_letter_low_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Btn-Letter-Low Clicked");
        char buf[2]; // 接收roller的值
        lv_roller_get_selected_str(roller_letter_low, buf, sizeof(buf));
        lv_textarea_add_text(ta_pass_text, buf);
    }
}

// 大写字母确认键 处理函数
static void btn_letter_up_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Btn-Letter-Up Clicked");
        char buf[2]; // 接收roller的值
        lv_roller_get_selected_str(roller_letter_up, buf, sizeof(buf));
        lv_textarea_add_text(ta_pass_text, buf);
    }
}



static void wifi_page_build_title(void)
{
    obj_scan_title = lv_obj_create(wifi_scan_page);
    lv_obj_set_size(obj_scan_title, 320, 40);
    lv_obj_set_style_pad_all(obj_scan_title, 0, 0);
    lv_obj_align(obj_scan_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(obj_scan_title, lv_color_hex(0x008b8b), 0);
    lv_obj_set_scrollbar_mode(obj_scan_title, LV_SCROLLBAR_MODE_OFF);

    // 返回按钮
    lv_obj_t *btn_back = lv_btn_create(obj_scan_title);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_size(btn_back, 60, 30);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_set_style_pad_all(btn_back, 0, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back, btn_backmain_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(label_back, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_back, lv_color_hex(0xffffff), 0);
    lv_obj_align(label_back, LV_ALIGN_CENTER, -10, 0);

    // 标题（扫描结果回来时由 ui_wifi_ap_list_build 改成 "N WLAN"）
    label_wifi_scan = lv_label_create(obj_scan_title);
    lv_label_set_text(label_wifi_scan, "WLAN扫描中...");
    lv_obj_set_style_text_color(label_wifi_scan, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label_wifi_scan, &font_alipuhui20, 0);
    lv_obj_align(label_wifi_scan, LV_ALIGN_CENTER, 0, 0);

    // 右侧的连接状态栏。**从页面上改挂到这里**：它原来占着页面顶部，
    // 而列表页的顶部就是标题栏，两者位置冲突。
    label_conn_status = lv_label_create(obj_scan_title);
    lv_obj_set_style_text_font(label_conn_status, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(label_conn_status, lv_color_hex(0xffffff), 0);
    lv_obj_align(label_conn_status, LV_ALIGN_RIGHT_MID, -10, 0);
    wifi_status_label_update();   // 建完立刻填一次，避免进页面时短暂空白
}


// 把存档里记着的网络填进列表（入口处调一次）。已保存的排在前面，扫描结果回来时
// 是**追加**在后面（见 ui_wifi_ap_list_build）—— 这个顺序正好就是用户要的
// "已保存的在上、本次扫到的在下，两处入口看到的是同一个列表"。
static void wifi_saved_rows_build(void)
{
    wifi_list_create_once();   // 先有列表，再往里填行

    wifi_saved_t saved;
    for (int i = 0; wifi_store_get(i, &saved); i++) {
        wifi_row_add(saved.ssid, true);
    }
}

// 「忘记网络」确认覆盖层要删的那个 SSID。用文件级缓冲而不是按钮的 user_data：
// user_data 里存的是行号（+1），而这一行在确认框弹出后、确认之前不会被删除，
// 但**行表会在删除后被压缩**，那时候行号已经指向别的网络了。
static char s_forget_ssid[33];

static void wifi_forget_close(void)
{
    if (wifi_forget_page != NULL) {
        lv_obj_del(wifi_forget_page);
        wifi_forget_page = NULL;
    }
    label_forget_ssid = NULL;
}

static void btn_forget_cancel_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lvgl_port_lock(0);
    wifi_forget_close();
    lvgl_port_unlock();
}

static void btn_forget_ok_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    lvgl_port_lock(0);

    wifi_store_forget(s_forget_ssid);   // 写 NVS，断电重启也不会再回来

    // 把那一行从列表和行表里一起摘掉，后面几行往前顶一格。
    for (int i = 0; i < s_row_count; i++) {
        if (strcmp(s_rows[i].ssid, s_forget_ssid) != 0) {
            continue;
        }
        if (s_rows[i].btn != NULL) {
            lv_obj_del(s_rows[i].btn);
        }
        for (int j = i + 1; j < s_row_count; j++) {
            s_rows[j - 1] = s_rows[j];
        }
        s_row_count--;
        break;
    }

    // **行号必须重打一遍**：user_data 里存的是"行号 + 1"，压缩之后原来的
    // 第 i+1 行变成了第 i 行，不重打的话点某一行会去连另一个网络。
    for (int i = 0; i < s_row_count; i++) {
        if (s_rows[i].btn != NULL) {
            lv_obj_set_user_data(s_rows[i].btn, (void *)(uintptr_t)(i + 1));
        }
    }

    // 删掉本回调所在的那棵树（覆盖层）。与 btn_connect_cb 里那句
    // lv_wifi_connect() 删掉自己所在页面的写法同源：都是在 LVGL 任务上、
    // 在事件回调里删掉包含当前按钮的那棵子树，删完不再碰任何控件。
    wifi_forget_close();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "已忘记 %s", s_forget_ssid);
}

// 弹「忘记 <SSID>?」确认框。
static void wifi_forget_show(const char *ssid)
{
    // 页面可能已经不在了（理论上不会：长按的前提是页面在屏幕上）。
    // 不判空的话这两个 lv_obj_create(NULL) 会各建一个游离 screen。
    if (wifi_scan_page == NULL) {
        return;
    }

    wifi_forget_close();   // 上一轮若没收干净，先收掉

    wifi_forget_page = lv_obj_create(wifi_scan_page);
    lv_obj_set_size(wifi_forget_page, 320, 240);
    lv_obj_align(wifi_forget_page, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(wifi_forget_page, 0, 0);
    lv_obj_set_style_border_width(wifi_forget_page, 0, 0);
    lv_obj_set_style_pad_all(wifi_forget_page, 0, 0);
    lv_obj_set_style_bg_color(wifi_forget_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wifi_forget_page, LV_OPA_70, 0);   // 压暗底下的列表
    lv_obj_set_scrollbar_mode(wifi_forget_page, LV_SCROLLBAR_MODE_OFF);

    label_forget_ssid = lv_label_create(wifi_forget_page);
    lv_label_set_text_fmt(label_forget_ssid, "忘记 %s?", ssid);
    lv_obj_set_style_text_font(label_forget_ssid, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(label_forget_ssid, lv_color_hex(0xffffff), 0);
    lv_obj_align(label_forget_ssid, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *btn_ok = lv_btn_create(wifi_forget_page);
    lv_obj_set_size(btn_ok, 90, 44);
    lv_obj_align(btn_ok, LV_ALIGN_CENTER, -60, 30);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0xd63a30), 0);
    lv_obj_add_event_cb(btn_ok, btn_forget_ok_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label_ok = lv_label_create(btn_ok);
    lv_label_set_text(label_ok, "忘记");
    lv_obj_set_style_text_font(label_ok, &font_alipuhui20, 0);
    lv_obj_center(label_ok);

    lv_obj_t *btn_cancel = lv_btn_create(wifi_forget_page);
    lv_obj_set_size(btn_cancel, 90, 44);
    lv_obj_align(btn_cancel, LV_ALIGN_CENTER, 60, 30);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x9E9E9E), 0);
    lv_obj_add_event_cb(btn_cancel, btn_forget_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(label_cancel, "取消");
    lv_obj_set_style_text_font(label_cancel, &font_alipuhui20, 0);
    lv_obj_center(label_cancel);
}

// 长按一个已保存的行 → 弹「忘记」确认框。
//
// **LVGL 的一个约定必须知道**：长按之后松手，它**仍会补发 LV_EVENT_CLICKED**
// （lv_indev.c:974-980 —— CLICKED 与 SHORT_CLICKED 都只在 scroll_obj == NULL
// 时发出，两者唯一的区别是 SHORT_CLICKED 多一道 long_pr_sent == 0 的闸门）。
// 所以行的"点击"注册的是 SHORT_CLICKED：它与长按天生互斥，不需要"长按置标志、
// 点击消费掉"那种写法（那种写法在"长按之后拖动一下"时会留下脏标志，
// 下一次点同一行就变成删除）。见 wifi_row_add。
// 另注：手指划过（滚动列表）时两者都不发，与改动前用 CLICKED 的行为一致。
static void list_btn_long_cb(lv_event_t * e)
{
    uintptr_t idx = (uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (idx == 0 || idx > (uintptr_t)s_row_count) {
        return;
    }
    const wifi_row_t *row = &s_rows[idx - 1];
    if (!row->saved) {
        return;   // 本次扫到的网络没有可删的存档
    }

    strlcpy(s_forget_ssid, row->ssid, sizeof(s_forget_ssid));
    ESP_LOGI(TAG, "长按已保存网络：%s", s_forget_ssid);
    wifi_forget_show(s_forget_ssid);
}

//会在这个界面呆着，没有退出按键，也不会显示连接成功或失败

static void lv_wifi_connect(void)
{
    if (wifi_password_page != NULL) {
        lv_obj_del(wifi_password_page); // 删除密码输入界面
        wifi_password_page = NULL;
    }
    // 上一轮的连接页若因故还在（例如失败后未收干净），先收掉再建，避免同名句柄覆盖成泄漏
    if (wifi_connect_page != NULL) {
        lv_obj_del(wifi_connect_page);
        wifi_connect_page = NULL;
    }

    // 创建一个面板对象
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_bg_opa( &style, LV_OPA_COVER );
    lv_style_set_border_width(&style, 0);
    lv_style_set_pad_all(&style, 0);
    lv_style_set_radius(&style, 0);  
    lv_style_set_width(&style, 320);  
    lv_style_set_height(&style, 240); 

    wifi_connect_page = lv_obj_create(lv_scr_act());
    lv_obj_add_style(wifi_connect_page, &style, 0);

    // 绘制label提示
    label_wifi_connect = lv_label_create(wifi_connect_page);
    lv_label_set_text(label_wifi_connect, "WLAN连接中...");
    lv_obj_set_style_text_font(label_wifi_connect, &font_alipuhui20, 0);
    lv_obj_align(label_wifi_connect, LV_ALIGN_CENTER, 0, -50);
}

// WiFi连接按钮 处理函数
static void btn_connect_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "OK Clicked");
        const char *wifi_ssid = lv_label_get_text(label_wifi_name);
        const char *wifi_password = lv_textarea_get_text(ta_pass_text);
        if(*wifi_password != '\0') // 判断是否为空字符串
        {
            // 初始化：下面两个 strlcpy 最多写满字段，显式清零让未用到的字节确定。
            wifi_account_t wifi_account = {0};
            // strcpy → strlcpy：这是代码审查第 2 条（缓冲区溢出）。
            // SSID 来自 wifi_ap_record_t.ssid（uint8_t[33]，32 字符 SSID 合法），
            // 密码来自原先没有长度上限的 textarea，两者都会写爆 32/64 字节的定长字段，
            // 溢出的字节落在栈上 wifi_account 的后面 —— 症状就是"点完 OK 过一会儿重启"。
            // 现在字段按容量截断，密码框另外限死 63 字符（见 list_btn_cb）。
            strlcpy(wifi_account.wifi_ssid, wifi_ssid, sizeof(wifi_account.wifi_ssid));
            strlcpy(wifi_account.wifi_password, wifi_password, sizeof(wifi_account.wifi_password));
            ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                    wifi_account.wifi_ssid, wifi_account.wifi_password);
            wifi_account.back_flag = 0; // 正常连接
            lv_wifi_connect(); // 显示wifi连接界面
            // 发送WiFi账号密码信息到队列
            xQueueSend(xQueueWifiAccount, &wifi_account, portMAX_DELAY); 
        }
    }
}

// 删除密码按钮
static void btn_del_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Clicked");
        lv_textarea_del_char(ta_pass_text);
    }
}

// 返回按钮
static void btn_back_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "btn_back Clicked");
        if (wifi_password_page != NULL) {
            lv_obj_del(wifi_password_page); // 删除密码输入界面
            wifi_password_page = NULL;
        }
        // 密码页没了，挂在它上面的两个句柄一并作废，否则是悬空指针
        label_wifi_name = NULL;
        ta_pass_text = NULL;
    }
}

// 进入输入密码界面
static void list_btn_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) {
        return;
    }

    // 行的身份走 user_data（存的是"行号 + 1"），**不再从按钮文字里反解 SSID** ——
    // 已保存的行文字里带着「已保存」标记，反解出来是"三教南303    已保存"这种脏串。
    uintptr_t idx = (uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (idx == 0 || idx > (uintptr_t)s_row_count) {
        ESP_LOGW(TAG, "列表行没有有效的行号，忽略本次点击");
        return;
    }
    const wifi_row_t *row = &s_rows[idx - 1];
    ESP_LOGI(TAG, "WLAN Name: %s（%s）", row->ssid, row->saved ? "已保存" : "本次扫到");

    if (!row->saved) {
        wifi_password_page_build(row->ssid);   // 本次扫到的：要密码，走密码页
        return;
    }

    // 已保存的：直接用存档里的密码连，跳过密码页
    wifi_account_t wifi_account = {0};
    strlcpy(wifi_account.wifi_ssid, row->ssid, sizeof(wifi_account.wifi_ssid));
    if (!wifi_store_get_password(row->ssid, wifi_account.wifi_password,
                                 sizeof(wifi_account.wifi_password))) {
        // 存档里没有它的密码（格式对不上会被当成空档）：退回密码页让用户重输
        ESP_LOGW(TAG, "存档里没有 %s 的密码，改走密码页", row->ssid);
        wifi_password_page_build(row->ssid);
        return;
    }
    wifi_account.back_flag = 0;
    lv_wifi_connect();      // 显示"WLAN连接中..."（它会把密码页先收掉）
    // 投递凭据。**必须走 wifi_connect_send**，不能直接 xQueueSend：
    // 这个队列在 wifi_connect 任务退出时会被删除并置 NULL，而"进页面自动扫描"
    // 期间正是"任务已退出、队列还没重建"的那几秒（app_wifi_connect 要先扫描完
    // 才重建队列），用户这时点一下已保存的行就正好踩在那个窗口上。
    wifi_connect_send(&wifi_account);
}

// 建密码输入页。
static void wifi_password_page_build(const char *wifi_name)
{
    // 上一次的密码页若还在（连点两个 AP 会走到这里），先收掉再建：
    // 直接赋值会让新对象覆盖旧句柄，旧页整棵子树留在 LVGL 里，再也删不掉了。
    if (wifi_password_page != NULL) {
        lv_obj_del(wifi_password_page);
        wifi_password_page = NULL;
    }

    // 创建密码输入页面
    wifi_password_page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wifi_password_page, 320, 240);
    lv_obj_set_style_border_width(wifi_password_page, 0, 0); // 设置边框宽度
    lv_obj_set_style_pad_all(wifi_password_page, 0, 0);  // 设置间隙
    lv_obj_set_style_radius(wifi_password_page, 0, 0); // 设置圆角

    // 创建返回按钮
    lv_obj_t *btn_back = lv_btn_create(wifi_password_page);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_size(btn_back, 60, 40);
    lv_obj_set_style_border_width(btn_back, 0, 0); // 设置边框宽度
    lv_obj_set_style_pad_all(btn_back, 0, 0);  // 设置间隙
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN); // 背景透明
    lv_obj_set_style_shadow_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN); // 阴影透明
    lv_obj_add_event_cb(btn_back, btn_back_cb, LV_EVENT_ALL, NULL); // 添加按键处理函数

    lv_obj_t *label_back = lv_label_create(btn_back);  
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);  // 按键上显示左箭头符号
    lv_obj_set_style_text_font(label_back, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_back, lv_color_hex(0x000000), 0); 
    lv_obj_align(label_back, LV_ALIGN_TOP_LEFT, 10, 10);

    // 显示选中的wifi名称
    label_wifi_name = lv_label_create(wifi_password_page);
    lv_obj_set_style_text_font(label_wifi_name, &lv_font_montserrat_20, 0);
    lv_label_set_text(label_wifi_name, wifi_name);
    lv_obj_align(label_wifi_name, LV_ALIGN_TOP_MID, 0, 10);

    // 创建密码输入框
    ta_pass_text = lv_textarea_create(wifi_password_page);
    lv_obj_set_style_text_font(ta_pass_text, &lv_font_montserrat_20, 0);
    lv_textarea_set_one_line(ta_pass_text, true);  // 一行显示
    // 密码最长 63 字符：wifi_account_t.wifi_password 是 char[64]（63 + '\0'）。
    // LVGL 的 max_length 默认就是 0（不限制），roller 键盘可以一直按，
    // 不挡住的话超长密码会被静默截断，用户只会看到"连不上"却不知道为什么。
    lv_textarea_set_max_length(ta_pass_text, 63);
    lv_textarea_set_password_mode(ta_pass_text, false); // 是否使用密码输入显示模式
    lv_textarea_set_placeholder_text(ta_pass_text, "password"); // 设置提醒词
    lv_obj_set_width(ta_pass_text, 150); // 宽度
    lv_obj_align(ta_pass_text, LV_ALIGN_TOP_LEFT, 10, 40); // 位置
    lv_obj_add_state(ta_pass_text, LV_STATE_FOCUSED); // 显示光标

    // 创建“连接按钮”
    lv_obj_t *btn_connect = lv_btn_create(wifi_password_page);
    lv_obj_align(btn_connect, LV_ALIGN_TOP_LEFT, 170, 40);
    lv_obj_set_width(btn_connect, 65); // 宽度
    lv_obj_add_event_cb(btn_connect, btn_connect_cb, LV_EVENT_ALL, NULL); // 事件处理函数

    lv_obj_t *label_ok = lv_label_create(btn_connect);
    lv_label_set_text(label_ok, "OK");
    lv_obj_set_style_text_font(label_ok, &lv_font_montserrat_20, 0);
    lv_obj_center(label_ok);

    // 创建“删除按钮”
    lv_obj_t *btn_del = lv_btn_create(wifi_password_page);
    lv_obj_align(btn_del, LV_ALIGN_TOP_LEFT, 245, 40);
    lv_obj_set_width(btn_del, 65); // 宽度
    lv_obj_add_event_cb(btn_del, btn_del_cb, LV_EVENT_ALL, NULL);  // 事件处理函数

    lv_obj_t *label_del = lv_label_create(btn_del);
    lv_label_set_text(label_del, LV_SYMBOL_BACKSPACE);
    lv_obj_set_style_text_font(label_del, &lv_font_montserrat_20, 0);
    lv_obj_center(label_del);

    // 创建roller样式
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_bg_color(&style, lv_color_black());
    lv_style_set_text_color(&style, lv_color_white());
    lv_style_set_border_width(&style, 0);
    lv_style_set_pad_all(&style, 0);
    lv_style_set_radius(&style, 0);

    // 创建"数字"roller
    const char * opts_num = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9";

    roller_num = lv_roller_create(wifi_password_page);
    lv_obj_add_style(roller_num, &style, 0);
    lv_obj_set_style_bg_opa(roller_num, LV_OPA_50, LV_PART_SELECTED);

    lv_roller_set_options(roller_num, opts_num, LV_ROLLER_MODE_INFINITE);
    lv_roller_set_visible_row_count(roller_num, 3); // 显示3行
    lv_roller_set_selected(roller_num, 5, LV_ANIM_OFF); // 默认选择
    lv_obj_set_width(roller_num, 90);
    lv_obj_set_style_text_font(roller_num, &lv_font_montserrat_20, 0);
    lv_obj_align(roller_num, LV_ALIGN_BOTTOM_LEFT, 15, -53);
    lv_obj_add_event_cb(roller_num, mask_event_cb, LV_EVENT_ALL, NULL); // 事件处理函数

    // 创建"数字"roller 的确认键
    lv_obj_t *btn_num_ok = lv_btn_create(wifi_password_page);
    lv_obj_align(btn_num_ok, LV_ALIGN_BOTTOM_LEFT, 15, -10); // 位置
    lv_obj_set_width(btn_num_ok, 90); // 宽度
    lv_obj_add_event_cb(btn_num_ok, btn_num_cb, LV_EVENT_ALL, NULL); // 事件处理函数

    lv_obj_t *label_num_ok = lv_label_create(btn_num_ok);
    lv_label_set_text(label_num_ok, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(label_num_ok, &lv_font_montserrat_20, 0);
    lv_obj_center(label_num_ok);

    // 创建"小写字母"roller
    const char * opts_letter_low = "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq\nr\ns\nt\nu\nv\nw\nx\ny\nz";

    roller_letter_low = lv_roller_create(wifi_password_page);
    lv_obj_add_style(roller_letter_low, &style, 0);
    lv_obj_set_style_bg_opa(roller_letter_low, LV_OPA_50, LV_PART_SELECTED); // 设置选中项的透明度
    lv_roller_set_options(roller_letter_low, opts_letter_low, LV_ROLLER_MODE_INFINITE); // 循环滚动模式
    lv_roller_set_visible_row_count(roller_letter_low, 3);
    lv_roller_set_selected(roller_letter_low, 15, LV_ANIM_OFF); // 
    lv_obj_set_width(roller_letter_low, 90);
    lv_obj_set_style_text_font(roller_letter_low, &lv_font_montserrat_20, 0);
    lv_obj_align(roller_letter_low, LV_ALIGN_BOTTOM_LEFT, 115, -53);
    lv_obj_add_event_cb(roller_letter_low, mask_event_cb, LV_EVENT_ALL, NULL); // 事件处理函数

    // 创建"小写字母"roller的确认键
    lv_obj_t *btn_letter_low_ok = lv_btn_create(wifi_password_page);
    lv_obj_align(btn_letter_low_ok, LV_ALIGN_BOTTOM_LEFT, 115, -10);
    lv_obj_set_width(btn_letter_low_ok, 90); // 宽度
    lv_obj_add_event_cb(btn_letter_low_ok, btn_letter_low_cb, LV_EVENT_ALL, NULL); // 事件处理函数

    lv_obj_t *label_letter_low_ok = lv_label_create(btn_letter_low_ok);
    lv_label_set_text(label_letter_low_ok, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(label_letter_low_ok, &lv_font_montserrat_20, 0);
    lv_obj_center(label_letter_low_ok);

    // 创建"大写字母"roller
    const char * opts_letter_up = "A\nB\nC\nD\nE\nF\nG\nH\nI\nJ\nK\nL\nM\nN\nO\nP\nQ\nR\nS\nT\nU\nV\nW\nX\nY\nZ";

    roller_letter_up = lv_roller_create(wifi_password_page);
    lv_obj_add_style(roller_letter_up, &style, 0);
    lv_obj_set_style_bg_opa(roller_letter_up, LV_OPA_50, LV_PART_SELECTED); // 设置选中项的透明度
    lv_roller_set_options(roller_letter_up, opts_letter_up, LV_ROLLER_MODE_INFINITE); // 循环滚动模式
    lv_roller_set_visible_row_count(roller_letter_up, 3);
    lv_roller_set_selected(roller_letter_up, 15, LV_ANIM_OFF);
    lv_obj_set_width(roller_letter_up, 90);
    lv_obj_set_style_text_font(roller_letter_up, &lv_font_montserrat_20, 0);
    lv_obj_align(roller_letter_up, LV_ALIGN_BOTTOM_LEFT, 215, -53);
    lv_obj_add_event_cb(roller_letter_up, mask_event_cb, LV_EVENT_ALL, NULL); // 事件处理函数

    // 创建"大写字母"roller的确认键
    lv_obj_t *btn_letter_up_ok = lv_btn_create(wifi_password_page);
    lv_obj_align(btn_letter_up_ok, LV_ALIGN_BOTTOM_LEFT, 215, -10);
    lv_obj_set_width(btn_letter_up_ok, 90); 
    lv_obj_add_event_cb(btn_letter_up_ok, btn_letter_up_cb, LV_EVENT_ALL, NULL); // 事件处理函数

    lv_obj_t *label_letter_up_ok = lv_label_create(btn_letter_up_ok);
    lv_label_set_text(label_letter_up_ok, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(label_letter_up_ok, &lv_font_montserrat_20, 0);
    lv_obj_center(label_letter_up_ok);

}


// 更新时间函数
void value_update_cb(lv_timer_t * timer)
{
    // 右上角的 WiFi 符号跟着真实连接状态走：连上才显示。
    // 放在最前面 —— 即使日期/时间 label 还没建好（开机自动连接可能早于解锁），
    // 这个小图标也该正确反映状态。
    ui_main_page_set_wifi_connected(wifi_is_connected());

    if (date_label == NULL || time_label == NULL) {
        return;
    }

    // 更新日期 星期 时分秒
    time(&now);
    localtime_r(&now, &timeinfo);
    lv_label_set_text_fmt(time_label, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    lv_label_set_text_fmt(date_label, "%d年%02d月%02d日", timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday);
}


// 对时成功后由 ui_router 调用：把主界面上已有的日期/时间 label 立刻刷成当前时间。
//
// **不建也不删任何控件**。这两个 label 和每秒刷新的定时器都由 lv_main_page()
// 独占创建（见 ui_main_page.c）—— 上一版这里把旧 label 删掉、而建 label 的那段
// 被注释掉了，于是每次对时成功都把主界面的时间日期擦成空白，且再也回不来。
// 现在这里只负责"对时完成了，屏幕上该显示新时间"，所有权只有一处。
//
// 主界面还没建好时（例如开机自动连接在解锁前就完成了对时）直接返回即可：
// lv_main_page() 建完 label 后会自己 value_update_cb() 一次，不会漏。
void ui_wifi_show_datetime(void)
{
    if (date_label == NULL || time_label == NULL) {
        return;   // 主界面尚未建好，或已经不在屏幕上
    }

    // value_update_cb 会改 LVGL 控件文本，必须持锁（本函数在 LVGL 任务上被调用，
    // 属于递归重入，与 lv_main_page() 那对锁同样处境）。
    lvgl_port_lock(0);
    value_update_cb(NULL);
    lvgl_port_unlock();
}

// 返回主界面按钮事件处理函数
static void btn_backmain_cb(lv_event_t * e)
{
    ESP_LOGI(TAG, "btn_backmain Clicked");

    lvgl_port_lock(0);
    wifi_scan_page_delete();
    lvgl_port_unlock();

    if (xQueueWifiAccount != NULL) {
        wifi_account_t wifi_account = {0};
        wifi_account.back_flag = 1;
        xQueueSend(xQueueWifiAccount, &wifi_account, portMAX_DELAY);
    }
    icon_flag = 0;
}

void ui_wifi_ap_list_build(const wifi_ap_record_t *ap_info, uint16_t ap_number)
{
    if (wifi_scan_page == NULL) {
        ESP_LOGI(TAG, "扫描结果到达时 WiFi 页面已关闭，丢弃本次结果");
        return;
    }

    lvgl_port_lock(0);
    // 修改标题
    if (label_wifi_scan != NULL) {
        lv_label_set_text_fmt(label_wifi_scan, "%d WIFI", ap_number);
    }
    wifi_list_create_once();

    // 显示wifi信息
    for (int i = 0; i < ap_number; i++) {
        ESP_LOGI(TAG, "SSID \t\t%s", ap_info[i].ssid);  // 终端输出wifi名称
        ESP_LOGI(TAG, "RSSI \t\t%d", ap_info[i].rssi);  // 终端输出wifi信号质量

        const char *ssid = (const char *)ap_info[i].ssid;

        bool dup = false;
        for (int j = 0; j < s_row_count; j++) {
            if (strcmp(s_rows[j].ssid, ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        wifi_row_add(ssid, false);
    }

    lvgl_port_unlock();
}
// （已删除 ui_wifi_tips_show / ui_wifi_tips_close）
// 这两个函数只服务于"WiFi 提示页"流程：由 wifiset_tips_task 发
// UI_MSG_WIFI_TIPS_SHOW / CLOSE 触发。而那个任务的创建点 ui_wifi_tips_task_start()
// 全树零调用 —— 整条流程从未跑过，属于死代码。
// 它们内部调用的 wifi_status_label_update() / wifi_scan_page_delete() 另有调用点，保留。

// 进入WIFI设置应用
void wifiset_event_handler(lv_event_t * e)
{  
    // 创建一个界面对象
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_radius(&style, 10);  
    lv_style_set_bg_opa( &style, LV_OPA_COVER );
    lv_style_set_bg_color(&style, lv_color_hex(0xFFFFFF));
    lv_style_set_border_width(&style, 0);
    lv_style_set_pad_all(&style, 0);
    lv_style_set_width(&style, 320);  
    lv_style_set_height(&style, 240); 

    wifi_scan_page = lv_obj_create(lv_scr_act());
    lv_obj_add_style(wifi_scan_page, &style, 0);

    icon_flag = 5;

    wifi_page_build_title();
    wifi_saved_rows_build();

    ui_wifi_app_connect_task_start();
}

// 连接结果页的渲染函数。它在连接页上显示「WLAN 连接成功/失败/异常」。
void ui_wifi_conn_show_result(wifi_result_t result, const char *ssid)
{
    const char *text;

    switch (result) {
    case WIFI_RESULT_OK:    text = "WLAN 连接成功"; break;
    case WIFI_RESULT_FAIL:  text = "WLAN 连接失败"; break;
    default:                text = "WLAN 连接异常"; break;
    }

    lvgl_port_lock(0);

    // 连接页可能已经不在了（用户在这 1 秒里按了返回），判空防 use-after-free
    if (label_wifi_connect != NULL) {
        lv_label_set_text(label_wifi_connect, text);
    }

    // 记下本次结果，供下一次进入一级页时在状态栏上显示。
    // 这是"一级页展示是否连接成功"的数据来源 —— 结果的生命周期必须跨会话，
    // 不能只寄存在那个 1 秒后就被删掉的连接页里。
    s_last_conn_result = (int)result;
    if (result == WIFI_RESULT_OK && ssid != NULL && ssid[0] != '\0') {
        strlcpy(s_conn_ssid, ssid, sizeof(s_conn_ssid));
    } else if (result != WIFI_RESULT_OK) {
        s_conn_ssid[0] = '\0';   // 失败/异常时清掉上一次的成功记录，免得状态栏自相矛盾
    }
    wifi_status_label_update();
    lvgl_port_unlock();
}

void ui_wifi_conn_close_pages(wifi_result_t result)
{
    lvgl_port_lock(0);
    if (wifi_connect_page != NULL) {
        lv_obj_del(wifi_connect_page); // 删除此页面
        wifi_connect_page = NULL;
    }
    label_wifi_connect = NULL;   // 它挂在连接页上，随页一起没了
    if (result != WIFI_RESULT_FAIL) {   // FAIL 分支原来只删连接页，不删扫描页
        wifi_scan_page_delete();
    }
    lvgl_port_unlock();
}
