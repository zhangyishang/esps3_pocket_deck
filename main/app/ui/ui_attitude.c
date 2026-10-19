#include "ui_attitude.h"
#include "ui_common.h"
#include "hal_qmi8658.h"
// att_event_handler 要启动姿态任务。方向是 ui/ → tasks/，与解耦前一致。
#include "att_task.h"
#include "esp_lvgl_port.h"
#include "math.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/******************************** 第1个图标 姿态传感器 应用程序*************************************************************************************/
lv_obj_t * label_x; // x角度值
lv_obj_t * label_y; // y角度值
lv_obj_t * label_z; // z角度值
lv_obj_t * x_bar;   // x角度bar
lv_obj_t * y_bar;   // y角度bar
lv_obj_t * z_bar;   // z角度bar

lv_obj_t * att_label; // 标题栏文字

lv_timer_t * my_lv_timer;

lv_obj_t *btn_att_back; // att姿态应用 后退按钮

static void btn_att_back_cb(lv_event_t * e)
{
    icon_flag = 0;
}

void att_update_cb(lv_timer_t * timer)
{
    t_sQMI8658 QMI8658;
    int att_x = 0, att_y = 0, att_z = 0;
    // 获取XYZ角度
    qmi8658_fetch_angleFromAcc(&QMI8658);
    att_x = round(QMI8658.AngleX);  // 四舍五入
    att_y = round(QMI8658.AngleY);  // 四舍五入
    att_z = round(QMI8658.AngleZ);  // 四舍五入
    // 更新角度值
    lv_label_set_text_fmt(label_x, "X: %d", att_x);
    lv_label_set_text_fmt(label_y, "Y: %d", att_y);
    lv_label_set_text_fmt(label_z, "Z: %d", att_z);
    // 更新角度bar
    lv_bar_set_start_value(x_bar, att_x-10, LV_ANIM_OFF);
    lv_bar_set_value(x_bar, att_x+10, LV_ANIM_OFF);
    lv_bar_set_start_value(y_bar, att_y-10, LV_ANIM_OFF);
    lv_bar_set_value(y_bar, att_y+10, LV_ANIM_OFF);
    lv_bar_set_start_value(z_bar, att_z-10, LV_ANIM_OFF);
    lv_bar_set_value(z_bar, att_z+10, LV_ANIM_OFF);

    // 判断运动状态
    uint8_t status = qmi8658_fetch_motion();
    if (status & 0x20) // 判断是否发生Any-Motion
    {
        lv_label_set_text(att_label, "运动或震动");
    }
    else if (status & 0x40) // 判断是否发生No-Motion
    {
        lv_label_set_text(att_label, "静止");
    }
    else if (status & 0x80) // 判断是否发生Significant-Motion
    {
        lv_label_set_text(att_label, "剧烈运动");
    }
}

void ui_att_render(int att_x, int att_y, int att_z, uint8_t status)
{
    // 更新角度值
    lv_label_set_text_fmt(label_x, "X: %d", att_x);
    lv_label_set_text_fmt(label_y, "Y: %d", att_y);
    lv_label_set_text_fmt(label_z, "Z: %d", att_z);
    // 更新角度bar
    lv_bar_set_start_value(x_bar, att_x-10, LV_ANIM_OFF);
    lv_bar_set_value(x_bar, att_x+10, LV_ANIM_OFF);
    lv_bar_set_start_value(y_bar, att_y-10, LV_ANIM_OFF);
    lv_bar_set_value(y_bar, att_y+10, LV_ANIM_OFF);
    lv_bar_set_start_value(z_bar, att_z-10, LV_ANIM_OFF);
    lv_bar_set_value(z_bar, att_z+10, LV_ANIM_OFF);

    // 判断运动状态
    if (status & 0x20) // 判断是否发生Any-Motion
    {
        lv_label_set_text(att_label, "运动或震动");
    }
    else if (status & 0x40) // 判断是否发生No-Motion
    {
        lv_label_set_text(att_label, "静止");
    }
    else if (status & 0x80) // 判断是否发生Significant-Motion
    {
        lv_label_set_text(att_label, "剧烈运动");
    }
}

void ui_att_build(void)
{
    lvgl_port_lock(0);
    // 显示x角度值
    label_x = lv_label_create(icon_in_obj);
    lv_label_set_text(label_x, "X:");
    lv_obj_set_style_text_color(label_x, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(label_x, &lv_font_montserrat_20, 0);
    lv_obj_align(label_x, LV_ALIGN_TOP_LEFT, 20, 60);
    // 显示x角度bar
    x_bar = lv_bar_create(icon_in_obj);
    lv_obj_set_size(x_bar, 200, 25);
    lv_obj_align(x_bar, LV_ALIGN_TOP_LEFT, 80, 60);
    lv_bar_set_mode(x_bar, LV_BAR_MODE_RANGE);
    lv_bar_set_range(x_bar, -101, 101);
    lv_bar_set_start_value(x_bar, -10, LV_ANIM_OFF);
    lv_bar_set_value(x_bar, 10, LV_ANIM_OFF);

    // 显示y角度值
    label_y = lv_label_create(icon_in_obj);
    lv_label_set_text(label_y, "Y:");
    lv_obj_set_style_text_color(label_y, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(label_y, &lv_font_montserrat_20, 0);
    lv_obj_align(label_y, LV_ALIGN_TOP_LEFT, 20, 120);
    // 显示y角度bar
    y_bar = lv_bar_create(icon_in_obj);
    lv_obj_set_size(y_bar, 200, 25);
    lv_obj_align(y_bar, LV_ALIGN_TOP_LEFT, 80, 120);
    lv_bar_set_mode(y_bar, LV_BAR_MODE_RANGE);
    lv_bar_set_range(y_bar, -101, 101);
    lv_bar_set_start_value(y_bar, -10, LV_ANIM_OFF);
    lv_bar_set_value(y_bar, 10, LV_ANIM_OFF);

    // 显示z角度值
    label_z = lv_label_create(icon_in_obj);
    lv_label_set_text(label_z, "Z:");
    lv_obj_set_style_text_color(label_z, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(label_z, &lv_font_montserrat_20, 0);
    lv_obj_align(label_z, LV_ALIGN_TOP_LEFT, 20, 180);
    // 显示z角度bar
    z_bar = lv_bar_create(icon_in_obj);
    lv_obj_set_size(z_bar, 200, 25);
    lv_obj_align(z_bar, LV_ALIGN_TOP_LEFT, 80, 180);
    lv_bar_set_mode(z_bar, LV_BAR_MODE_RANGE);
    lv_bar_set_range(z_bar, -101, 101);
    lv_bar_set_start_value(z_bar, -10, LV_ANIM_OFF);
    lv_bar_set_value(z_bar, 10, LV_ANIM_OFF);

    lvgl_port_unlock();

}

// 传感器初始化失败提示的渲染接缝（第 10 步 A）：从 task_process_att 的失败分支抽出。
void ui_att_init_failed(void)
{
    lvgl_port_lock(0);
    lv_obj_t * label = lv_label_create(icon_in_obj);
    lv_label_set_text(label, "QMI8658传感器错误...");
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0); 
    lv_obj_set_style_text_font(label, &font_alipuhui20, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lvgl_port_unlock();
}

void ui_att_close_canvas(void)
{
    lvgl_port_lock(0);
    lv_obj_del(icon_in_obj); // 删除画布 回到主界面
    lvgl_port_unlock();
}

void att_event_handler(lv_event_t * e)
{
    // 创建一个界面
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_radius(&style, 10);  
    lv_style_set_bg_opa( &style, LV_OPA_COVER );
    lv_style_set_bg_color(&style, lv_color_hex(0xffffff));
    lv_style_set_border_width(&style, 0);
    lv_style_set_pad_all(&style, 0);
    lv_style_set_width(&style, 320);  
    lv_style_set_height(&style, 240); 

    icon_in_obj = lv_obj_create(lv_scr_act());
    lv_obj_add_style(icon_in_obj, &style, 0);

    // 创建标题背景
    lv_obj_t *att_title = lv_obj_create(icon_in_obj);
    lv_obj_set_size(att_title, 320, 40);
    lv_obj_set_style_pad_all(att_title, 0, 0);  // 设置间隙
    lv_obj_align(att_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(att_title, lv_color_hex(0x30a830), 0);
    // 显示标题
    att_label = lv_label_create(att_title);
    lv_label_set_text(att_label, "运动监测");
    lv_obj_set_style_text_color(att_label, lv_color_hex(0xffffff), 0); 
    lv_obj_set_style_text_font(att_label, &font_alipuhui20, 0);
    lv_obj_align(att_label, LV_ALIGN_CENTER, 0, 0);
    // 创建后退按钮
    btn_att_back = lv_btn_create(att_title);
    lv_obj_align(btn_att_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_size(btn_att_back, 60, 30);
    lv_obj_set_style_border_width(btn_att_back, 0, 0); // 设置边框宽度
    lv_obj_set_style_pad_all(btn_att_back, 0, 0);  // 设置间隙
    lv_obj_set_style_bg_opa(btn_att_back, LV_OPA_TRANSP, LV_PART_MAIN); // 背景透明
    lv_obj_set_style_shadow_opa(btn_att_back, LV_OPA_TRANSP, LV_PART_MAIN); // 阴影透明
    lv_obj_add_event_cb(btn_att_back, btn_att_back_cb, LV_EVENT_CLICKED, NULL); // 添加按键处理函数

     lv_obj_t *label_back = lv_label_create(btn_att_back); 
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);  // 按键上显示左箭头符号
    lv_obj_set_style_text_font(label_back, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_back, lv_color_hex(0xffffff), 0); 
    lv_obj_align(label_back, LV_ALIGN_CENTER, -10, 0);

    icon_flag = 1; // 标记已经进入第一个应用
    ui_attitude_task_start();
}



