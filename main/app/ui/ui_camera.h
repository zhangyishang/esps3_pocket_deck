#pragma once

#include "lvgl.h"
#include "hal_camera.h"

void camera_event_handler(lv_event_t * e);


void ui_camera_render_frame(camera_fb_t *frame);
void ui_camera_close_canvas(void);
void ui_camera_capture_result(int result);
