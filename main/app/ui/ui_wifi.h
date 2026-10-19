#pragma once

#include "lvgl.h"
// ui_wifi_ap_list_build() 的形参用到 wifi_ap_record_t。
#include "esp_wifi_types.h"
// ui_wifi_conn_show_result/_close_pages 的形参用到 wifi_result_t。
#include "ui_msg.h"

void wifiset_event_handler(lv_event_t * e);


void ui_wifi_show_datetime(void);

void ui_wifi_ap_list_build(const wifi_ap_record_t *ap_info, uint16_t ap_number);

void ui_wifi_conn_show_result(wifi_result_t result, const char *ssid);
void ui_wifi_conn_close_pages(wifi_result_t result);

void value_update_cb(lv_timer_t * timer);// 时间显示的定时器回调（主界面建它，也用它立即填一次）