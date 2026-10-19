#include "ui_router.h"
#include "ui_msg.h"
#include "ui_common.h"
#include "ui_main_page.h"
#include "ui_wifi.h"
#include "ui_attitude.h"
#include "ui_camera.h"
#include "ui_sdcard.h"
#include "ui_music.h"
#include "ui_lockscreen.h"
#include "ui_weather.h"
#include "ui_ota.h"
#include "ui_gallery.h"
#include "ui_web.h"
#include "ui_xiaozhi.h"
#include "camera_task.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <time.h>   // UI_MSG_MAIN_PAGE_BUILD 分支里判断"是否已经对过时"

static const char *TAG = "ui_router";

// 单次抽干最多处理多少条消息。
// 上限的意义是别让 LVGL 任务在一次 lv_timer_handler 里干太久 ——
// CONFIG_ESP_TASK_WDT_TIMEOUT_S = 5，而 lv_timer_handler 期间不喂狗。
// 超出部分留到下一个周期（30ms 后），消息不会丢。
#define UI_ROUTER_DRAIN_MAX   8

static lv_timer_t *s_drain_timer;
static bool        s_draining;

// 抽干用的消息缓冲。
// **故意用 static 而不是局部变量**：sizeof(ui_msg_t) = 280 字节，放栈上并不会
// 溢出，但 LVGL 任务栈是**唯一**一条此后要承载全部页面构建路径的栈，
// 能不让它多背 280 字节就不背。代价只是 280 字节常驻 .bss。
// 抽干只在 LVGL 任务上跑，单线程访问，复用同一个缓冲是安全的；
// s_draining 再挡一层重入。
static ui_msg_t    s_msg;

static void ui_router_dispatch(const ui_msg_t *msg)
{
    switch (msg->id) {
    case UI_MSG_MAIN_PAGE_BUILD:
        // lv_main_page() 内部自带 lvgl_port_lock/unlock 对，这里是从 LVGL
        // 任务调用，属于递归重入 —— 那对锁逐字保留，不需要改。
        //
        // 发送方有两个：开机流程（main_page_task）和锁屏的上滑手势
        // （ui_lockscreen.c）。后者是在本回调之内发的，也就是说这条消息会先
        // 入队、等下一个抽干周期（30ms）才被处理 —— 30ms 的延迟肉眼看不出来。
        // 抽干本身是防重入的（s_draining），这里不会递归进 dispatch。
        lv_main_page();

        // 让已经对好的时间立刻上屏。控件和每秒刷新的定时器都由 lv_main_page()
        // 自己建（见 ui_main_page.c），所以这里**不需要**补建任何东西，只是省掉
        // 一次"最多等 1 秒才跳到新时间"的观感延迟。
        // 判断是自洽的：只在真的对过时之后刷（time() 还停在 1970 的话，主界面上
        // 那句 1970 就让它留着，多刷一次也没有意义）。
        if (main_obj != NULL && time(NULL) > 1600000000) {
            ui_wifi_show_datetime();
        }
        break;

    case UI_MSG_LOCKSCREEN_BUILD:
        // ui_lockscreen_build() 内部**不加锁**（与 ui_att_render 同样处境：
        // 本回调运行在 lv_timer_handler 之内，LVGL 锁已由 LVGL 任务持有）。
        ui_lockscreen_build();
        break;

    // （UI_MSG_WIFI_TIPS_SHOW / CLOSE 两条已删除：唯一的发送者 wifiset_tips_task
    //   从未被创建过，这条"显示 500ms 后关页"的流程是死代码。）

    case UI_MSG_WIFI_DATETIME_SHOW:
        // 无负载：ui_wifi_show_datetime() 自己取当前时间，不依赖发送方的任何数据。
        ui_wifi_show_datetime();
        break;

    case UI_MSG_WIFI_AP_LIST:
        // .ptr 指向 wifi_connect_task.c 的文件级 static 缓冲，**所有权不转移** ——
        // 这里只读，渲染完不需要（也不能）free。见 ui_msg.h 里该消息的说明。
        ui_wifi_ap_list_build((const wifi_ap_record_t *)msg->ptr,
                              (uint16_t)msg->v.u[0]);
        break;

    case UI_MSG_WIFI_CONN_RESULT:
        // .blob 里跟着本次连接的 SSID（任务侧填的，可能为空串）。
        // UI 层要靠它在一级页状态栏上显示"已连接 <SSID>"，见 ui_wifi_conn_show_result。
        ui_wifi_conn_show_result((wifi_result_t)msg->v.i[0], (const char *)msg->blob);
        break;

    case UI_MSG_WIFI_CONN_CLOSE:
        // 注意：这两条消息成对出现，中间隔着 wifi_connect 任务里那段
        // vTaskDelay(1000)。任务侧等完 1 秒才发 CLOSE，所以抽干队列里
        // RESULT 一定先于 CLOSE 被处理（同一个队列、同一个任务，FIFO）。
        ui_wifi_conn_close_pages((wifi_result_t)msg->v.i[0]);
        break;

    case UI_MSG_CAMERA_FRAME:
        // .ptr 是 camera_fb_t*，**所有权仍在 camera_task 侧**：这里只借用
        // 帧缓冲做一次渲染，紧接着必须 release，任务才归还该帧并取下一帧。
        // 漏掉 release 的后果是摄像头画面永久定格（任务一直阻塞在信号量上）。
        ui_camera_render_frame((camera_fb_t *)msg->ptr);
        ui_camera_frame_release();
        break;

    case UI_MSG_CAMERA_CLOSE:
        ui_camera_close_canvas();
        break;

    case UI_MSG_CAMERA_CAPTURE_DONE:
        // 无锁：与其它分支一样，本回调在 lv_timer_handler() 之内运行，
        // LVGL 锁已由 LVGL 任务持有。
        // 负载是 camera_capture_result_t，只用来决定弹哪句提示。
        ui_camera_capture_result((int)msg->v.i[0]);
        break;

    case UI_MSG_WEATHER_RESULT:
        // 直接递 .blob 的地址：weather_data_t 就存在那儿，消费端只读、不改。
        // 不 memcpy 出来再传 —— 消息在本次回调期间一直有效，而 ui_weather_show()
        // 第一件事就是判"页面还在不在"，页面没了它连读都不读。
        ui_weather_show(msg->blob);
        break;

    case UI_MSG_WEATHER_FAILED:
        ui_weather_failed((int)msg->v.i[0]);
        break;

    // ---- OTA 系统升级 ----
    // 四条都无锁：本回调在 lv_timer_handler() 之内运行，LVGL 锁已由 LVGL 任务持有。
    case UI_MSG_OTA_FOUND:
        // .blob 里是 ota_found_t（版本 + 大小），UI 据此弹确认框。
        ui_ota_found(msg->blob);
        break;

    case UI_MSG_OTA_LATEST:
        ui_ota_latest();
        break;

    case UI_MSG_OTA_PROGRESS:
        // .v.i[0] = 0~100。任务侧**已经按 1% 节流过**（见 ota_task.c），
        // 这里不用再管频率 —— 队列深度只有 2，逐块发会把它打爆。
        ui_ota_progress((int)msg->v.i[0]);
        break;

    case UI_MSG_OTA_FAILED:
        // .v.i[0] = ota_result_t。分得细是为了让界面能说出"死在哪一步"。
        ui_ota_failed((int)msg->v.i[0]);
        break;

    case UI_MSG_SDCARD_MOUNT_FAILED:
        ui_sdcard_mount_failed();
        break;

    case UI_MSG_SDCARD_CLOSE:
        ui_sdcard_close_canvas();
        break;

    case UI_MSG_ATT_BUILD:
        ui_att_build();
        break;

    case UI_MSG_ATT_INIT_FAILED:
        ui_att_init_failed();
        break;

    case UI_MSG_MUSIC_TRACK_CHANGED:
        // 发送方是 esp-audio-player 的任务（不是 LVGL 任务），原来它在自己的
        // 回调里直接 lvgl_port_lock(0) 改下拉框 —— 那是与 btn_music_back_cb 的
        // 双向死锁，详见 ui_msg.h 里这条消息的说明。绕道本队列之后，
        // "改 LVGL 控件"重新回到了 LVGL 任务上。
        // 无锁：与 UI_MSG_ATT_UPDATE 同样，本回调在 lv_timer_handler() 之内运行。
        ui_music_set_current_track((int)msg->v.i[0]);
        break;

    case UI_MSG_ATT_UPDATE:
        // 无锁：本回调在 lv_timer_handler() 之内运行，而 LVGL 任务是在
        // lvgl_port_lock(0) 之内调 lv_timer_handler 的，锁已持有。
        // 原来的 att_update_cb 也是同一个处境，本来就没有 lock/unlock 对。
        ui_att_render(msg->v.att.x, msg->v.att.y, msg->v.att.z,
                      (uint8_t)msg->v.att.motion);
        break;

    case UI_MSG_ATT_CLOSE_CANVAS:
        ui_att_close_canvas();
        break;

    case UI_MSG_SDCARD_BUILD:
        // 负载是容量（GB）。这里会走 opendir/readdir 列目录 —— 注意
        // 这**不是**新引入的：解耦前 file_list_btn_cb 与 btn_sdback_cb 两个
        // 事件回调就已在 LVGL 任务上调用 list_sdcard_files 做同样的事。
        ui_sdcard_build(msg->v.u64);
        break;

    // ---- 图库：四条消息的 .ptr 负载所有权都转移给 UI ----
    case UI_MSG_GALLERY_LIST:
        ui_gallery_show_list((gallery_list_t *)msg->ptr);
        break;

    case UI_MSG_GALLERY_FRAME:
        ui_gallery_show_frame((gallery_frame_t *)msg->ptr);
        break;

    case UI_MSG_GALLERY_FAILED:
        ui_gallery_show_failed((int)msg->v.i[0]);
        break;

    case UI_MSG_GALLERY_CLOSE:
        ui_gallery_close_canvas();
        break;

    // ---- 网页文件管理：状态 + IP ----
    case UI_MSG_WEB_STATE:
        ui_web_show_state((int)msg->v.i[0], (int)msg->v.i[1], (const char *)msg->blob);
        break;

    // ---- 小智语音助手 ----
    case UI_MSG_XZ_STATE:
        ui_xiaozhi_show_state((int)msg->v.i[0]);
        break;

    case UI_MSG_XZ_STT:
        ui_xiaozhi_show_text(true, (const char *)msg->blob);
        break;

    case UI_MSG_XZ_TTS_TEXT:
        ui_xiaozhi_show_text(false, (const char *)msg->blob);
        break;

    case UI_MSG_XZ_FAILED:
        ui_xiaozhi_show_error((int)msg->v.i[0]);
        break;

    case UI_MSG_XZ_CLOSE:
        ui_xiaozhi_close_canvas();
        break;

    case UI_MSG_XZ_ACTIVATION:
        ui_xiaozhi_show_activation((const char *)msg->blob);
        break;

    case UI_MSG_XZ_DEBUG:
        ui_xiaozhi_show_debug((const char *)msg->blob);
        break;

    default:
        ESP_LOGW(TAG, "未处理的消息 id=%d", (int)msg->id);
        break;
    }
}

static void ui_router_drain_cb(lv_timer_t *timer)
{
    // 兜底补记 LVGL 任务句柄。本回调在 lv_timer_handler() 之内执行，
    // 当前任务就是 LVGL 任务。
    ui_msg_bind_ui_task();

    if (s_draining) {
        return;   // 防重入：s_msg 是共享的
    }
    s_draining = true;

    for (int n = 0; n < UI_ROUTER_DRAIN_MAX; n++) {
        if (!ui_msg_recv(&s_msg, 0)) {
            break;   // 队列空
        }
        ui_router_dispatch(&s_msg);
    }

    s_draining = false;
}

void ui_router_start(void)
{
    // lv_timer_create 会改 LVGL 的定时器链表，必须持锁。
    lvgl_port_lock(0);
    s_drain_timer = lv_timer_create(ui_router_drain_cb,
                                    CONFIG_LV_INDEV_DEF_READ_PERIOD, NULL);
    lvgl_port_unlock();

    if (s_drain_timer == NULL) {
        ESP_LOGE(TAG, "抽干定时器创建失败");
        return;
    }
    // 周期取 indev 读周期（30ms），与触摸驱动已注册的定时器同频，
    // 不额外增加 LVGL 任务的唤醒次数。
    ESP_LOGI(TAG, "抽干定时器已启动，周期 %d ms，单次上限 %d 条",
             CONFIG_LV_INDEV_DEF_READ_PERIOD, UI_ROUTER_DRAIN_MAX);
}
