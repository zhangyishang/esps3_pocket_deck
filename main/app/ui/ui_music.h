#pragma once

#include <stdbool.h>
#include "lvgl.h"

bool mp3_player_init(void);
void music_ui(void);
void music_event_handler(lv_event_t * e);


void ui_music_set_current_track(int index);
