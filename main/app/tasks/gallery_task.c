#include "gallery_task.h"
#include "ui_msg.h"
#include "app_events.h"
#include "hal_sd.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "gallery";

/** 图库扫描的两个目录。 */
static const char *s_dirs[] = { SD_PHOTO_DIR, "/sdcard/pic" };
#define GALLERY_DIR_NUM  (sizeof(s_dirs) / sizeof(s_dirs[0]))

/** UI 目标区域尺寸：解码结果超过它就缩到这个框里。 */
#define GALLERY_VIEW_W   320
#define GALLERY_VIEW_H   240

/** 解码任务栈（esp_jpeg 的解码器本身要几 KB）。 */
#define GALLERY_TASK_STACK   (10 * 1024)

/** JPEG 工作缓冲（放 PSRAM，避免占内部 RAM）。 */
#define GALLERY_WORK_BUF     (8 * 1024)

/** 单张 JPEG 的文件大小上限。 */
#define GALLERY_MAX_JPEG     (4 * 1024 * 1024)

/** 完整路径缓冲长度：目录前缀 + '/' + 255 字节长文件名。 */
#define GALLERY_PATH_MAX     288

/**
 * @brief 扫描结果与路径表都放 PSRAM。
 *        注意**不能**做成 static 数组：128 条 × 288 字节 ≈ 36KB，
 *        加上列表本身就 43KB，全都会落到内部 DRAM（.bss）上。
 */
static gallery_list_t *s_list  = NULL;
static char (*s_paths)[GALLERY_PATH_MAX] = NULL;
static int s_current = -1;

/**
 * @brief 按需分配列表与路径表（PSRAM）。
 * @return true 分配成功
 */
static bool ensure_buffers(void)
{
    if (s_list == NULL) {
        s_list = heap_caps_malloc(sizeof(gallery_list_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_paths == NULL) {
        s_paths = heap_caps_malloc(sizeof(*s_paths) * GALLERY_MAX_ITEMS,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return (s_list != NULL) && (s_paths != NULL);
}

/**
 * @brief 把三段拼成一个路径放进 dst（自动截断，不会溢出）。
 * @param dst  目标
 * @param cap  容量
 * @param dir  目录
 * @param name 文件名
 */
static void join_path(char *dst, size_t cap, const char *dir, const char *name)
{
    strlcpy(dst, dir, cap);
    strlcat(dst, "/", cap);
    strlcat(dst, name, cap);
}

/**
 * @brief 判断文件名是否是一张可显示的图片（只认 .jpg / .jpeg）。
 * @param name 文件名
 * @return true 是图片
 */
static bool is_image_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 5) {
        return false;
    }
    if (strcasecmp(name + n - 4, ".jpg") == 0) {
        return true;
    }
    if (n >= 5 && strcasecmp(name + n - 5, ".jpeg") == 0) {
        return true;
    }
    return false;
}

/**
 * @brief 把一条记录加进列表（调用方已持 SD 锁）。
 * @param dir  目录
 * @param name 文件名
 * @param size 字节数
 */
static void list_add(const char *dir, const char *name, uint32_t size)
{
    if (s_list->count >= GALLERY_MAX_ITEMS) {
        return;
    }
    int i = s_list->count;
    strlcpy(s_list->items[i].name, name, sizeof(s_list->items[i].name));
    s_list->items[i].size = size;
    join_path(s_paths[i], sizeof(s_paths[i]), dir, name);
    s_list->count++;
}

/**
 * @brief 扫描一个目录下的所有图片。
 * @param dir 目录
 */
static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        if (!is_image_name(e->d_name)) {
            continue;
        }
        char full[GALLERY_PATH_MAX];
        join_path(full, sizeof(full), dir, e->d_name);

        struct stat st;
        uint32_t size = (stat(full, &st) == 0) ? (uint32_t)st.st_size : 0;
        list_add(dir, e->d_name, size);
        s_list->total++;
    }
    closedir(d);
}

/**
 * @brief 图库扫描任务：挂载 SD、扫描两个目录、把列表发消息给 UI，然后自删除。
 * @param arg 未使用
 */
static void gallery_scan_task(void *arg)
{
    int result = GALLERY_OK;

    if (!ensure_buffers()) {
        result = GALLERY_NO_MEM;
        goto done;
    }
    memset(s_list, 0, sizeof(gallery_list_t));
    s_current = -1;

    if (hal_sdcard_lock(3000) != ESP_OK) {
        result = GALLERY_NO_SD;
        goto done;
    }
    if (!bsp_sdcard_is_mounted() && bsp_sdcard_mount() != ESP_OK) {
        hal_sdcard_unlock();
        result = GALLERY_NO_SD;
        goto done;
    }

    for (size_t i = 0; i < GALLERY_DIR_NUM; i++) {
        scan_dir(s_dirs[i]);
    }
    hal_sdcard_unlock();

    ESP_LOGI(TAG, "扫描完成：%d 张", s_list->count);
    if (s_list->count == 0) {
        result = GALLERY_EMPTY;
    }

done:
    if (result == GALLERY_OK) {
        gallery_list_t *out = heap_caps_malloc(sizeof(gallery_list_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (out == NULL) {
            result = GALLERY_NO_MEM;
        } else {
            memcpy(out, s_list, sizeof(gallery_list_t));
            ui_msg_t msg = { .id = UI_MSG_GALLERY_LIST };
            msg.ptr = out;
            ui_msg_send(&msg, portMAX_DELAY);
            vTaskDelete(NULL);
            return;
        }
    }

    {
        ui_msg_t msg = { .id = UI_MSG_GALLERY_FAILED };
        msg.v.i[0] = result;
        ui_msg_send(&msg, portMAX_DELAY);
    }
    vTaskDelete(NULL);
}

/**
 * @brief 启动图库扫描任务（一次性）。
 */
void ui_gallery_task_start(void)
{
    xTaskCreatePinnedToCore(gallery_scan_task, "gallery_scan", 6 * 1024, NULL, 5, NULL, 1);
}

/**
 * @brief 把 RGB565 图像按比例缩放到目标框内（最近邻）。
 * @param src     源
 * @param sw, sh  源尺寸
 * @param dst     目标
 * @param dw, dh  目标框
 * @param out_w, out_h 实际输出尺寸
 */
static void scale_fit(const uint8_t *src, int sw, int sh,
                      uint8_t *dst, int dw, int dh, int *out_w, int *out_h)
{
    int w, h;
    if (sw * dh > sh * dw) {
        w = dw;
        h = (sh * dw) / sw;
    } else {
        h = dh;
        w = (sw * dh) / sh;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    for (int y = 0; y < h; y++) {
        const uint8_t *srow = src + ((size_t)(y * sh / h) * sw * 2);
        uint8_t       *drow = dst + (size_t)y * w * 2;
        for (int x = 0; x < w; x++) {
            const uint8_t *sp = srow + (size_t)(x * sw / w) * 2;
            drow[x * 2 + 0] = sp[0];
            drow[x * 2 + 1] = sp[1];
        }
    }
    *out_w = w;
    *out_h = h;
}

/**
 * @brief 解码一张图片并把它发消息给 UI。
 * @param arg 列表下标（以 intptr_t 传入）
 */
static void gallery_decode_task(void *arg)
{
    int index = (int)(intptr_t)arg;
    int result = GALLERY_OK;
    uint8_t *jpeg = NULL;
    uint8_t *decoded = NULL;
    uint8_t *work = NULL;

    if (s_list == NULL || s_paths == NULL ||
        index < 0 || index >= s_list->count) {
        result = GALLERY_DECODE_FAIL;
        goto fail;
    }

    if (hal_sdcard_lock(3000) != ESP_OK) {
        result = GALLERY_NO_SD;
        goto fail;
    }

    FILE *fp = fopen(s_paths[index], "rb");
    if (fp == NULL) {
        hal_sdcard_unlock();
        result = GALLERY_DECODE_FAIL;
        goto fail;
    }
    fseek(fp, 0, SEEK_END);
    long flen = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (flen <= 0 || flen > GALLERY_MAX_JPEG) {
        fclose(fp);
        hal_sdcard_unlock();
        result = GALLERY_DECODE_FAIL;
        goto fail;
    }

    jpeg = heap_caps_malloc(flen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpeg == NULL) {
        fclose(fp);
        hal_sdcard_unlock();
        result = GALLERY_NO_MEM;
        goto fail;
    }
    if (fread(jpeg, 1, flen, fp) != (size_t)flen) {
        fclose(fp);
        hal_sdcard_unlock();
        result = GALLERY_DECODE_FAIL;
        goto fail;
    }
    fclose(fp);
    hal_sdcard_unlock();

    // 先按"不缩放"问一次尺寸，再挑一个能把结果压进视图的缩放档
    esp_jpeg_image_cfg_t cfg = {
        .indata = jpeg,
        .indata_size = (uint32_t)flen,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        // ★ 必须交换字节序 ★
        // tjpgd 输出的 RGB565 是「高字节在前」，而 LVGL 的 lv_color_t 在
        // 16 位色深下是「低字节在前」。不交换的话每个像素的两个字节被对调，
        // 颜色就会变成照片里那种诡异的紫绿噪点。
        .flags.swap_color_bytes = 1,
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || info.width == 0) {
        result = GALLERY_DECODE_FAIL;
        goto fail;
    }

    if (info.width > GALLERY_VIEW_W * 8 || info.height > GALLERY_VIEW_H * 8) {
        cfg.out_scale = JPEG_IMAGE_SCALE_1_8;
    } else if (info.width > GALLERY_VIEW_W * 4 || info.height > GALLERY_VIEW_H * 4) {
        cfg.out_scale = JPEG_IMAGE_SCALE_1_4;
    } else if (info.width > GALLERY_VIEW_W * 2 || info.height > GALLERY_VIEW_H * 2) {
        cfg.out_scale = JPEG_IMAGE_SCALE_1_2;
    }

    esp_jpeg_image_output_t out = {0};
    if (esp_jpeg_get_image_info(&cfg, &out) != ESP_OK || out.output_len == 0) {
        result = GALLERY_DECODE_FAIL;
        goto fail;
    }

    decoded = heap_caps_malloc(out.output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (decoded == NULL) {
        result = GALLERY_NO_MEM;
        goto fail;
    }
    work = heap_caps_malloc(GALLERY_WORK_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (work != NULL) {
        cfg.advanced.working_buffer = work;
        cfg.advanced.working_buffer_size = GALLERY_WORK_BUF;
    }

    cfg.outbuf = decoded;
    cfg.outbuf_size = out.output_len;
    if (esp_jpeg_decode(&cfg, &out) != ESP_OK) {
        result = GALLERY_DECODE_FAIL;
        goto fail;
    }

    // 缩放到视图框内（解码尺寸可能仍大于 320x240）
    {
        gallery_frame_t *frame = heap_caps_malloc(sizeof(gallery_frame_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t fit_bytes = (size_t)GALLERY_VIEW_W * GALLERY_VIEW_H * 2;
        uint8_t *fit = heap_caps_malloc(fit_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (frame == NULL || fit == NULL) {
            if (frame) { heap_caps_free(frame); }
            if (fit) { heap_caps_free(fit); }
            result = GALLERY_NO_MEM;
            goto fail;
        }

        int w = 0, h = 0;
        if (out.width == GALLERY_VIEW_W && out.height == GALLERY_VIEW_H) {
            memcpy(fit, decoded, fit_bytes);
            w = GALLERY_VIEW_W;
            h = GALLERY_VIEW_H;
        } else {
            scale_fit(decoded, out.width, out.height, fit,
                      GALLERY_VIEW_W, GALLERY_VIEW_H, &w, &h);
        }

        frame->pixels = fit;
        frame->w = (uint16_t)w;
        frame->h = (uint16_t)h;
        s_current = index;

        heap_caps_free(decoded);
        decoded = NULL;
        if (work != NULL) { heap_caps_free(work); work = NULL; }
        if (jpeg != NULL) { heap_caps_free(jpeg); jpeg = NULL; }

        ui_msg_t msg = { .id = UI_MSG_GALLERY_FRAME };
        msg.ptr = frame;
        ui_msg_send(&msg, portMAX_DELAY);
        vTaskDelete(NULL);
        return;
    }

fail:
    if (jpeg)    { heap_caps_free(jpeg); }
    if (decoded) { heap_caps_free(decoded); }
    if (work)    { heap_caps_free(work); }
    {
        ui_msg_t msg = { .id = UI_MSG_GALLERY_FAILED };
        msg.v.i[0] = result;
        ui_msg_send(&msg, portMAX_DELAY);
    }
    vTaskDelete(NULL);
}

/**
 * @brief 启动解码任务打开第 index 张图。
 * @param index 下标
 */
void ui_gallery_open(int index)
{
    xTaskCreatePinnedToCore(gallery_decode_task, "gallery_dec", GALLERY_TASK_STACK,
                            (void *)(intptr_t)index, 5, NULL, 1);
}

/**
 * @brief 打开上一张 / 下一张。
 * @param step +1 下一张，-1 上一张
 */
void ui_gallery_step(int step)
{
    if (s_list == NULL) {
        return;
    }
    int next = s_current + step;
    if (next < 0 || next >= s_list->count) {
        return;
    }
    ui_gallery_open(next);
}
