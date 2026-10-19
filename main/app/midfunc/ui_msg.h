#pragma once


#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// 消息 ID。
typedef enum {
    UI_MSG_NONE = 0,

    // 搭建主界面。开机音乐播完后**不直接发这条** —— 先出锁屏（见下一条），
    // 由锁屏的上滑手势发出本条。
    UI_MSG_MAIN_PAGE_BUILD,

    // 搭建锁屏页。由 main_page_task 在开机音乐播完后发出，是这个应用启动流程的
    // 最后一站：锁屏是一层全屏透明膜，上滑才发 UI_MSG_MAIN_PAGE_BUILD 换出主界面。
    // 无负载。
    UI_MSG_LOCKSCREEN_BUILD,

    // WiFi 提示页：已连接时先显示"WLAN 已连接"，500ms 后关掉整页。
    // （UI_MSG_WIFI_TIPS_SHOW / CLOSE 已删除：唯一的发送者是 wifiset_tips_task，
    //   而它的创建点 ui_wifi_tips_task_start() 全树零调用 —— 这条流程从未跑过。）

    // 摄像头：.ptr 携带 camera_fb_t*（**所有权仍在 task 层**，UI 层只借用帧缓冲
    // 做渲染）。UI 侧渲染完必须调 ui_camera_frame_release() 通知任务可以归还 ——
    // 理由见 app/tasks/camera_task.c 里关于 fb_count=2 的说明。
    UI_MSG_CAMERA_FRAME,
    UI_MSG_CAMERA_CLOSE,

    // 相机：一张照片拍完了（成功或失败）。.v.i[0] = camera_capture_result_t。
    //
    // **没有负载指针**：照片数据全程留在 camera_task 里（编码、写卡都在那边完成），
    // UI 层只需要知道结果，弹一句提示。方向是 task → UI，和其它消息一致。
    UI_MSG_CAMERA_CAPTURE_DONE,

    // 天气：一次抓取成功。.blob 里是 weather_data_t（定义在本文件下方的类型区）。
    // 之所以走 .blob 而不是 .ptr：那个结构只有 96 字节，远小于 UI_MSG_BLOB_MAX，
    // 就地拷贝比"malloc 一块、消费端 free"少一条所有权规则、也少一次堆操作。
    UI_MSG_WEATHER_RESULT,
    // 天气：抓取失败。.v.i[0] = weather_result_t，UI 据此显示不同的提示。
    UI_MSG_WEATHER_FAILED,

    // OTA 系统升级。四条消息构成一个状态机，全部由 tasks/ota_task.c 发往 UI：
    //   FOUND     检查完成且远端更新。.blob 里是 ota_found_t，UI 据此弹确认框
    //   LATEST    检查完成，已是最新。无负载
    //   PROGRESS  下载中，.v.i[0] = 0~100。**必须节流**（每 1% 一条），
    //             队列深度只有 2，逐块发会把队列打爆、把 LVGL 任务淹没
    //   FAILED    失败，.v.i[0] = ota_result_t
    UI_MSG_OTA_FOUND,
    UI_MSG_OTA_LATEST,
    UI_MSG_OTA_PROGRESS,
    UI_MSG_OTA_FAILED,

    // SD 卡：挂载失败先报错、1 秒后关画布；挂载成功则建页面并列出根目录。
    // BUILD 的负载是容量（GB），放在 v.u64。
    UI_MSG_SDCARD_MOUNT_FAILED,
    UI_MSG_SDCARD_CLOSE,
    UI_MSG_SDCARD_BUILD,

    // WiFi 时间：联网取到 SNTP 时间后，在主界面建日期/时间两个 label 并开始每秒刷新。
    // 无负载 —— 时间由渲染侧自己取（见 ui_wifi_show_datetime 的说明）。
    UI_MSG_WIFI_DATETIME_SHOW,

    // WiFi 扫描结果列表。.ptr 指向 app/tasks/wifi_connect_task.c 里的文件级
    // static 缓冲 s_ap_info，.v.u[0] = 实际 AP 数量。
    // **所有权不转移**（与 UI_MSG_CAMERA_FRAME 不同）：缓冲不属于任何一次调用，
    // 消费端只读、不 free。之所以不能内联进 .blob，是因为
    // 5 × sizeof(wifi_ap_record_t) ≈ 360 字节 > UI_MSG_BLOB_MAX 的 256。
    UI_MSG_WIFI_AP_LIST,

    // WiFi 连接结果：.v.i[0] = wifi_result_t。
    // RESULT 只改 label 文字，CLOSE 只删页面 —— 拆成两条是因为原来中间夹着
    // wifi_connect 任务里那段不变的 vTaskDelay(1000)：任务侧等完 1 秒再发 CLOSE，
    // 就把"改文字 → 等 1 秒 → 删页"的节奏原样保住了。
    // 把 1000ms 挪进 UI 层是不行的 —— 那会让 LVGL 任务整秒不跑 lv_timer_handler。
    //
    // RESULT 的 .blob 里**额外带本次连接的 SSID**（SSID 最长 33 字节，放得下）。
    // 一级页顶部的状态栏要显示"已连接 <SSID>"，而连接页 1 秒后就被删掉，
    // SSID 不能只寄存在那个页面上，必须让 UI 层自己记下来。
    // 空串表示本次没有 SSID 可记（失败、异常、用户主动退出三种情况）。
    // CLOSE 不读 .blob。
    UI_MSG_WIFI_CONN_RESULT,
    UI_MSG_WIFI_CONN_CLOSE,

    // 音乐播放器：当前播放到第几首变了（一首放完自动续下一首，或用户点了
    // 上一首/下一首）。.v.i[0] = 新曲目在播放列表里的下标。
    //
    // **这条消息的存在意义是修一条死锁**：发送方是 esp-audio-player 自己的
    // 任务（_audio_player_callback 的 IDLE 分支），不是 LVGL 任务。
    // 原来它直接在回调里 lvgl_port_lock(0) 去改下拉框，构成双向等待：
    //   LVGL 任务在 btn_music_back_cb 里调 audio_player_delete()，要等 audio
    //     任务退出（最多 5 × 100ms），这期间一直持有 LVGL 锁；
    //   audio 任务在回调里等 lvgl_port_lock()，永远等不到。
    // 退出时的现象就是 audio 任务卡死、delete 用尽重试后无条件释放掉
    // event_queue 和 mp3 解码缓冲 —— 那些内存 audio 任务还在用。
    // 改走消息总线之后，"碰 LVGL 控件"这件事回到了 LVGL 任务上。
    UI_MSG_MUSIC_TRACK_CHANGED,

    // 姿态应用。四条消息里只有 UPDATE 有负载（.v.att），其余三条没有 ——
    // 发送方仍会把 .v.att 全填上（恒为 0），这样四种情况的构造代码一个形状。
    //   BUILD        建三个 label + 三个 bar（原 task_process_att 成功分支）
    //                 注意它**不再**包含 lv_timer_create：采样改由任务负责
    UI_MSG_ATT_BUILD,
    //   INIT_FAILED  显示"QMI8658传感器错误..."，1 秒后由 CLOSE_CANVAS 收尾
    UI_MSG_ATT_INIT_FAILED,
    //   UPDATE       任务每 200ms 采一次样，把 X/Y/Z 角度 + 运动状态送过来
    UI_MSG_ATT_UPDATE,
    //   CLOSE_CANVAS 删掉 icon_in_obj（失败分支与正常退出都走这条）
    UI_MSG_ATT_CLOSE_CANVAS,

    // 图库。
    //   LIST   一次目录扫描的结果。.ptr = gallery_list_t*，**所有权转移**，
    //          消费端（ui_router）用完必须 free。
    //   FRAME  一张解码好的图。.ptr = gallery_frame_t*，所有权转移，UI 用完 free。
    //          之所以不用"复用任务侧缓冲 + release 信号量"那套（相机在用）：
    //          图库是一次显示一张，不需要背压。
    //   FAILED .v.i[0] = gallery_result_t
    //   CLOSE  无负载（退出时删页面）
    UI_MSG_GALLERY_LIST,
    UI_MSG_GALLERY_FRAME,
    UI_MSG_GALLERY_FAILED,
    UI_MSG_GALLERY_CLOSE,

    // 网页文件管理。.v.i[0] = 1 启动 / 0 停止；.v.i[1] = esp_err_t；
    // .blob = 设备 IP 的点分十进制文本（未联网时为 "-"）。
    // IP 走 .blob 而不是 .v.u[1]：union 里 i[1] 与 u[1] 是同一块 4 字节。
    UI_MSG_WEB_STATE,

    // 小智语音助手。
    //   STATE    .v.i[0] = xz_state_t
    //   STT      .blob = 语音识别出的文本（用户说了什么）
    //   TTS_TEXT .blob = 助手的回复文本（逐句，用于字幕）
    //   FAILED   .v.i[0] = xz_error_t
    //   CLOSE    无负载（退出时删页面）
    UI_MSG_XZ_STATE,
    UI_MSG_XZ_STT,
    UI_MSG_XZ_TTS_TEXT,
    UI_MSG_XZ_FAILED,
    UI_MSG_XZ_CLOSE,
    // ACTIVATION: .blob = 激活码。官方云要求把 6 位验证码显示在屏幕上，
    // 用户到 xiaozhi.me 输入、绑定后，服务器才会下发 token。
    UI_MSG_XZ_ACTIVATION,
    // DEBUG: .blob = 一行诊断文字（HTTP 状态、连接错误等）。
    // 作用：不接串口也能在屏幕上看到"卡在哪一步"。
    UI_MSG_XZ_DEBUG,
} ui_msg_id_t;

// WiFi 连接结果码，发送方填进 .v.i[0]。三个值对应解耦前 wifi_connect 里
// 那个 if / else if / else 的三条分支，一一对应，不增不减。
typedef enum {
    WIFI_RESULT_OK = 0,          // 连上了（WIFI_CONNECTED_BIT）
    WIFI_RESULT_FAIL = 1,        // 重试用尽（WIFI_FAIL_BIT）
    WIFI_RESULT_UNEXPECTED = 2,  // 两个位都没等到
} wifi_result_t;

// 相机拍照结果，发送方（camera_task）填进 .v.i[0]。
// 与上面的 wifi_result_t 同样放在这里：消费端是 ui_router / ui_camera，
// 而 tasks/ 的头文件不该被 midfunc 反向包含。
typedef enum {
    CAMERA_CAPTURE_OK = 0,           // 照片已经写进 SD 卡
    CAMERA_CAPTURE_NO_SD = 1,        // 卡没插 / 挂载失败
    CAMERA_CAPTURE_ENCODE_FAIL = 2,  // RGB565 → JPEG 编码失败
    CAMERA_CAPTURE_WRITE_FAIL = 3,   // 建目录 / 打开 / 写入失败
} camera_capture_result_t;

/* ======================= 图库 ======================= */

/**
 * @brief 图库列表里单个文件的描述（名字 + 字节数）。
 */
#define GALLERY_NAME_LEN    48
typedef struct {
    char     name[GALLERY_NAME_LEN];   // 文件名（不含目录）
    uint32_t size;                     // 字节数
} gallery_item_t;

/**
 * @brief 图库一次扫描的结果，随 UI_MSG_GALLERY_LIST 的 .ptr 传送（消费端 free）。
 */
#define GALLERY_MAX_ITEMS   128
typedef struct {
    int            count;                      // 本次带回的条目数
    int            total;                      // 目录里一共多少张
    gallery_item_t items[GALLERY_MAX_ITEMS];
} gallery_list_t;

/**
 * @brief 一张解码好的图，随 UI_MSG_GALLERY_FRAME 的 .ptr 传送（消费端 free）。
 *        像素缓冲是 PSRAM 里的 RGB565。
 */
typedef struct {
    uint8_t *pixels;     // PSRAM: w * h * 2
    uint16_t w;
    uint16_t h;
} gallery_frame_t;

/**
 * @brief 图库操作的错误码，填入 .v.i[0]。
 */
typedef enum {
    GALLERY_OK = 0,
    GALLERY_NO_SD = 1,          // 卡没插 / 挂载失败
    GALLERY_EMPTY = 2,          // 目录里没有图片
    GALLERY_DECODE_FAIL = 3,    // JPEG 解码失败
    GALLERY_NO_MEM = 4,         // 分配缓冲失败
} gallery_result_t;

/* ======================= 小智语音助手 ======================= */

/**
 * @brief 小智助手的对话状态，随 UI_MSG_XZ_STATE 的 .v.i[0] 传送。
 */
typedef enum {
    XZ_STATE_IDLE = 0,          // 空闲 / 未连接
    XZ_STATE_CONNECTING = 1,    // 正在连服务端
    XZ_STATE_READY = 2,         // 已连接，可以按住说话
    XZ_STATE_RECORDING = 3,     // 正在录音上传
    XZ_STATE_THINKING = 4,      // 等回复
    XZ_STATE_SPEAKING = 5,      // 正在播回复
} xz_state_t;

/**
 * @brief 小智助手的错误码，随 UI_MSG_XZ_FAILED 的 .v.i[0] 传送。
 */
typedef enum {
    XZ_ERR_NO_WIFI = 0,         // 没联网
    XZ_ERR_NETWORK = 1,         // WebSocket 连不上
    XZ_ERR_OPUS = 2,            // Opus 初始化失败
    XZ_ERR_NO_MEM = 3,          // 分配缓冲失败
} xz_error_t;

// 天气抓取结果码，发送方（weather_task）填进 .v.i[0]。
typedef enum {
    WEATHER_OK = 0,              // 抓到了（这时走的是 UI_MSG_WEATHER_RESULT，本码不发送）
    WEATHER_NO_WIFI = 1,         // 没连上 WiFi —— 不是错误，是"还没联网"
    WEATHER_CONNECT_FAIL = 2,    // 连不上 / 超时 / 读失败
    WEATHER_HTTP_STATUS = 3,     // 连上了但状态码不是 200
    WEATHER_PARSE_FAIL = 4,      // 响应拆不出 6 个字段（格式变了或者被中间设备改了）
} weather_result_t;

// OTA 失败原因，发送方（ota_task）填进 .v.i[0]。
//
// 分得细是因为**排查时最需要的就是"死在哪一步"**：网络问题和 flash 写入问题的
// 现象都是"升级失败"，不区分的话只能重新加日志再烧一次。
typedef enum {
    OTA_OK = 0,
    OTA_NO_WIFI = 1,          // 没连上 WiFi
    OTA_FETCH_FAIL = 2,       // 拉版本清单失败（连不上/超时/非 200）
    OTA_PARSE_FAIL = 3,       // 清单格式不对
    OTA_NO_PARTITION = 4,     // 分区表里没有可用的 OTA 槽
    OTA_TOO_BIG = 5,          // 镜像比分区大
    OTA_OPEN_FAIL = 6,        // 打开固件下载流失败
    OTA_BEGIN_FAIL = 7,       // esp_ota_begin 失败
    OTA_READ_FAIL = 8,        // 下载中途读失败
    OTA_WRITE_FAIL = 9,       // esp_ota_write 失败
    OTA_SIZE_MISMATCH = 10,   // 收到的字节数与清单声明不符
    OTA_VERIFY_FAIL = 11,     // esp_ota_end 校验不过（镜像坏了）
    OTA_SETBOOT_FAIL = 12,    // esp_ota_set_boot_partition 失败
    OTA_BUSY = 13,            // 上一次还没结束
    OTA_NO_MEM = 14,          // 分配写入缓冲失败
    OTA_VERSION_MISMATCH = 15,// 写进去的固件版本与清单不符（见 ota_task.c 的说明）
} ota_result_t;

// 随 UI_MSG_OTA_FOUND 的 .blob 传送。24+4 = 28 字节。
// 只带"弹确认框需要的那两个数字"，URL、sha256 这些留在任务侧 —— UI 不需要知道。
typedef struct {
    char     version[24];    // "1.0.1"
    uint32_t size;           // 固件字节数，UI 自己格式化成 MB
} ota_found_t;

// 天气数据，随 UI_MSG_WEATHER_RESULT 的 .blob 一起送过来（就地拷贝，消费端不用 free）。
//
// 六个字段**都已经是可以直接显示的字符串**：状况翻成了中文、风速剥掉了箭头。
// 也就是说"翻译"和"清洗"在任务侧做完，UI 只负责摆位置 —— 这样 UI 层不需要知道
// 数据源长什么样，将来换天气源只需要改 tasks/weather_task.c。
//
// 尺寸：40+20+12+8+12 = 92 字节，远小于 UI_MSG_BLOB_MAX(256)。
typedef struct {
    char city[40];    // "Wuhan, Hubei, CN"（wttr.in 的 %l，可能是非 ASCII 地名）
    char cond[20];    // "多云" —— 由英文状况文本映射而来，见 weather_task.c 的映射表
    // "+29°C" —— **原样保留，不拆成数字和单位**。
    // 本来打算拆开（大号数字 + 小号单位），但 lv_font_montserrat_24 的字表里
    // 就有 U+00B0（°），一个 label 用同一种字体就能显示，拆开纯属多余。
    char temp[12];
    char humid[8];    // "31%"
    char wind[12];    // "6km/h" —— 已经剥掉原始数据开头的方向箭头
} weather_data_t;

// 内联 POD 负载的可用字节数。sizeof(ui_msg_t) = 24 + 本值 = 280。
//
// 这个值是被**实际负载量出来**的，不是拍的：全项目最大的内联负载是
// ui_wifi.c 的 wifi_account_t（33+64+1 = 98 字节）。定 256 留了 2.6 倍余量。
//
// 为什么不按"够大就好"定成 2KB：每个**发送者在栈上放一个 ui_msg_t**，
// 而任务栈只有 3072~4096（main_page_task 4KB、ui_wifi 的 3~4KB）。
// 2KB 的消息会吃掉半个栈 —— 这类溢出编译期不报、符号比对看不见，
// 症状是随机跑飞。队列也一样：深度 2 时 1792 要占 3.6KB 内部 RAM。
//
// 规则：**超过本值的负载一律走 .ptr 堆负载通道**，所有权随消息转移。
// 已知唯一的超标负载是 ui_sdcard.c 的 path_now/path_back（各 512 字节）。
#define UI_MSG_BLOB_MAX  256

// 消息为 POD，可直接按值拷贝进队列。
//
// 三种负载通道，按场景选：
//   .v     少量标量（角度、结果码、状态）
//   .ptr   堆负载指针，**所有权随消息转移给消费端**，由消费端 free
//   .blob  大块 POD（列表、数组），就地拷贝，消费端不需要 free
typedef struct {
    ui_msg_id_t id;
    void       *ptr;
    union {
        int32_t  i[2];
        uint32_t u[2];
        uint64_t u64;   // SD 卡容量这类 64 位量；各成员都不超过 16 字节，union 不涨
        float    f[4];
        char     s[16];
        // 姿态采样：三轴角度（已四舍五入的整数）+ QMI8658 的运动状态位。
        // 16 字节，与 f[4] 同宽，加进来 sizeof(ui_msg_t) 仍是 280。
        struct { int32_t x, y, z, motion; } att;
    } v;
    uint8_t blob[UI_MSG_BLOB_MAX];
} ui_msg_t;

// 建队列并记录 LVGL 任务句柄。必须在 bsp_lvgl_start() 之后、任何 send 之前调用。
void ui_msg_init(void);

// 由 ui_router 的抽干回调调用：按名字没找到 LVGL 任务时，在这里补记。
// 本函数必须运行在 LVGL 任务上。
void ui_msg_bind_ui_task(void);

// 发送。wait_ticks 为 0 表示不阻塞。
// 成功返回 true；队列满且不阻塞时返回 false 并打印警告（症状是"点了没反应"）。
bool ui_msg_send(const ui_msg_t *msg, TickType_t wait_ticks);

// 接收。wait_ticks 为 0 表示队列空就立刻返回 false。
bool ui_msg_recv(ui_msg_t *out, TickType_t wait_ticks);
