#include "ui_sdcard.h"
#include "ui_common.h"
// sdcard_event_handler 要启动 SD 卡任务。
#include "sdcard_task.h"
#include "hal_sd.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_err.h"
#include "string.h"
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "app_ui";

/******************************** 第3个图标 SD卡 应用程序***************************************************************************/
lv_obj_t * sdcard_title; // SD卡页面标题背景
lv_obj_t * sdcard_label; // SD卡页面标题
lv_obj_t * sdcard_file_list; // SD卡文件列表


struct file_path_info
{
    uint8_t path_index;  // 在第几级目录
    char path_now[512]; // 当前文件路径
    char path_back[512]; // 上级文件路径
};
struct file_path_info file_path_info;

// 函数声明
esp_err_t list_sdcard_files(char * path);
static void file_list_btn_cb(lv_event_t * e); 

// 返回主界面按钮事件处理函数
static void btn_sdback_cb(lv_event_t * e)
{
    if (file_path_info.path_index == 0){ // 如果当前是根目录
        bsp_sdcard_unmount(); // 卸载SD卡
        lv_obj_del(icon_in_obj); // 回到主界面
    }else{
        lv_obj_clean(sdcard_file_list); // 清除当前wifi列表
        esp_err_t ret = list_sdcard_files(file_path_info.path_back); // 列出上一级目录文件
        if (ret == ESP_OK){ // 如果成功列出目录
            strcpy(file_path_info.path_now, file_path_info.path_back); // 刚刚进入的这个目录路径 变成当前路径
            file_path_info.path_index--; // 目录级数索引退一级
            // 计算再向下退一级的目录路径
            char *slash = strrchr(file_path_info.path_back, '/'); // 从后往前查找字符'/'
            if (slash!= NULL) { // 如果查找到
                *slash = '\0'; // 替换为NULL 表示字符串结束
            }
            ESP_LOGI(TAG, "path_index: %d", file_path_info.path_index);
            ESP_LOGI(TAG, "path_now: %s", file_path_info.path_now);
            ESP_LOGI(TAG, "path_back: %s", file_path_info.path_back);
        }
    }
}

// 列出SD卡中的文件
esp_err_t list_sdcard_files(char * path) 
{
    esp_err_t ret;
    DIR *dir;
    struct dirent *ent;
    lv_obj_t * btn;
    if ((dir = opendir(path))!= NULL) { // 打开目录
        while ((ent = readdir(dir))!= NULL) { // 读取目录中的文件
            /* 常规文件处理 */
            if (ent->d_type == DT_REG){ // 如果是常规文件
                int file_type_flag = 0;
                const char* extension = strrchr(ent->d_name, '.'); // 从后往前 找到字符'.'
                if (extension != NULL) { // 如果找到了'.'
                    extension++; // 指针地址+1
                    if (strcmp(extension, "mp3") == 0) { // 如果是mp3
                        file_type_flag = 1; // 标记为音乐文件
                    } else if (strcmp(extension, "wav") == 0) { // 如果是wav
                        file_type_flag = 1; // 标记为音乐文件
                    } else if (strcmp(extension, "mp4") == 0) {
                        file_type_flag = 2; // 标记为视频文件
                    } else if (strcmp(extension, "avi") == 0) {
                        file_type_flag = 2; // 标记为视频文件
                    } else if (strcmp(extension, "jpg") == 0) {
                        file_type_flag = 3; // 标记为图片
                    } else if (strcmp(extension, "jpeg") == 0) {
                        file_type_flag = 3; // 标记为图片
                    } else if (strcmp(extension, "png") == 0) {
                        file_type_flag = 3; // 标记为图片
                    } else if (strcmp(extension, "bmp") == 0) {
                        file_type_flag = 3; // 标记为图片
                    } else if (strcmp(extension, "gif") == 0) {
                        file_type_flag = 3; // 标记为图片
                    } else {
                        file_type_flag = 0; // 除了以上文件 其它文件都归类为普通文件
                    }
                }
                lvgl_port_lock(0);
                switch (file_type_flag)
                {
                case 1:
                    btn = lv_list_add_btn(sdcard_file_list, LV_SYMBOL_AUDIO, (const char *)ent->d_name);  // 显示音乐文件图标
                    break;
                case 2:
                    btn = lv_list_add_btn(sdcard_file_list, LV_SYMBOL_VIDEO, (const char *)ent->d_name);  // 显示视频文件图标
                    break;
                case 3:
                    btn = lv_list_add_btn(sdcard_file_list, LV_SYMBOL_IMAGE, (const char *)ent->d_name);  // 显示图片文件图标
                    break;
                default:
                    btn = lv_list_add_btn(sdcard_file_list, LV_SYMBOL_FILE, (const char *)ent->d_name);  // 显示普通文件图标
                    break;
                }
                lv_obj_t *icon = lv_obj_get_child(btn, 0); // 获取图标指针
                lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0); // 修改图标的字体    
                lv_obj_add_event_cb(btn, file_list_btn_cb, LV_EVENT_CLICKED, NULL);  // 添加点击回调函数
                lvgl_port_unlock();
            }
            /* 文件夹处理 */
            else if (ent->d_type == DT_DIR) { // 如果是文件夹
                lvgl_port_lock(0);
                btn = lv_list_add_btn(sdcard_file_list, LV_SYMBOL_DIRECTORY, (const char *)ent->d_name); 
                lv_obj_t *icon = lv_obj_get_child(btn, 0); // 获取图标指针
                lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0); // 修改图标的字体
                lv_obj_add_event_cb(btn, file_list_btn_cb, LV_EVENT_CLICKED, NULL); // 添加点击回调函数
                lvgl_port_unlock();
            }
        }
        closedir(dir);
        ret = ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to open directory %s.", path);
        ret = ESP_FAIL;
    }
    return ret;
}

// 文件点击 事件处理函数
static void file_list_btn_cb(lv_event_t * e)
{
    const char *file_name = NULL; // 当前文件名称
    // 获取点击的按钮名称 即文件名称
    file_name = lv_list_get_btn_text(lv_obj_get_parent(e->target), e->target);
    ESP_LOGI(TAG, "file name: %s", file_name);
    // 列出 SD 卡中的文件
    struct stat st; // 获取文件状态信息结构体
    strcpy(file_path_info.path_back, file_path_info.path_now); // 保存上一级目录
    strcat(file_path_info.path_now, "/");
    strcat(file_path_info.path_now, file_name);
    if (stat(file_path_info.path_now, &st) == 0){ // 如果成功获取到状态信息
        if (S_ISDIR(st.st_mode)){ // 如果是目录
            lv_obj_clean(sdcard_file_list); // 清除当前wifi列表
            esp_err_t ret = list_sdcard_files(file_path_info.path_now);
            if (ret == ESP_OK){ // 如果成功列出了目录
                file_path_info.path_index++; // 表示进入到下一集目录
                ESP_LOGI(TAG, "path_index: %d", file_path_info.path_index);
                ESP_LOGI(TAG, "path_now: %s", file_path_info.path_now);
                ESP_LOGI(TAG, "path_back: %s", file_path_info.path_back);
            }
            return;
        }
    }
    // 如果没有成功进入目录
    strcpy(file_path_info.path_now, file_path_info.path_back); // 没有列出新的列表 还原当前路径
    // 还原再向下退一级的目录路径
    char *slash = strrchr(file_path_info.path_back, '/'); // 从后往前查找字符'/'
    if (slash!= NULL) { // 如果查找到
        *slash = '\0'; // 替换为NULL 表示字符串结束
    }
    ESP_LOGI(TAG, "path_index: %d", file_path_info.path_index);
    ESP_LOGI(TAG, "path_now: %s", file_path_info.path_now);
    ESP_LOGI(TAG, "path_back: %s", file_path_info.path_back);
}

// 渲染函数：挂载失败时在标题栏显示错误提示。
void ui_sdcard_mount_failed(void)
{
    lvgl_port_lock(0);
    lv_label_set_text(sdcard_label, "SD卡挂载不成功");
    lvgl_port_unlock();
}

// 渲染函数：删除 SD 卡画布，回到主界面。
void ui_sdcard_close_canvas(void)
{
    lvgl_port_lock(0);
    lv_obj_del(icon_in_obj);
    lvgl_port_unlock();
}

// 渲染函数：建好 SD 卡页面（容量标签 + 返回按钮 + 文件列表）并列出根目录。
//
// capacity_gb 由调用方算好传进来（原表达式就写在 lv_label_set_text_fmt 的实参里，
// 现在提到调用侧，算式一字不改）。两对 lock/unlock 之间的分段逐字保留 ——
// 原来是"先单独刷容量标签，再单独建控件"两次持锁，这里不合并。
void ui_sdcard_build(uint64_t capacity_gb)
{
    // 液晶屏标题栏显示SD卡容量
    lvgl_port_lock(0);
    lv_label_set_text_fmt(sdcard_label, "SD: %lluGB", capacity_gb);
    lvgl_port_unlock();

    // 创建返回按钮
    lvgl_port_lock(0);
    lv_obj_t *btn_back = lv_btn_create(sdcard_title);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_size(btn_back, 60, 30);
    lv_obj_set_style_border_width(btn_back, 0, 0); // 设置边框宽度
    lv_obj_set_style_pad_all(btn_back, 0, 0);  // 设置间隙
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN); // 背景透明
    lv_obj_set_style_shadow_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN); // 阴影透明
    lv_obj_add_event_cb(btn_back, btn_sdback_cb, LV_EVENT_CLICKED, NULL); // 添加按键处理函数

    lv_obj_t *label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);  // 按键上显示左箭头符号
    lv_obj_set_style_text_font(label_back, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_back, lv_color_hex(0xffffff), 0);
    lv_obj_align(label_back, LV_ALIGN_CENTER, -10, 0);

    // 创建文件列表
    sdcard_file_list = lv_list_create(icon_in_obj);
    lv_obj_set_size(sdcard_file_list, 320, 200);
    lv_obj_align(sdcard_file_list, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_border_width(sdcard_file_list, 0, 0);
    lv_obj_set_style_text_font(sdcard_file_list, &font_alipuhui20, 0);
    lv_obj_set_scrollbar_mode(sdcard_file_list, LV_SCROLLBAR_MODE_OFF); // 隐藏wifi_list滚动条
    lvgl_port_unlock();
    // 列出 SD 卡中的文件
    file_path_info.path_index = 0; // 表示当前在根目录
    strcpy(file_path_info.path_now, SD_MOUNT_POINT); // 装入当前路径
    list_sdcard_files(file_path_info.path_now); // 列出当前目录文件
}

// SD卡处理任务 task_process_sdcard 与 ui_sdcard_task_start 已搬到
// app/tasks/sdcard_task.c —— RTOS 与文件系统的关注点不该和 LVGL 混在一个文件里。

// 进入SD卡应用程序
void sdcard_event_handler(lv_event_t * e)
{
    // 创建一个界面对象
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
    sdcard_title = lv_obj_create(icon_in_obj);
    lv_obj_set_size(sdcard_title, 320, 40);
    lv_obj_set_style_pad_all(sdcard_title, 0, 0);  // 设置间隙
    lv_obj_align(sdcard_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(sdcard_title, lv_color_hex(0x008b8b), 0);
    // 显示标题
    sdcard_label = lv_label_create(sdcard_title);
    lv_label_set_text(sdcard_label, "TF卡扫描中...");
    lv_obj_set_style_text_color(sdcard_label, lv_color_hex(0xffffff), 0); 
    lv_obj_set_style_text_font(sdcard_label, &font_alipuhui20, 0);
    lv_obj_align(sdcard_label, LV_ALIGN_CENTER, 0, 0);

    icon_flag = 3; // 标记已经进入第三个应用

    ui_sdcard_task_start();
}




