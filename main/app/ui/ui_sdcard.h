#pragma once

#include <stdint.h>
#include "lvgl.h"

void sdcard_event_handler(lv_event_t * e);


void ui_sdcard_mount_failed(void);
void ui_sdcard_close_canvas(void);
void ui_sdcard_build(uint64_t capacity_gb);
