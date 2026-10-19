#include "ui_lockscreen.h"
#include "ui_common.h"
#include "ui_msg.h"

LV_IMG_DECLARE(lockscreen_map);
/*********************** 锁屏界面 ****************************/

// 判定"上滑"的最小距离（像素）
#define LOCK_SWIPE_MIN_Y   40

static lv_obj_t  *s_lock_obj;
static lv_point_t s_press_pt;

// true = 这一层是压在已经建好的主界面上的锁屏

static bool s_over_main;

// 解锁：撕掉锁屏。开机路径再让消息层去建主界面。
static void lock_unlock(void)
{
    // 判空：lv_obj_del(NULL) 不是空操作，会一路走到 lv_obj_invalidate(NULL) →
    // _lv_obj_get_ext_draw_size(NULL) 读 obj->spec_attr 直接崩（LoadProhibited）。
    if (s_lock_obj != NULL) {
        lv_obj_del(s_lock_obj);
        s_lock_obj = NULL;
    }

    if (s_over_main) {
        return;     // 主界面那份还在，撕膜就完事
    }

    ui_msg_t msg = { .id = UI_MSG_MAIN_PAGE_BUILD };
    ui_msg_send(&msg, 0);
}

//上滑事件解锁
static void lock_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);

    if (indev == NULL) {
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &s_press_pt);
    }
    else if (code == LV_EVENT_RELEASED) {
        lv_point_t now;
        lv_indev_get_point(indev, &now);

        int dy = now.y - s_press_pt.y;
        int dx = now.x - s_press_pt.x;

        if (-dy >= LOCK_SWIPE_MIN_Y && -dy > (dx < 0 ? -dx : dx)) {
            lock_unlock();
        }
    }
}

// 创建锁屏
static void lockscreen_create(bool opaque)
{
    if (s_lock_obj != NULL) {
        return;
    }
    s_over_main = opaque;

    s_lock_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_lock_obj, 320, 240);
    lv_obj_set_pos(s_lock_obj, 0, 0);
    lv_obj_set_style_border_width(s_lock_obj, 0, 0);
    lv_obj_set_style_pad_all(s_lock_obj, 0, 0);
    lv_obj_clear_flag(s_lock_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_lock_obj, LV_OBJ_FLAG_CLICKABLE);

    if (opaque) {
        lv_obj_set_style_bg_opa(s_lock_obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_lock_obj, lv_color_hex(0x000000), 0);
    } else {
        lv_obj_set_style_bg_opa(s_lock_obj, LV_OPA_TRANSP, 0);
    }

    lv_obj_add_event_cb(s_lock_obj, lock_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_lock_obj, lock_event_cb, LV_EVENT_RELEASED, NULL);


    // 在里面放图片
    lv_obj_t *img = lv_img_create(s_lock_obj);
    lv_img_set_src(img, &lockscreen_map);

    // 让图片对齐并铺满
    lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);   // 让点击穿透到 s_lock_obj

    lv_obj_t *hint = lv_label_create(s_lock_obj);
    lv_label_set_text(hint, "上滑进入菜单");
    lv_obj_set_style_text_font(hint, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xabbbbb), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -22);
}

void ui_lockscreen_build(void)
{
    lockscreen_create(false);
}

// 主界面上的"回到锁屏"应用用。
void ui_lockscreen_build_over_main(void)
{
    lockscreen_create(true);
}

// 主界面第 2 页那个挂锁图标的点击回调。
void lock_event_handler(lv_event_t * e)
{
    ui_lockscreen_build_over_main();
}
