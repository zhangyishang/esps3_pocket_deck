#pragma once

#include "esp_bit_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

/*********************** 应用状态 ****************************/

// 标记现在进入哪个应用，在主界面时为 0。
extern int icon_flag;

enum {
    SCREEN_MAIN = 0,
    SCREEN_ATT = 1,
    SCREEN_MUSIC = 2,
    SCREEN_SD = 3,
    SCREEN_CAMERA = 4,
    SCREEN_WIFI = 5,
    // 6 号（原蓝牙）已停用：蓝牙功能整体移除。**编号不复用**，
    // 免得和 git 历史里的旧代码/日志对不上。
    SCREEN_CALENDAR = 7,
    SCREEN_WEATHER = 8,
    SCREEN_OTA = 9,
    SCREEN_GALLERY = 10,
    SCREEN_WEB = 11,
    SCREEN_XIAOZHI = 12,
};


extern EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1
#define WIFI_START_BIT        BIT2
#define WIFI_GET_SNTP_BIT     BIT3   


typedef struct {
    char wifi_ssid[33];  // 获取wifi名字
    char wifi_password[64]; // 获取wifi密码
    char back_flag; // 是否退出 / 是否是上电自动连接
} wifi_account_t;

#define WIFI_BACK_FLAG_AUTO 2

extern QueueHandle_t xQueueWifiAccount;
