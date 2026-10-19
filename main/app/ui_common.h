#pragma once

#include "lvgl.h"
#include "app_events.h"

/*********************** 主界面 ****************************/
extern lv_obj_t *main_obj;        // 主界面

/*********************** 应用界面 ****************************/
extern lv_obj_t *icon_in_obj; // 应用界面

LV_FONT_DECLARE(font_alipuhui20);


bool ui_is_alive(void);
