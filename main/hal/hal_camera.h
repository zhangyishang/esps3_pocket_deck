#pragma once

/***********************************************************/
/****************    摄像头 ↓   ****************************/
#define CAMERA_EN    1
#if CAMERA_EN
#include "esp_camera.h"

#define CAMERA_PIN_PWDN -1
#define CAMERA_PIN_RESET -1
#define CAMERA_PIN_XCLK 5
#define CAMERA_PIN_SIOD 1
#define CAMERA_PIN_SIOC 2

#define CAMERA_PIN_D7 9
#define CAMERA_PIN_D6 4
#define CAMERA_PIN_D5 6
#define CAMERA_PIN_D4 15
#define CAMERA_PIN_D3 17
#define CAMERA_PIN_D2 8
#define CAMERA_PIN_D1 18
#define CAMERA_PIN_D0 16
#define CAMERA_PIN_VSYNC 3
#define CAMERA_PIN_HREF 46
#define CAMERA_PIN_PCLK 7


#define XCLK_FREQ_HZ 24000000

/**
 * @brief 初始化摄像头（DVP + GC0308，RGB565 / QVGA，帧缓冲在 PSRAM）。
 *        调用前需先由 PCA9557 给摄像头上电。
 */
void bsp_camera_init(void);
#endif
/********************    摄像头 ↑   *************************/
/***********************************************************/
