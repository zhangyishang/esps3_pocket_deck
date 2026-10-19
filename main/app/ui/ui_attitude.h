#pragma once

#include "lvgl.h"

void att_event_handler(lv_event_t * e);
void ui_att_build(void);
void ui_att_init_failed(void);
void ui_att_close_canvas(void);
void ui_att_render(int att_x, int att_y, int att_z, uint8_t status);
