#include "ota_task.h"
#include "ui_msg.h"
#include "wifi_net.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ota";

/* ======================= 配置 ======================= */

/**
 * @brief 版本清单地址（纯文本，竖线分隔）。
 */
#define OTA_MANIFEST_URL "http://192.168.0.138/build/version.txt"

#define OTA_HTTP_TIMEOUT_MS 15000  //ota最长传输时间


#define OTA_CHUNK_SIZE 4096  // 写 flash 的分块大小。

#define OTA_MANIFEST_BUF 320    // 清单缓冲区大小
#define OTA_FIELD_MAX    6       // 清单最大字段数

#define OTA_YIELD_EVERY_CHUNKS  2     // 每 2 个分块（8KB）
#define OTA_YIELD_MS            2     // 让出 2ms

/* ======================= 版本比较 ======================= */

/**
  * @brief  按数字段逐段比较两个版本号
  * @param  a 版本字符串 a
  * @param  b 版本字符串 b
  * @retval >0 表示 a 更新；<0 表示 b 更新；0 表示相同
  */
static int version_cmp(const char *a, const char *b)
{
    for (;;) {
        // 跳过所有非数字字符（'.'、'-'、以及可能的 'v' 前缀）
        while (*a != '\0' && !(*a >= '0' && *a <= '9')) a++;
        while (*b != '\0' && !(*b >= '0' && *b <= '9')) b++;
        // 两边都到末尾，版本相同
        if (*a == '\0' && *b == '\0') {
            return 0;
        }
        // 逐位解析本段数字
        long va = 0, vb = 0;
        while (*a >= '0' && *a <= '9') va = va * 10 + (*a++ - '0');
        while (*b >= '0' && *b <= '9') vb = vb * 10 + (*b++ - '0');
        // 本段不同则直接给出比较结果
        if (va != vb) {
            return (va > vb) ? 1 : -1;
        }
        if (*a == '\0' && *b == '\0') {
            return 0;
        }
    }
}

/* ======================= 清单解析 ======================= */
// OTA 清单信息结构体
typedef struct {
    char     version[24];   //版本号
    uint32_t size;          //镜像大小
    char     url[200];      //镜像下载URL
    char     sha256[72];    //签名校验的密钥
} ota_info_t;

static ota_info_t s_info;
static bool       s_info_valid;

/**
  * @brief  按 '|' 分割字符串为多个字段
  * @param  s   待切分的输入字符串（会被就地修改）
  * @param  out 输出字段指针数组
  * @param  max 最多接受的字段数
  * @retval 实际切分出的字段数
  */
static int split_fields(char *s, char *out[], int max)
{
    int n = 0;
    out[n++] = s;
    for (char *p = s; *p != '\0' && n < max; p++) {
        if (*p == '|') {
            *p = '\0';
            out[n++] = p + 1;
        }
    }
    return n;
}

/**
  * @brief  解析版本清单文本，填充 ota_info_t
  * @param  body 清单文本（会被就地修改）
  * @param  out  输出解析结果
  * @retval true 解析成功；false 格式非法
  */
static bool parse_manifest(char *body, ota_info_t *out)
{
    char *f[OTA_FIELD_MAX] = {0};
    int n = split_fields(body, f, OTA_FIELD_MAX);
    ESP_LOGI(TAG, "清单字段数 = %d", n);
    if (n < 4) {
        ESP_LOGE(TAG, "字段数不足（至少要 version|size|url）");
        return false;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->version, f[0], sizeof(out->version));

    for (const char *p = out->version; *p != '\0'; p++) {
        if (!((*p >= '0' && *p <= '9') || *p == '.')) {
            ESP_LOGE(TAG, "版本号里有非法字符: '%s'", out->version);
            return false;
        }
    }
    if (out->version[0] == '\0') {
        ESP_LOGE(TAG, "版本号为空");
        return false;
    }

    out->size = (uint32_t)strtoul(f[1], NULL, 10);
    if (out->size == 0) {
        ESP_LOGE(TAG, "大小解析失败: '%s'", f[1]);
        return false;
    }

    strlcpy(out->url, f[2], sizeof(out->url));
    if (strncmp(out->url, "http", 4) != 0) {
        ESP_LOGE(TAG, "URL 不以 http 开头: '%s'", out->url);
        return false;
    }

    if (n >= 4) {
        strlcpy(out->sha256, f[3], sizeof(out->sha256));
    }
    if (n >= 5) {
        ESP_LOGI(TAG, "更新说明: %s", f[4]);
    }
    return true;
}

/* ======================= 发消息 ======================= */

/**
  * @brief  向 UI 发送一条带结果码的 OTA 消息
  * @param  id   UI 消息 ID
  * @param  code 结果码
  * @retval 无
  */
static void ota_post_msg(ui_msg_id_t id, int code)
{
    ui_msg_t msg = { .id = id };
    msg.v.i[0] = code;
    ui_msg_send(&msg, portMAX_DELAY);
}
/**
  * @brief  向 UI 发送"发现新版本"消息（携带版本与大小）
  * @param  info 清单信息
  * @retval 无
  */
static void ota_post_found(const ota_info_t *info)
{
    ui_msg_t msg = { .id = UI_MSG_OTA_FOUND };
    ota_found_t f = {0};
    strlcpy(f.version, info->version, sizeof(f.version));
    f.size = info->size;
    // 只把"弹确认框需要的两个数字"送过去；URL、sha256 留在任务侧。
    memcpy(msg.blob, &f, sizeof(f));
    ui_msg_send(&msg, portMAX_DELAY);
}
/**
  * @brief  向 UI 发送 OTA 进度消息
  * @param  pct 进度百分比（0~100）
  * @retval 无
  */
static void ota_post_progress(int pct)
{
    ui_msg_t msg = { .id = UI_MSG_OTA_PROGRESS };
    msg.v.i[0] = pct;
    ui_msg_send(&msg, portMAX_DELAY);
}

/* ======================= HTTP 小工具 ======================= */

/**
  * @brief  HTTP GET 一个小文件到缓冲区
  * @param  url 请求地址
  * @param  buf 输出缓冲区
  * @param  cap 缓冲区容量
  * @retval >=0 读到的字节数；-1 失败
  */
static int http_get_small(const char *url, char *buf, size_t cap)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = false,// 允许重定向，关掉重定向会变成"看着能连上但读不到内容"。
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init 失败");
        return -1;
    }
    esp_http_client_set_header(c, "User-Agent", "curl/8.4.0");

    int total = -1;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        ESP_LOGE(TAG, "连接失败: %s", url);
        goto out;
    }
    esp_http_client_fetch_headers(c);
    if (esp_http_client_get_status_code(c) != 200) {
        ESP_LOGE(TAG, "HTTP 状态码 %d（%s）", esp_http_client_get_status_code(c), url);
        goto out;
    }
    // 循环读取，直到缓冲剩余空间不足 1 字节
    total = 0;
    while (total < (int)cap - 1) {
        int r = esp_http_client_read(c, buf + total, cap - 1 - total);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    buf[total] = '\0'; // 保证字符串结束

out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return total;
}

/* ======================= 下载并写入 ======================= */

/**
  * @brief  执行完整的 OTA 下载、写入、校验与切换分区流程
  * @param  info 清单信息（含 URL、大小、版本）
  * @retval ota_result_t 结果码（成功路径不会返回）
  */
static int ota_perform_update(const ota_info_t *info)
{
    // 获取下一个可写 OTA 分区
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        ESP_LOGE(TAG, "找不到可用的 OTA 分区 —— 分区表里必须有 ota_0 和 ota_1");
        return OTA_NO_PARTITION;
    }
    ESP_LOGI(TAG, "目标分区: %s @0x%08lx, 大小 %lu 字节",
             part->label, (unsigned long)part->address, (unsigned long)part->size);
    // 镜像大于分区则直接放弃
    if (info->size > part->size) {
        ESP_LOGE(TAG, "镜像 %lu 字节 > 分区 %lu 字节，装不下",
                 (unsigned long)info->size, (unsigned long)part->size);
        return OTA_TOO_BIG;
    }

    const esp_http_client_config_t cfg = {
        .url = info->url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = false,
        .buffer_size = OTA_CHUNK_SIZE,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init 失败");
        return OTA_OPEN_FAIL;
    }
    esp_http_client_set_header(c, "User-Agent", "curl/8.4.0");

    int rc = OTA_OK;
    esp_ota_handle_t ota = 0;
    uint8_t *buf = NULL;
    bool ota_started = false;
     // 打开下载流
    if (esp_http_client_open(c, 0) != ESP_OK) {
        ESP_LOGE(TAG, "打开下载流失败: %s", info->url);
        rc = OTA_OPEN_FAIL;
        goto clean;
    }
    esp_http_client_fetch_headers(c);
    if (esp_http_client_get_status_code(c) != 200) {
        ESP_LOGE(TAG, "HTTP 状态码 %d", esp_http_client_get_status_code(c));
        rc = OTA_OPEN_FAIL;
        goto clean;
    }

    buf = (uint8_t *)heap_caps_malloc(OTA_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "内部 RAM 分配 %d 字节失败", OTA_CHUNK_SIZE);
        rc = OTA_NO_MEM;
        goto clean;
    }
    ESP_LOGI(TAG, "写入缓冲 %d 字节，位于 %s", OTA_CHUNK_SIZE,
             esp_ptr_internal(buf) ? "内部 RAM" : "PSRAM(异常!)");

    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota); //开始写入
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(err));
        rc = OTA_BEGIN_FAIL;
        goto clean;
    }
    ota_started = true;

    uint64_t written = 0;
    int last_pct = -1;
    int since_yield = 0;
    for (;;) {
        int r = esp_http_client_read(c, (char *)buf, OTA_CHUNK_SIZE);
        if (r < 0) {
            ESP_LOGE(TAG, "读取失败（已写 %llu 字节）", (unsigned long long)written);
            rc = OTA_READ_FAIL;
            goto clean;
        }
        if (r == 0) {
            break;      // 流结束
        }
        err = esp_ota_write(ota, buf, (size_t)r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write 失败: %s（已写 %llu 字节）",
                     esp_err_to_name(err), (unsigned long long)written);
            rc = OTA_WRITE_FAIL;
            goto clean;
        }
        written += (uint64_t)r;

        if (++since_yield >= OTA_YIELD_EVERY_CHUNKS) {
            since_yield = 0;
            vTaskDelay(pdMS_TO_TICKS(OTA_YIELD_MS));
        }
        //OTA进度条计算
        int pct = (int)(written * 100 / info->size);
        if (pct != last_pct) {
            last_pct = pct;
            ESP_LOGI(TAG, "写入 %3d%%  (%llu/%lu 字节)", pct,
                     (unsigned long long)written, (unsigned long)info->size);
            ota_post_progress(pct); //向 UI 发送 OTA 进度消息
        }
    }
    ESP_LOGI(TAG, "下载并写入完成，共 %llu 字节", (unsigned long long)written);

    if (written != (uint64_t)info->size) {
        ESP_LOGE(TAG, "大小不符：清单声明 %lu，实际收到 %llu",
                 (unsigned long)info->size, (unsigned long long)written);
        rc = OTA_SIZE_MISMATCH;
        goto clean;
    }

    // 校验镜像：下载被截断、写坏、或者根本不是固件，都在这里被挡下来
    err = esp_ota_end(ota);
    ota_started = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end 失败: %s（镜像校验没过）", esp_err_to_name(err));
        rc = OTA_VERIFY_FAIL;
        goto clean;
    }

    // ---- 回读校验：写进去的固件，版本号是不是清单上那个 防"无限升级循环"的
    {
        esp_app_desc_t desc;
        esp_err_t re = esp_partition_read(part, 0x20, &desc, sizeof(desc));
        if (re == ESP_OK && desc.magic_word == ESP_APP_DESC_MAGIC_WORD) {
            if (strcmp(desc.version, info->version) != 0) {
                ESP_LOGE(TAG, "版本对不上：写进去的是 '%s'，清单声明的是 '%s'",
                         desc.version, info->version);
                ESP_LOGE(TAG, "多半是忘了先改工程根的 version.txt 再编译。");
                ESP_LOGE(TAG, "拒绝切换分区 —— 否则设备会陷入无限升级循环。");
                rc = OTA_VERSION_MISMATCH;
                goto clean;
            }
            ESP_LOGI(TAG, "回读校验通过：目标分区里的版本 = %s", desc.version);
        } else {
            ESP_LOGW(TAG, "回读 app 描述符失败(%s)，跳过版本校验", esp_err_to_name(re));
        }
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition 失败: %s", esp_err_to_name(err));
        rc = OTA_SETBOOT_FAIL;
        goto clean;
    }

    ESP_LOGW(TAG, "======================================");
    ESP_LOGW(TAG, " 升级就绪：下次启动走 %s", part->label);
    ESP_LOGW(TAG, " 3 秒后重启。新固件必须在 60 秒内自检通过，");
    ESP_LOGW(TAG, " 否则下一次重启会自动回滚。");
    ESP_LOGW(TAG, "======================================");
    if (buf != NULL) {
        free(buf);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return OTA_OK;  

clean:
    if (ota_started) {
        esp_ota_abort(ota);
        ESP_LOGW(TAG, "已中止本次写入，旧固件不受影响");
    }
    if (buf != NULL) {
        free(buf);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return rc;
}

/* ======================= 任务体 ======================= */
// OTA 任务运行模式
typedef enum {
    OTA_MODE_CHECK = 0, // 检查更新
    OTA_MODE_UPGRADE,   // 执行升级
} ota_mode_t;

static volatile bool s_running; // 运行标志：防止同一时刻启动多个 OTA 任务
/**
  * @brief  OTA 任务主体：按模式执行检查或升级
  * @param  arg 模式（通过 intptr_t 传入）
  * @retval 无（任务结束前自删除）
  */
static void task_ota(void *arg)
{
    const ota_mode_t mode = (ota_mode_t)(int)(intptr_t)arg;
    // 打印本地版本与当前运行分区
    const esp_app_desc_t *me = esp_app_get_description();
    ESP_LOGI(TAG, "[%s] 本地版本 = %s（分区 %s）",
             mode == OTA_MODE_CHECK ? "检查" : "升级",
             me->version, esp_ota_get_running_partition()->label);

    if (!wifi_is_connected()) {
        ESP_LOGE(TAG, "没连上 WiFi");
        ota_post_msg(UI_MSG_OTA_FAILED, OTA_NO_WIFI);
        goto out;
    }

    // ---- 升级模式：直接用上次检查的结果，不再拉清单 ----
    if (mode == OTA_MODE_UPGRADE) {
        if (!s_info_valid) {
            ESP_LOGE(TAG, "没有可用的清单（应该先检查一次）");
            ota_post_msg(UI_MSG_OTA_FAILED, OTA_PARSE_FAIL);
            goto out;
        }
        int rc = ota_perform_update(&s_info);
        ESP_LOGE(TAG, "升级失败，结果码 %d（旧固件不受影响）", rc);
        ota_post_msg(UI_MSG_OTA_FAILED, rc);
        goto out;
    }

    // ---- 检查模式 ----
    static char body[OTA_MANIFEST_BUF];   // static：不占任务栈
    int n = http_get_small(OTA_MANIFEST_URL, body, sizeof(body));
    if (n <= 0) {
        ESP_LOGE(TAG, "拉取清单失败（返回 %d）", n);
        ota_post_msg(UI_MSG_OTA_FAILED, OTA_FETCH_FAIL);
        goto out;
    }
    ESP_LOGI(TAG, "清单 %d 字节: %s", n, body);
    // 解析清单
    ota_info_t info;
    if (!parse_manifest(body, &info)) {
        ESP_LOGE(TAG, "清单解析失败");
        ota_post_msg(UI_MSG_OTA_FAILED, OTA_PARSE_FAIL);
        goto out;
    }
    ESP_LOGI(TAG, "远端版本 = %s，大小 = %lu 字节",
             info.version, (unsigned long)info.size);

    if (version_cmp(info.version, me->version) <= 0) {
        ESP_LOGI(TAG, "已是最新（本地 %s >= 远端 %s）", me->version, info.version);
        ota_post_msg(UI_MSG_OTA_LATEST, OTA_OK);
        goto out;
    }

    ESP_LOGW(TAG, "发现新版本: %s -> %s", me->version, info.version);
    // 存下来给升级动作用
    s_info = info;
    s_info_valid = true;
    ota_post_found(&info);

out:
    s_running = false;
    vTaskDelete(NULL);
}
/**
  * @brief  启动一次 OTA 任务（带运行中互斥）
  * @param  mode 运行模式（检查/升级）
  * @retval true 启动成功；false 已有任务在运行或创建失败
  */
static bool ota_start(ota_mode_t mode)
{
    if (s_running) {
        ESP_LOGW(TAG, "上一次 OTA 任务还没结束，忽略本次请求");
        return false;
    }
    s_running = true;

    BaseType_t ok = xTaskCreatePinnedToCore(task_ota, "task_ota", 8 * 1024,
                                            (void *)(intptr_t)mode, 5, NULL, 1);
    if (ok != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "创建 OTA 任务失败");
        return false;
    }
    return true;
}

/**
 * @brief 启动一次"检查更新"任务（只拉清单比对版本，不下载）。
 * @return true 启动成功；false 已有 OTA 任务在运行或创建失败
 */
bool ui_ota_check_start(void)
{
    return ota_start(OTA_MODE_CHECK);
}

/**
 * @brief 启动一次"下载并升级"任务（拉清单 → 下载 → 写入 → 切换分区 → 重启）。
 * @return true 启动成功；false 已有 OTA 任务在运行或创建失败
 */
bool ui_ota_upgrade_start(void)
{
    return ota_start(OTA_MODE_UPGRADE);
}

/**
 * @brief 查询是否已有 OTA 任务在运行。
 * @return true 正在检查或升级中
 */
bool ui_ota_busy(void)
{
    return s_running;
}
