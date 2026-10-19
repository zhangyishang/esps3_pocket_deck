#include "weather_task.h"
#include "ui_msg.h"
#include "app_events.h"
#include "wifi_net.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "weather";

/* ======================= 数据源 ======================= */

#define WEATHER_URL "http://wttr.in/?format=%l%7C%C%7C%t%7C%h%7C%w%7C%p"

#define WEATHER_BUF_SIZE 320

#define WEATHER_TIMEOUT_MS 10000

/* ======================= 状况文本 → 中文 ======================= */

typedef struct {
    const char *key;
    const char *zh;
} weather_cond_map_t;

/**
 * @brief 英文天气状况关键词到中文的映射表（按顺序匹配，先命中先用）。
 */
static const weather_cond_map_t s_cond_map[] = {
    { "thunder",  "雷阵雨" },
    { "sleet",    "雨夹雪" },
    { "blizzard", "暴雪"   },
    { "snow",     "雪"     },
    { "ice",      "冰"     },
    { "drizzle",  "毛毛雨" },
    { "heavy rain", "大雨" },
    { "rain",     "雨"     },
    { "fog",      "雾"     },
    { "mist",     "薄雾"   },
    { "haze",     "霾"     },
    { "smoke",    "烟霾"   },
    { "dust",     "浮尘"   },
    { "overcast", "阴"     },
    { "cloud",    "多云"   },
    { "sunny",    "晴"     },
    { "clear",    "晴"     },
};

/**
 * @brief 不区分大小写地在 hay 中查找子串 needle。
 * @param hay    被搜索的字符串
 * @param needle 要查找的子串（应为小写）
 * @return true 找到；false 未找到或参数非法
 */
static bool contains_ci(const char *hay, const char *needle)
{
    if (hay == NULL || needle == NULL) {
        return false;
    }
    for (; *hay; hay++) {
        const char *h = hay;
        const char *n = needle;
        while (*h && *n &&
               ((*h >= 'A' && *h <= 'Z') ? (*h + 32) : *h) == *n) {
            h++;
            n++;
        }
        if (*n == '\0') {
            return true;
        }
    }
    return false;
}

/**
 * @brief 将英文天气状况转换为中文描述。
 * @param en 英文状况字符串
 * @return 对应中文；匹配不到则返回原英文；输入为空返回"未知"
 */
static const char *cond_to_zh(const char *en)
{
    if (en == NULL || en[0] == '\0') {
        return "未知";
    }
    for (size_t i = 0; i < sizeof(s_cond_map) / sizeof(s_cond_map[0]); i++) {
        if (contains_ci(en, s_cond_map[i].key)) {
            return s_cond_map[i].zh;
        }
    }
    return en;
}

/* ======================= 字段切分与清洗 ======================= */

/**
 * @brief 按 '|' 分割字符串为多个字段（就地修改）。
 * @param s   待切分的输入字符串
 * @param out 输出字段指针数组
 * @param max 最多接受的字段数
 * @return 实际切分出的字段数
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
 * @brief 去除首尾空白并丢弃开头的非 ASCII 装饰字节。
 * @param src 源字符串
 * @return 指向内部静态缓冲的清洗结果字符串
 */
static char *trim_and_strip(const char *src)
{
    static char buf[64];
    size_t i = 0;

    if (src == NULL) {
        buf[0] = '\0';
        return buf;
    }

    while (*src != '\0' && (unsigned char)*src >= 0x80) {
        src++;
    }
    while (*src == ' ' || *src == '\t') {
        src++;
    }

    size_t len = strlen(src);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' ||
                       src[len - 1] == '\r' || src[len - 1] == '\n')) {
        len--;
    }
    for (; i < len && i < sizeof(buf) - 1; i++) {
        buf[i] = src[i];
    }
    buf[i] = '\0';
    return buf;
}

/**
 * @brief 安全拷贝字符串到定长字段。
 * @param dst 目标字段
 * @param cap 目标容量
 * @param src 源字符串（可为 NULL）
 */
static void set_field(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    strlcpy(dst, (src != NULL) ? src : "", cap);
}

/* ======================= 抓取 ======================= */

/**
 * @brief 从 wttr.in 抓取并解析天气数据。
 * @param out 输出解析结果
 * @return weather_result_t 结果码
 */
static int weather_fetch(weather_data_t *out)
{
    if (!wifi_is_connected()) {
        return WEATHER_NO_WIFI;
    }

    const esp_http_client_config_t cfg = {
        .url = WEATHER_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = WEATHER_TIMEOUT_MS,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init 失败");
        return WEATHER_CONNECT_FAIL;
    }

    esp_http_client_set_header(client, "User-Agent", "curl/8.4.0");

    int result = WEATHER_OK;
    char *fields[8] = {0};

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接失败: %s", esp_err_to_name(err));
        result = WEATHER_CONNECT_FAIL;
        goto out;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP 状态码 %d", status);
        result = WEATHER_HTTP_STATUS;
        goto out;
    }
    (void)content_len;

    static char body[WEATHER_BUF_SIZE];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = esp_http_client_read(client, body + total, sizeof(body) - 1 - total);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    body[total] = '\0';

    if (total <= 0) {
        ESP_LOGE(TAG, "没读到任何内容");
        result = WEATHER_CONNECT_FAIL;
        goto out;
    }

    ESP_LOGI(TAG, "响应 %d 字节: %s", total, body);

    int n = split_fields(body, fields, (int)(sizeof(fields) / sizeof(fields[0])));
    if (n < 6) {
        ESP_LOGE(TAG, "字段数不对：期望 6，实际 %d", n);
        result = WEATHER_PARSE_FAIL;
        goto out;
    }

    if (fields[0][0] == '\0' || strpbrk(fields[2], "0123456789") == NULL) {
        ESP_LOGE(TAG, "字段内容不合理（地名空或者温度没数字）");
        result = WEATHER_PARSE_FAIL;
        goto out;
    }

    set_field(out->city, sizeof(out->city), trim_and_strip(fields[0]));
    set_field(out->cond, sizeof(out->cond), cond_to_zh(trim_and_strip(fields[1])));
    set_field(out->temp, sizeof(out->temp), trim_and_strip(fields[2]));
    set_field(out->humid, sizeof(out->humid), trim_and_strip(fields[3]));
    set_field(out->wind, sizeof(out->wind), trim_and_strip(fields[4]));

out:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

/* ======================= 任务体 ======================= */

/**
 * @brief 是否有一次抓取正在跑（防止重复创建任务）。
 */
static volatile bool s_running;

/**
 * @brief 天气任务主体：抓取一次天气，通过 UI 消息上报结果（成功带数据块，
 *        失败带错误码），然后自删除。
 * @param arg 任务参数（未使用）
 */
static void task_weather(void *arg)
{
    weather_data_t data = {0};

    int result = weather_fetch(&data);

    if (result == WEATHER_OK) {
        ui_msg_t msg = { .id = UI_MSG_WEATHER_RESULT };
        memcpy(msg.blob, &data, sizeof(data));
        ui_msg_send(&msg, portMAX_DELAY);
    } else {
        ui_msg_t msg = { .id = UI_MSG_WEATHER_FAILED };
        msg.v.i[0] = result;
        ui_msg_send(&msg, portMAX_DELAY);
    }
    s_running = false;
    vTaskDelete(NULL);
}

/**
 * @brief 启动一次天气抓取任务（带运行中互斥，6KB 栈，优先级 5，绑核 1）。
 * @return true 启动成功；false 已有任务在跑或创建失败
 */
bool ui_weather_task_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "上一次抓取还没结束，忽略本次请求");
        return false;
    }
    s_running = true;

    BaseType_t ok = xTaskCreatePinnedToCore(task_weather, "task_weather",6 * 1024, NULL, 5, NULL, 1);
    if (ok != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "创建天气任务失败");
        return false;
    }
    return true;
}
