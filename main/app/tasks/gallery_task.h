#pragma once

/**
 * @brief 扫描 SD 卡里的图片目录，把文件列表发消息给 UI。
 *        目录：/sdcard/photo（相机拍的）与 /sdcard/pic（网页上传的）。
 */
void ui_gallery_task_start(void);

/**
 * @brief 打开列表里的第 index 张图片，解码成 320x240 以内后发消息给 UI。
 * @param index 下标（对应 UI_MSG_GALLERY_LIST 里 items 的顺序）
 */
void ui_gallery_open(int index);

/**
 * @brief 打开列表里的下一张 / 上一张（到头不循环）。
 * @param step +1 下一张，-1 上一张
 */
void ui_gallery_step(int step);
