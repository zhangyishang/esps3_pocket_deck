#include "ui_camera.h"
#include "ui_common.h"
// camera_event_handler 要启动摄像头任务。
#include "camera_task.h"
// camera_capture_result_t（拍照结果码）定义在这里，与 wifi_result_t 同一处。
#include "ui_msg.h"
#include "hal_pca9557.h"
#include "hal_camera.h"
#include "esp_lvgl_port.h"
// 预览帧的本地副本要显式申请 PSRAM
#include "esp_heap_caps.h"
#include <string.h>

/******************************** 第4个图标 相机 应用程序 *****************************************************************************/

lv_obj_t * img_camera;

static lv_obj_t *s_shutter_btn;
static lv_obj_t *s_toast_label;
static lv_timer_t *s_toast_timer;

// ---------------------------------------------------------------------------
// 预览帧的本地副本
//
// ★ 为什么必须拷一份 ★
//
// 原来的写法是直接把 LVGL 的图像指针指到摄像头的帧缓冲上：
//
//     img_camera_dsc.data = frame->buf;
//     lv_img_set_src(img_camera, &img_camera_dsc);
//
// 但 **lv_img_set_src() 只是把指针记下来并标脏**（LVGL 8.3 里函数第一句就是
// lv_obj_invalidate()，真正读像素要等这次 lv_timer_handler() 后面的 refresh
// 阶段）。而 ui_router 在设完指针后**立刻**调 ui_camera_frame_release() 放行，
// camera_task 随即 esp_camera_fb_return() 归还该缓冲，下一轮 fb_get() 很可能
// 又拿回同一块，摄像头 DMA 于是开始往里面写新帧 ✗
//
// 结果就是 LVGL 一边读、摄像头一边写：读到的是"上半截新帧 + 下半截旧帧"，
// 屏幕上表现为**固定的水平撕裂接缝**（实测在距顶约 1/4 处，因为写比读快约 4 倍）。
//
// 拷到自己的缓冲后两侧彻底解耦：摄像头怎么写都无所谓，LVGL 读的是一份快照。
// 代价是每帧一次 320×240×2 = 150KB 的 memcpy（约 1~2ms，远小于一次刷屏的
// 十几毫秒），换来画面不再撕裂。
// ---------------------------------------------------------------------------
static uint8_t *s_ui_frame_buf = NULL;
#define CAMERA_FRAME_BYTES   (320 * 240 * 2)

// 提示语停留多久（毫秒）
#define CAMERA_TOAST_MS   1500

// 计时器到点：把提示藏起来并**暂停自己**。
// 刻意不在回调里 lv_timer_del(self)：在定时器自己的回调里删自己，各家实现
// 的语义都容易踩坑；暂停 + 下次重置再恢复，生命周期清楚得多。
static void toast_hide_cb(lv_timer_t *timer)
{
    if (s_toast_label != NULL) {
        lv_obj_add_flag(s_toast_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_pause(timer);
}

// 拍照结果。由 ui_router 在 LVGL 任务上调用（锁已持有，这里不再加锁）。
void ui_camera_capture_result(int result)
{
    if (s_toast_label == NULL) {
        return;     // 页面已经关了
    }

    const char *text;
    switch (result) {
    case CAMERA_CAPTURE_OK:          text = "已保存到 SD"; break;
    case CAMERA_CAPTURE_NO_SD:       text = "没有 SD 卡"; break;
    case CAMERA_CAPTURE_ENCODE_FAIL: text = "图片编码失败"; break;
    default:                         text = "写入 SD 失败"; break;
    }

    lv_label_set_text(s_toast_label, text);
    lv_obj_clear_flag(s_toast_label, LV_OBJ_FLAG_HIDDEN);

    if (s_toast_timer != NULL) {
        // 连拍时把停留时间重新计起，而不是让上一条的尾巴把新提示截断
        lv_timer_reset(s_toast_timer);
        lv_timer_resume(s_toast_timer);
    }
}

// 快门键：只置一个请求标志，真正的编码/写卡在 camera_task 里做。
static void btn_shutter_cb(lv_event_t * e)
{
    ui_camera_request_capture();
}

// 摄像头图像
lv_img_dsc_t img_camera_dsc = {
  .header.cf = LV_IMG_CF_TRUE_COLOR,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = 320,
  .header.h = 240,
  .data_size = 240*320*2,
};

// 渲染函数：把一帧显示到画布上。
//
// 关键：**先拷到本地副本，再交给 LVGL**。理由见 s_ui_frame_buf 上面那段说明 ——
// 直接指摄像头缓冲会让 LVGL 在 refresh 阶段读到正被摄像头 DMA 改写的内容，
// 表现为固定的水平撕裂接缝。
void ui_camera_render_frame(camera_fb_t *frame)
{
    // ★ 原来的写法少了这个判空：esp_camera_fb_get() 取帧失败会返回 NULL，
    //   那时 frame->buf 就是解引用空指针，必然崩溃。
    if (frame == NULL || frame->buf == NULL) {
        return;
    }
    if (frame->len < CAMERA_FRAME_BYTES) {
        // 帧没取满（驱动出错或格式不对），宁可不显示也不越界读。
        return;
    }

    if (s_ui_frame_buf == NULL) {
        // 必须显式指定 PSRAM：CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048 的规则下，
        // 超过 2048 字节的普通 malloc 虽然也会落到 PSRAM，但这里写清楚更安全，
        // 而且绝对不会误占内部 RAM（内部 RAM 已经被蓝牙/WiFi 挤得很紧）。
        s_ui_frame_buf = heap_caps_malloc(CAMERA_FRAME_BYTES,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_ui_frame_buf == NULL) {
            // 退一步：没有副本就先不显示，避免拿摄像头缓冲去撕裂。
            return;
        }
    }

    memcpy(s_ui_frame_buf, frame->buf, CAMERA_FRAME_BYTES);

    img_camera_dsc.data = s_ui_frame_buf;      // ← 指向自己的快照，不再指摄像头缓冲
    lv_img_set_src(img_camera, &img_camera_dsc);
}

// 渲染函数：删除摄像头画布。
void ui_camera_close_canvas(void)
{
    lvgl_port_lock(0);
    lv_obj_del(icon_in_obj); // 删除摄像头画布
    // 图像已经没有任何对象在引用了，副本可以还掉：
    // 退出相机页后这 150KB PSRAM 不该一直占着。
    if (s_ui_frame_buf != NULL) {
        heap_caps_free(s_ui_frame_buf);
        s_ui_frame_buf = NULL;
    }
    img_camera_dsc.data = NULL;
    lvgl_port_unlock();
}

// 返回主界面按钮事件处理函数
static void btn_camback_cb(lv_event_t * e)
{
    if (s_toast_timer != NULL) {
        lv_timer_del(s_toast_timer);
        s_toast_timer = NULL;
    }
    s_toast_label  = NULL;
    s_shutter_btn  = NULL;

    icon_flag = 0;
}

// 进入摄像头应用
void camera_event_handler(lv_event_t * e)
{
    bsp_camera_init(); // 摄像头初始化
    // 创建一个界面对象
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_radius(&style, 10);  
    lv_style_set_bg_opa( &style, LV_OPA_COVER );
    lv_style_set_bg_color(&style, lv_color_hex(0xcccccc));
    lv_style_set_border_width(&style, 0);
    lv_style_set_pad_all(&style, 0);
    lv_style_set_width(&style, 320);  
    lv_style_set_height(&style, 240); 

    icon_in_obj = lv_obj_create(lv_scr_act());
    lv_obj_add_style(icon_in_obj, &style, 0);

    img_camera = lv_img_create(icon_in_obj);
    lv_obj_set_pos(img_camera, 0, 0);
    lv_obj_set_size(img_camera, 320, 240);

    // 创建返回按钮
    lv_obj_t *btn_back = lv_btn_create(icon_in_obj);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_size(btn_back, 60, 30);
    lv_obj_set_style_border_width(btn_back, 0, 0); // 设置边框宽度
    lv_obj_set_style_pad_all(btn_back, 0, 0);  // 设置间隙
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN); // 背景透明
    lv_obj_set_style_shadow_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN); // 阴影透明
    lv_obj_add_event_cb(btn_back, btn_camback_cb, LV_EVENT_CLICKED, NULL); // 添加按键处理函数

    lv_obj_t *label_back = lv_label_create(btn_back); 
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);  // 按键上显示左箭头符号
    lv_obj_set_style_text_font(label_back, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_back, lv_color_hex(0xffffff), 0); 
    lv_obj_align(label_back, LV_ALIGN_CENTER, -10, 0);

    /* ---- 快门键：底部中间，浮在预览画面上 ---- */
    // 半透明白圈 + 白边，像相机上的快门；压在画面上也要看得见，
    // 所以底色给到 70% 不透明而不是全透。
    s_shutter_btn = lv_btn_create(icon_in_obj);
    lv_obj_set_size(s_shutter_btn, 54, 54);
    lv_obj_align(s_shutter_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_radius(s_shutter_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_shutter_btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(s_shutter_btn, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_shutter_btn, 3, 0);
    lv_obj_set_style_border_color(s_shutter_btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_pad_all(s_shutter_btn, 0, 0);
    lv_obj_set_style_shadow_opa(s_shutter_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(s_shutter_btn, btn_shutter_cb, LV_EVENT_CLICKED, NULL);

    /* ---- 结果提示：默认藏起来，拍完由 ui_camera_capture_result() 弹出来 ---- */
    s_toast_label = lv_label_create(icon_in_obj);
    lv_obj_set_style_text_font(s_toast_label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_toast_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_color(s_toast_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_toast_label, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_toast_label, 8, 0);
    lv_obj_set_style_pad_all(s_toast_label, 6, 0);   // 标签默认无内边距，加上才像个胶囊
    lv_obj_align(s_toast_label, LV_ALIGN_TOP_MID, 0, 46);   // 避开左上角的返回键
    lv_obj_clear_flag(s_toast_label, LV_OBJ_FLAG_CLICKABLE); // 别挡住快门/返回键的点击
    lv_obj_add_flag(s_toast_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_toast_label, "");

    // 提示计时器：建好就暂停着，等真的有提示再 reset + resume。
    s_toast_timer = lv_timer_create(toast_hide_cb, CAMERA_TOAST_MS, NULL);
    if (s_toast_timer != NULL) {
        lv_timer_pause(s_toast_timer);
    }

    icon_flag = 4; // 标记已经进入第四个应用

    ui_camera_task_start();
}



