#include "ui_common.h"

lv_obj_t *main_obj;        // 主界面
lv_obj_t *icon_in_obj;     // 应用界面
int icon_flag;             // 标记现在进入哪个应用 在主界面时为0

/*********************** 界面存活判据 ****************************/

// 给 OTA 回滚自检用。
bool ui_is_alive(void)
{
    lv_obj_t *scr = lv_scr_act();
    if (scr == NULL) 
    {
        return false;
    }

    return lv_obj_get_child_cnt(scr) > 0;
}



