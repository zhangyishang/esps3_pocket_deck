#include "xiaozhi_task.h"
#include "ui_msg.h"
#include "app_events.h"
#include "hal_audio.h"
#include "hal_pca9557.h"
#include "wifi_net.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <opus.h>

static const char *TAG = "xiaozhi";

/* ==================== 服务器配置 ==================== */


#define XZ_USE_OFFICIAL     1

#define XZ_OTA_URL       "https://api.tenclass.net/xiaozhi/ota/"


/** 激活码轮询间隔（用户输完码之前一直轮询）。 */
#define XZ_ACTIVATION_POLL_MS   30000   /* 30 秒一次：太频繁会触发服务端按 IP 限流 */

/** 激活轮询总时长（官方给的 timeout_ms 一般是 30 分钟，这里按 3 分钟够用了）。 */
#define XZ_ACTIVATION_TRIES     40    /* 30s × 40 = 20 分钟 */

#define XZ_TX_RATE       16000   /* 上行（麦克风->服务端）：codec 原生，服务端已接受 */
#define XZ_RX_RATE       24000   /* 下行（服务端->喇叭）：tts start 里明确写 sample_rate=24000 */
#define XZ_SAMPLE_RATE   XZ_TX_RATE
#define XZ_FRAME_MS      60
#define XZ_FRAME_SAMPLES (XZ_TX_RATE * XZ_FRAME_MS / 1000)   /* 16000*60/1000 = 960 帧 */
#define XZ_RX_SAMPLES    (XZ_RX_RATE * XZ_FRAME_MS / 1000)   /* 24000*60/1000 = 1440 帧 */
#define XZ_MAX_SAMPLES   (48000 * XZ_FRAME_MS / 1000)   /* 自适应上限：48000*60/1000 = 2880 */
/** 服务端 TTS 解出来的幅度很小（实测峰值 30~500，满量程 32767），播放前做固定增益。 */
#define XZ_DEFAULT_VOLUME  50   /* 默认音量 0~100 */

#define XZ_TASK_STACK    (14 * 1024)   /* 必须在内部 RAM：要做 NVS/TLS 读 Flash */
#define XZ_RX_TASK_STACK (12 * 1024)  /* 回调里要做 JSON 解析，8KB 偏紧 */
#define XZ_REC_TASK_STACK (32 * 1024)  /* opus_encode 要 20~30KB，栈在 PSRAM */

#define XZ_NVS_NAMESPACE "xz"
#define XZ_NVS_KEY_TOKEN "token"
#define XZ_NVS_KEY_URL   "url"
#define XZ_NVS_KEY_CID   "cid2"   /* Client-Id：官方要求是 UUID，不是 MAC */

/* ==================== 运行时状态 ==================== */

static xz_state_t   s_state = XZ_STATE_IDLE;
static esp_websocket_client_handle_t s_ws = NULL;
static OpusEncoder *s_enc = NULL;
static OpusDecoder *s_dec = NULL;
static volatile bool s_running = false;

#define XZ_PKT_MB_SIZE   (16 * 1024)
static MessageBufferHandle_t s_pkt_mb = NULL;
static StaticMessageBuffer_t s_pkt_mb_struct;
static uint8_t *s_pkt_mb_buf = NULL;

static volatile bool s_recording = false;
static volatile uint32_t s_uploaded = 0;
static volatile uint32_t s_played   = 0;
static volatile int s_cur_volume = XZ_DEFAULT_VOLUME;

static volatile int s_play_rate = 16000;
static volatile bool s_dec_need_rebuild = false;
static volatile bool s_rec_exit  = false;  /* 让 record_task 退出 */
static volatile bool s_rec_gone  = false;  /* record_task 已真正退出 */
static volatile bool s_rec_start_pending = false;
static volatile bool s_rec_stop_pending  = false;

static int16_t *s_pcm  = NULL;      /* 单声道 PCM 工作区（PSRAM），录音/播放共用 */
static int16_t *s_dec_pcm = NULL;   /* 24kHz 解码结果（PSRAM），再抽取到 16kHz */
static uint8_t *s_opus = NULL;      /* Opus 输出区（PSRAM） */

static char s_client_id[40];   /* 形如 8-4-4-4-12 的 UUID */
static char s_ws_url[192];
static char s_token[160];

/**
 * @brief 上报状态给 UI。
 * @param st 新状态
 */
static void report_state(xz_state_t st)
{
    s_state = st;
    ui_msg_t msg = { .id = UI_MSG_XZ_STATE };
    msg.v.i[0] = (int)st;
    ui_msg_send(&msg, 0);
}

/**
 * @brief 上报错误。
 * @param code xz_error_t
 */
static void report_error(xz_error_t code)
{
    ui_msg_t msg = { .id = UI_MSG_XZ_FAILED };
    msg.v.i[0] = (int)code;
    ui_msg_send(&msg, 0);
}

/**
 * @brief 把一段文本作为字幕发给 UI。
 * @param id   消息类型
 * @param text UTF-8 文本
 */
static void report_text(ui_msg_id_t id, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    ui_msg_t msg = { .id = id };
    strlcpy((char *)msg.blob, text, sizeof(msg.blob));
    ui_msg_send(&msg, 0);
}

/**
 * @brief 把一行诊断文字同时打到串口和屏幕上（不接串口也能看病）。
 * @param fmt printf 风格格式串
 */
static void report_debug(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "[诊断] %s", buf);
    report_text(UI_MSG_XZ_DEBUG, buf);
}

/* ==================== NVS：存 token / ws url ==================== */

/**
 * @brief 从 NVS 读一个字符串。
 * @param key 键
 * @param out 输出
 * @param cap 容量
 * @return true 读到
 */
static bool nvs_get_str_(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(XZ_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    esp_err_t r = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return (r == ESP_OK && len > 1);
}

/**
 * @brief 往 NVS 写一个字符串。
 * @param key 键
 * @param val 值
 */
static void nvs_set_str_(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(XZ_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

/**
 * @brief 首次启动生成一个 UUID 当 Client-Id（官方 ota.cc 用的是 board.GetUuid()，
 *        不是 MAC —— 传 MAC 会被服务器判成非法客户端，返回 400）。
 *        生成后存 NVS，之后一直复用。
 */
/**
 * @brief 首次启动生成一个 UUID 当 Client-Id。
 *
 * 为什么必须是 UUID：官方 ota.cc 用的是 board.GetUuid()，传 MAC 会被服务器
 * 判成非法客户端，直接返回 400。
 *
 * 为什么用 esp_random 而不是从 MAC 推导：服务器对**同一个 Client-Id 只下发一次
 * 激活码**，码丢了就再也拿不到。用随机 UUID + 换 NVS 键名，就等于把设备当成
 * 一台"全新的"，服务器会重新下发验证码。
 */
static void xz_ensure_client_id(void)
{
    if (s_client_id[0] != '\0') {
        return;
    }

    /* ① 老键 "cid" 里存的是**当初绑定设备时用的那个 ID**，必须优先用它 ——
     *    服务器按 Client-Id 记住这台设备属于哪个账号，换了 ID 就认不出来，
     *    只会回 test-token。 */
    if (nvs_get_str_("cid", s_client_id, sizeof(s_client_id))) {
        ESP_LOGI(TAG, "复用已绑定的 Client-Id: %s", s_client_id);
        return;
    }
    /* ② 新键 cid2：随机生成的（用于需要换身份重新激活的场景） */
    if (nvs_get_str_(XZ_NVS_KEY_CID, s_client_id, sizeof(s_client_id))) {
        ESP_LOGI(TAG, "复用 Client-Id: %s", s_client_id);
        return;
    }
    /* ③ 都没有：由 MAC 推导一个**稳定**的 UUID。
     *    不能用 esp_random —— 那样每次清 NVS 都变成新设备，绑定关系就丢了。 */
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(s_client_id, sizeof(s_client_id),
             "%02x%02x%02x%02x-%02x%02x-4%02x%x-8%02x%x-%02x%02x%02x%02x%02x%02x",
             m[0], m[1], m[2], m[3],
             m[4], m[5],
             m[0], m[1] & 0x0F,
             m[2], m[3] & 0x0F,
             m[4], m[5], m[0], m[1], m[2], m[3]);
    nvs_set_str_(XZ_NVS_KEY_CID, s_client_id);
    ESP_LOGI(TAG, "由 MAC 推导 Client-Id: %s", s_client_id);
}

/* ==================== JSON 小工具 ==================== */

/**
 * @brief 取 JSON 里某个键的字符串值（够用就行）。
 * @param json 源
 * @param key  形如 "\"code\"" 的键
 * @param out  输出
 * @param cap  容量
 * @return true 找到
 */
static bool json_get_str(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = strstr(json, key);
    if (p == NULL) {
        return false;
    }
    p = strchr(p + strlen(key), ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '"') {
        p++;
    }
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap) {
        out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0;
}

/* ==================== 官方云：激活 / 取 token ==================== */

/**
 * @brief 请求官方 OTA 接口，拿激活码或 WebSocket 地址 + token。
 *
 * 严格按官方 ota.cc 的格式来：
 *   Header: Activation-Version: 1
 *           Device-Id: <MAC>       Client-Id: <MAC>
 *           Accept-Language: zh-CN Content-Type: application/json
 *   POST body: {}
 *   响应: {"activation":{"code":"123456","message":"..."},
 *          "websocket":{"url":"wss://...","token":"..."}}
 *
 * @return true 已拿到可用的 ws url 和 token
 */
#if XZ_USE_OFFICIAL
static bool xz_fetch_ota(void)
{
    char mac[18] = {0};
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);

    esp_http_client_config_t cfg = {
        .url = XZ_OTA_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* 官方接口是 HTTPS */
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        report_debug("http_init 失败");
        return false;
    }
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "Activation-Version", "1");
    esp_http_client_set_header(cli, "Device-Id", mac);
    xz_ensure_client_id();
    esp_http_client_set_header(cli, "Client-Id", s_client_id);
    esp_http_client_set_header(cli, "User-Agent", "esp-handheld/1.0");
    esp_http_client_set_header(cli, "Accept-Language", "zh-CN");

    char *body = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        esp_http_client_cleanup(cli);
        return false;
    }
    body[0] = '\0';

    esp_err_t err = esp_http_client_open(cli, 2);
    if (err != ESP_OK) {
        report_debug("连不上服务器: %s", esp_err_to_name(err));
        heap_caps_free(body);
        esp_http_client_cleanup(cli);
        return false;
    }
    esp_http_client_write(cli, "{}", 2);
    esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    int n = esp_http_client_read(cli, body, 4095);
    body[(n > 0) ? n : 0] = '\0';
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (status == 200) {
        report_debug("已连上服务端 (HTTP 200)");
    } else {
        report_debug("服务端返回 HTTP %d", status);
    }
    if (n > 0) {
        ESP_LOGI(TAG, "响应: %s", body);
    }

    bool ok = false;
    if (status == 200 && n > 0) {
        cJSON *root = cJSON_Parse(body);
        if (root == NULL) {
            report_debug("响应不是 JSON: %.60s", body);
        } else {
            /* ① 先看有没有激活码。有就说明还没绑定 —— 这时服务器虽然也会
             *    返回 websocket.url，但 token 是占位值 "test-token"，
             *    拿它去连必然被拒。所以只显示验证码、不要连。 */
            bool need_activation = false;
            cJSON *act = cJSON_GetObjectItem(root, "activation");
            if (cJSON_IsObject(act)) {
                cJSON *code = cJSON_GetObjectItem(act, "code");
                cJSON *msg  = cJSON_GetObjectItem(act, "message");
                if (cJSON_IsString(code) && code->valuestring[0] != '\0') {
                    need_activation = true;
                    ESP_LOGW(TAG, "需要激活，验证码: %s", code->valuestring);
                    report_text(UI_MSG_XZ_ACTIVATION, code->valuestring);
                }
                if (cJSON_IsString(msg)) {
                    ESP_LOGW(TAG, "服务器提示: %s", msg->valuestring);
                }
            }

            if (need_activation) {
                report_state(XZ_STATE_CONNECTING);   /* UI 上显示"等待激活…" */
            } else {
                /* ② 没激活码 = 已绑定，这时 token 才是真的 */
                cJSON *ws = cJSON_GetObjectItem(root, "websocket");
                if (cJSON_IsObject(ws)) {
                    cJSON *url   = cJSON_GetObjectItem(ws, "url");
                    cJSON *token = cJSON_GetObjectItem(ws, "token");
                    /* 以前这里排除了 test-token，但从没验证过它到底能不能连。
                     * 改成：只要给了地址就试连 —— 连不上 WS 会报错，比一直干等有用。 */
                    if (cJSON_IsString(url) && url->valuestring[0] != '\0') {
                        strlcpy(s_ws_url, url->valuestring, sizeof(s_ws_url));
                        strlcpy(s_token, cJSON_IsString(token) ? token->valuestring : "", sizeof(s_token));
                        nvs_set_str_(XZ_NVS_KEY_URL, s_ws_url);
                        nvs_set_str_(XZ_NVS_KEY_TOKEN, s_token);
                        ESP_LOGI(TAG, "拿到 WebSocket 地址: %s", s_ws_url);
                        ok = true;
                    } else if (cJSON_IsString(token)) {
                        ESP_LOGW(TAG, "token 还是占位值（%s），继续等激活", token->valuestring);
                    }
                }
            }
            cJSON_Delete(root);
        }
    }

    heap_caps_free(body);
    return ok;
}
#else  /* !XZ_USE_OFFICIAL：自建模式不需要激活接口 */
static bool xz_fetch_ota(void)
{
    return false;
}
#endif

/* ==================== WebSocket 事件 ==================== */

/**
 * @brief WebSocket 事件回调。
 * @param handler 句柄
 * @param base    事件基
 * @param id      事件 id
 * @param data    事件数据
 */
static void ws_event_cb(void *handler, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
    {
        ESP_LOGI(TAG, "WebSocket 已连接");
        /* hello 必须在连接建立**之后**发。
         * 字段对齐官方 websocket_protocol.cc:GetHelloMessage()：
         *   type / version / features / transport / audio_params
         * 官方没有 mode 字段，aec 只在启用服务端 AEC 时才加 —— 我都不加。
         * 这里 mcp 填 false：我们没实现 MCP 协议，填 true 服务端会不断下发
         * mcp initialize 之类的消息（虽然不影响音频，但纯属噪音）。 */
        static const char hello[] =
            "{\"type\":\"hello\",\"version\":1,"
            "\"features\":{\"mcp\":false},"
            "\"transport\":\"websocket\","
            "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
            "\"channels\":1,\"frame_duration\":60}}";
        esp_websocket_client_send_text(s_ws, hello, strlen(hello), pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "已发送 hello: %s", hello);
        report_state(XZ_STATE_READY);
        break;
    }

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket 断开");
        report_state(XZ_STATE_IDLE);
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket 连不上");
        report_error(XZ_ERR_NETWORK);
        break;

    case WEBSOCKET_EVENT_DATA:
        /* 打印收到的帧。
         * ⚠️ 绝对不能用 %.400s 打 ev->data_ptr —— 那是裸字节缓冲区，
         * 不一定以 '\0' 结尾，用 %s 会一直往后读、越界踩坏堆
         * （实测会崩在 LVGL 刷屏的 SPI DMA 分配里）。
         * 正确做法：先按 data_len 拷进本地缓冲并补 0。 */
        {
            char frame[256];
            size_t fn = (size_t)ev->data_len;
            if (fn > sizeof(frame) - 1) {
                fn = sizeof(frame) - 1;
            }
            if (fn > 0 && ev->data_ptr != NULL) {
                memcpy(frame, ev->data_ptr, fn);
            }
            frame[fn] = '\0';
            if (fn > 0) {
                ESP_LOGI(TAG, "收到帧 op_code=0x%x len=%d: %s",
                         ev->op_code, ev->data_len, frame);
            }
        }

        if (ev->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            char buf[512];
            size_t n = ev->data_len < sizeof(buf) - 1 ? ev->data_len : sizeof(buf) - 1;
            memcpy(buf, ev->data_ptr, n);
            buf[n] = '\0';

            if (strstr(buf, "\"tts\"") != NULL) {
                if (strstr(buf, "\"start\"") != NULL) {
                    /* 自适应采样率：取出服务端声明的 sample_rate，
                     * 播放时按它切 codec（和音乐播放器的 _audio_player_std_clock 一样）。 */
                    {
                        const char *psr = strstr(buf, "\"sample_rate\"");
                        if (psr != NULL) {
                            const char *pc = strchr(psr, ':');
                            if (pc != NULL) {
                                int sr = atoi(pc + 1);
                                /* 只记录，不跟随。
                                 * 官方固件固定 16000，服务端声明值仅作参考。 */
                                if (sr != s_play_rate) {
                                    ESP_LOGW(TAG, "服务端声明 TTS 采样率 %d Hz（本机固定用 %d）",
                                             sr, s_play_rate);
                                }
                            }
                        }
                    }
                    report_state(XZ_STATE_SPEAKING);
                } else if (strstr(buf, "\"stop\"") != NULL) {
                    report_state(XZ_STATE_IDLE);
                } else if (strstr(buf, "sentence_start") != NULL) {
                    char text[200];
                    if (json_get_str(buf, "\"text\"", text, sizeof(text))) {
                        report_text(UI_MSG_XZ_TTS_TEXT, text);
                    }
                }
            } else if (strstr(buf, "\"stt\"") != NULL) {
                char text[200];
                if (json_get_str(buf, "\"text\"", text, sizeof(text))) {
                    report_text(UI_MSG_XZ_STT, text);
                }
            } else if (strstr(buf, "\"llm\"") != NULL) {
                char text[200];
                if (json_get_str(buf, "\"text\"", text, sizeof(text))) {
                    report_text(UI_MSG_XZ_TTS_TEXT, text);
                }
            }
        } else if (ev->op_code == WS_TRANSPORT_OPCODES_BINARY) {
            /* 服务端下发的 TTS 音频。
             * ⚠️ 这里**不能**直接 opus_decode —— 本回调运行在 websocket_task 里，
             * 该任务栈由库内部创建（8KB），而 opus_decode 需要 20~30KB，
             * 会触发 stack overflow（实测崩过）。
             * 只把包塞进消息缓冲区，解码和播放交给 xz_rec（栈在 PSRAM）。 */
            if (s_pkt_mb != NULL && ev->data_len > 0) {
                size_t sent = xMessageBufferSend(s_pkt_mb, ev->data_ptr,
                                                 (size_t)ev->data_len, 0);
                static uint32_t dbg_n = 0;
                if (++dbg_n <= 4) {
                    ESP_LOGW(TAG, ">>> MB SEND #%u: len=%d sent=%d",
                             (unsigned)dbg_n, ev->data_len, (int)sent);
                }
                if (sent == 0) {
                    /* 缓冲满了就丢包，避免阻塞 WS 任务 */
                    static uint32_t drop = 0;
                    if (++drop % 50 == 1) {
                        ESP_LOGW(TAG, "下行缓冲满，丢弃音频包（累计 %u）", (unsigned)drop);
                    }
                }
            }
        }
        break;

    default:
        break;
    }
}

/* ==================== 录音上传 ==================== */

/**
 * @brief 按住期间不断读麦克风 -> Opus 编码 -> 二进制帧发出。
 * @param arg 未使用
 */
static void record_task(void *arg)
{
    /* 常驻任务：只创建一次。
     * 之前是「每按一次按钮就建一个任务」，既要重复申请 16KB 栈，
     * 又在 LVGL 任务里直接调 WebSocket API —— 两个都是踩坏堆的隐患。
     * 现在音频相关的所有动作（发 listen、读麦克风、Opus、上传）全部在这个
     * 任务里串行完成，LVGL 任务只负责置标志。 */
    for (;;) {
        if (s_rec_exit) {
            s_rec_gone = true;
            /* 注意：栈是用 xTaskCreateWithCaps 从 PSRAM 分配的，
             * 必须用对应的 vTaskDeleteWithCaps 才释放，否则每次开关泄漏 32KB。 */
            vTaskDeleteWithCaps(NULL);
            return;
        }
        if (!s_running) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* 开始录音：只有本任务会碰 WebSocket 发送 */
        if (s_rec_start_pending) {
            s_rec_start_pending = false;
            s_recording = true;
            /* 录音要用 16kHz，切回去（播放时是 24kHz） */
            bsp_codec_set_fs(XZ_TX_RATE, 16, (i2s_slot_mode_t)2);
            int rv = 0;
            bsp_codec_volume_set(s_cur_volume, &rv);
            ESP_LOGW(TAG, ">>> REC: send listen start");
            if (s_ws != NULL) {
                const char *start =
                    "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}";
                esp_websocket_client_send_text(s_ws, start, strlen(start),
                                               pdMS_TO_TICKS(500));
            }
        }

        /* 录音中：读一帧 -> 编码 -> 上传 */
        if (s_recording) {
            if (hal_mic_read_mono(s_pcm, XZ_FRAME_SAMPLES, 100) == ESP_OK) {
                int n = opus_encode(s_enc, s_pcm, XZ_FRAME_SAMPLES, s_opus, 512);
                if (n > 0 && s_ws != NULL) {
                    int sent = esp_websocket_client_send_bin(s_ws, (const char *)s_opus, n,
                                                             pdMS_TO_TICKS(500));
                    s_uploaded++;
                    if (s_uploaded % 50 == 1) {
                        ESP_LOGW(TAG, ">>> UPLOAD #%u: opus=%d sent=%d",
                                 (unsigned)s_uploaded, n, sent);
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        /* 停止录音 */
        if (s_rec_stop_pending) {
            s_rec_stop_pending = false;
            s_recording = false;
            ESP_LOGW(TAG, ">>> REC: send listen stop (uploaded %u frames)",
                     (unsigned)s_uploaded);
            if (s_ws != NULL) {
                const char *stop = "{\"type\":\"listen\",\"state\":\"stop\"}";
                esp_websocket_client_send_text(s_ws, stop, strlen(stop),
                                               pdMS_TO_TICKS(500));
            }
        }

        /* 播放：从下行缓冲取 Opus 包 -> 解码 -> 喇叭。
         * 这一步在 record_task 里做，栈在 PSRAM（32KB），够 opus_decode 用。 */
        {
            static bool once = false;
            if (!once) {
                once = true;
                ESP_LOGW(TAG, ">>> MB STATE: mb=%p dec=%p dec_pcm=%p pcm=%p",
                         (void *)s_pkt_mb, (void *)s_dec, (void *)s_dec_pcm, (void *)s_pcm);
            }
        }
        if (s_pkt_mb != NULL && s_dec != NULL && s_dec_pcm != NULL) {
            static uint8_t pkt[1024];
            size_t got = xMessageBufferReceive(s_pkt_mb, pkt, sizeof(pkt), 0);
            if (got > 0) {
                /* 服务端 TTS 是 24kHz，所以按 XZ_RX_SAMPLES 解到 s_dec_pcm */
                /* 自适应采样率：服务端说多少就解多少（和 MP3 播放器的做法一致）。
                 * tts start 里声明了新采样率就重建解码器。 */
                if (false && s_dec_need_rebuild && s_dec != NULL) {
                    opus_decoder_destroy(s_dec);
                    int re = 0;
                    s_dec = opus_decoder_create(s_play_rate, 1, &re);
                    s_dec_need_rebuild = false;
                    ESP_LOGW(TAG, "解码器重建为 %d Hz: %s",
                             s_play_rate, esp_err_to_name(re));
                }
                int frame_samples = (s_play_rate * XZ_FRAME_MS) / 1000;
                if (frame_samples > XZ_MAX_SAMPLES) {
                    frame_samples = XZ_MAX_SAMPLES;
                }
                int dn = opus_decode(s_dec, pkt, (opus_int32)got,
                                     s_dec_pcm, frame_samples, 0);

                if (dn > 0) {
                    /* 解码结果就是 s_play_rate（自适应），原样送喇叭。
                     *
                     * 走过的两条弯路：
                     *   1) 24k 裸抽到 16k：无抗混叠滤波 -> 金属声/沙沙声/齿音刺耳；
                     *   2) 把 codec 切到 24k 直接播：ES8311 的 24k 不对劲 -> 音色变了。
                     * 现在用 16kHz 解码器（libopus 自带高质量重采样），codec 固定 16kHz。 */
                    const int out = dn;
                    if (out > 0) {
                        /* 超时不能给 0！bsp_i2s_write 在 timeout=0 时 I2S DMA 一满就立刻返回
                             * ESP_ERR_TIMEOUT、written=0，结果一帧都发不出去（音乐 App 就是因为
                             * 用了合理超时才有声音）。这里给 200ms，足够 960 样本（60ms）排进去。 */
                        /* ⚠️ 每次播放前都重新解除静音并设音量。
                         * 音乐 App 的 _audio_player_mute_fn() 会在播放/暂停时调
                         * bsp_codec_mute_set()，一旦你开过音乐，codec 就停在静音状态；
                         * 开机时解除的那一次早就被覆盖了。 */
                        if (s_played < 3 || (s_played % 20) == 0) {
                            /* 自适应采样率：和解码器保持一致（同音乐播放器的 std_clock）。
                             * 切失败就退回 16kHz，避免出现"音色变了"。 */
                            /* codec 固定 16kHz，不切了（切 24k 会音色不对）。
                             * 只在第 1 帧确认一次格式。 */
                            if (s_played == 0) {
                                bsp_codec_set_fs(16000, 16, (i2s_slot_mode_t)2);
                            }
                            bsp_codec_mute_set(false);
                            int vv = 0;
                            bsp_codec_volume_set(s_cur_volume, &vv);
                        }
                        /* 直接写解码结果，不做重采样、不加增益 */
                        esp_err_t werr = hal_spk_write_mono(s_dec_pcm, (size_t)out, 200);
                        s_played++;
                        if (s_played % 20 == 1) {
                            /* 顺便算一下峰值：如果是 0，说明解出来就是静音，问题在解码；
                             * 如果峰值正常但还是听不到，问题在喇叭/codec。 */
                            int16_t peak = 0;
                            for (int k = 0; k < out; k++) {
                                int16_t v = s_dec_pcm[k] < 0 ? -s_dec_pcm[k] : s_dec_pcm[k];
                                if (v > peak) peak = v;
                            }
                            ESP_LOGW(TAG, ">>> PLAY #%u: opus=%d pcm=%d wr=%s peak=%d",
                                     (unsigned)s_played, (int)got, out,
                                     esp_err_to_name(werr), (int)peak);
                        }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ==================== 启动：激活 -> 连接 ==================== */

/**
 * @brief 启动任务：先走激活流程，再连 WebSocket、发 hello。
 * @param arg 未使用
 */
static void xz_boot_task(void *arg)
{
    if (!wifi_is_connected()) {
        report_error(XZ_ERR_NO_WIFI);
        vTaskDelete(NULL);
        return;
    }

    /* 先试本地缓存的地址（已激活过就不用每次都问服务器） */
    bool have = nvs_get_str_(XZ_NVS_KEY_URL, s_ws_url, sizeof(s_ws_url));
    if (have) {
        nvs_get_str_(XZ_NVS_KEY_TOKEN, s_token, sizeof(s_token));
        ESP_LOGI(TAG, "用缓存的地址: %s", s_ws_url);
    }

#if !XZ_USE_OFFICIAL
    /* 自建服务端：不用激活，直接用写死的地址 */
    strlcpy(s_ws_url, XZ_HARDCODED_WS, sizeof(s_ws_url));
    strlcpy(s_token, XZ_HARDCODED_TOK, sizeof(s_token));
    have = true;
    ESP_LOGI(TAG, "自建模式，直连 %s", s_ws_url);
#endif

    /* 没缓存，或者缓存过期 -> 问官方接口。
     * 未绑定设备会先拿到激活码，用户到 xiaozhi.me 输完码之后，下一次轮询
     * 才会返回 websocket 地址和 token。 */
    if (!have) {
        for (int i = 0; i < XZ_ACTIVATION_TRIES && !have; i++) {
            ESP_LOGI(TAG, "第 %d 次请求激活接口…", i + 1);
            if (xz_fetch_ota()) {
                have = true;
                break;
            }
            if (!s_running) {
                vTaskDelete(NULL);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(XZ_ACTIVATION_POLL_MS));
        }
    }
    if (!have) {
        report_error(XZ_ERR_NETWORK);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* Opus */
    int oerr = 0;
    s_enc = opus_encoder_create(XZ_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &oerr);
    s_dec = opus_decoder_create(XZ_TX_RATE, 1, &oerr);   /* 用 16kHz 解码器：libopus 内部会自动高质量重采样 */
    if (s_enc == NULL || s_dec == NULL || oerr != OPUS_OK) {
        report_error(XZ_ERR_OPUS);
        vTaskDelete(NULL);
        return;
    }
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(0));  /* 0 最省栈，语音够用 */
    ESP_LOGW(TAG, "opus 初始化后本任务栈剩余 %u 字节（栈共 %d）",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), XZ_TASK_STACK);

    /* 不改 codec 采样率：ES8311/ES7210 + I2S 本来就是 16 kHz，
     * 官方小智固件用的也是 16000。之前按服务端 hello 里的 24000 强改过一次，
     * 结果录音和播放都不正常（没有声音）。 */
    {
        esp_err_t fr = bsp_codec_set_fs(XZ_SAMPLE_RATE, 16, (i2s_slot_mode_t)2);
        ESP_LOGI(TAG, "codec 采样率 %d Hz: %s", XZ_SAMPLE_RATE, esp_err_to_name(fr));

        /* ⚠️⚠️ 最关键的一步：打开功放（PA）！
         *
         * PA 使能挂在 PCA9557 的 IO1 上（PA_EN_GPIO = BIT(1)），
         * hal_pca9557.c 里上电默认写的是 "PA_EN = 0"，也就是**默认关闭**。
         * 音乐 App 每次播放前都会 pa_en(1)，退出时 pa_en(0)。
         *
         * 我之前的代码从来没调用过 pa_en(1) —— codec 的模拟输出根本没被功放放大，
         * 所以无论写多少数据、音量设多高，都是一个字：静音。
         * 这就是"写入返回 ESP_OK 却完全没有声音"的真正原因。 */
        pa_en(1);

        /* 再解除静音并设音量。
         * 音乐 App 的 _audio_player_mute_fn() 会调 bsp_codec_mute_set()，
         * 退出音乐后 codec 很可能停在静音状态。 */
        esp_err_t mret = bsp_codec_mute_set(false);
        int vol = 0;
        esp_err_t vret = bsp_codec_volume_set(s_cur_volume, &vol);
        ESP_LOGW(TAG, "pa_en(1) | 解除静音: %s | 音量设 100 -> 实际 %d (%s)",
                 esp_err_to_name(mret), vol, esp_err_to_name(vret));

    }

    /* WebSocket */
    esp_websocket_client_config_t cfg = {
        .uri = s_ws_url,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .buffer_size = 4096,
        .task_stack = XZ_RX_TASK_STACK,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* wss:// 要证书 */
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) {
        report_error(XZ_ERR_NETWORK);
        vTaskDelete(NULL);
        return;
    }
    {
        /* ⚠️ 这三个头是关键：服务端靠 Device-Id + Client-Id 认设备，
         * 缺了它无法把这条 WS 连接关联到账号，会直接下发 CLOSE 帧。
         * 对照官方 websocket_protocol.cc:104-106。 */
        char mac[18] = {0};
        uint8_t m[6] = {0};
        esp_read_mac(m, ESP_MAC_WIFI_STA);
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 m[0], m[1], m[2], m[3], m[4], m[5]);

        xz_ensure_client_id();
        esp_websocket_client_append_header(s_ws, "Protocol-Version", "1");
        esp_websocket_client_append_header(s_ws, "Device-Id", mac);
        esp_websocket_client_append_header(s_ws, "Client-Id", s_client_id);
        ESP_LOGI(TAG, "WS 头: Protocol-Version=1 Device-Id=%s Client-Id=%s",
                 mac, s_client_id);
    }
    if (s_token[0] != '\0') {
        /* 官方是给 token 自动补 "Bearer " 前缀，不是裸 token。
         * 注意这个 API 是 (client, key, value) 三段式，不带冒号和 CRLF。 */
        char auth[200];
        if (strchr(s_token, ' ') == NULL) {
            snprintf(auth, sizeof(auth), "Bearer %s", s_token);
        } else {
            strlcpy(auth, s_token, sizeof(auth));
        }
        esp_websocket_client_append_header(s_ws, "Authorization", auth);
        ESP_LOGI(TAG, "WS Authorization: %s", auth);
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_cb, NULL);

    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        report_error(XZ_ERR_NETWORK);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (s_pkt_mb == NULL) {
        s_pkt_mb_buf = heap_caps_malloc(XZ_PKT_MB_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_pkt_mb_buf != NULL) {
            s_pkt_mb = xMessageBufferCreateStatic(XZ_PKT_MB_SIZE, s_pkt_mb_buf,
                                                  &s_pkt_mb_struct);
        }
        if (s_pkt_mb == NULL) {
            ESP_LOGE(TAG, "下行音频缓冲创建失败");
        }
    }

    BaseType_t ok = xTaskCreateWithCaps(record_task, "xz_rec", XZ_REC_TASK_STACK,
                                        NULL, 6, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        report_debug("录音任务创建失败（PSRAM 不足）");
        report_error(XZ_ERR_NO_MEM);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "录音任务已创建（栈 %d 字节，PSRAM）", XZ_REC_TASK_STACK);

    report_debug("正在连接 WebSocket…");

    ESP_LOGW(TAG, "内存快照: 内部RAM 空闲 %u / 最大块 %u | PSRAM 空闲 %u / 最大块 %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    vTaskDelete(NULL);
}

/* ==================== 对外接口 ==================== */

/**
 * @brief 查询是否就绪。
 * @return true 可以说话
 */
/**
 * @brief 设置播放音量（0~100），并立即应用到 codec。
 * @param vol 音量
 */
void ui_xiaozhi_set_volume(int vol)
{
    if (vol < 0) {
        vol = 0;
    } else if (vol > 100) {
        vol = 100;
    }
    s_cur_volume = vol;
    bsp_codec_volume_set(vol, NULL);
    ESP_LOGI(TAG, "音量设为 %d", vol);
}

/**
 * @brief 读取当前音量。
 * @return 0~100
 */
int ui_xiaozhi_get_volume(void)
{
    return s_cur_volume;
}

bool ui_xiaozhi_is_ready(void)
{
    return s_running && s_ws != NULL && esp_websocket_client_is_connected(s_ws);
}

/**
 * @brief 按住说话：切录音状态并起录音任务。
 */
void ui_xiaozhi_record_begin(void)
{
    if (!ui_xiaozhi_is_ready() || s_recording) {
        return;
    }
    s_rec_start_pending = true;
    report_state(XZ_STATE_RECORDING);
    ESP_LOGW(TAG, ">>> BTN DOWN: start_pending=1");
    ESP_LOGW(TAG, "    ready=%d running=%d ws=%p recording=%d smb=%p enc=%p",
             (int)ui_xiaozhi_is_ready(), (int)s_running, (void *)s_ws,
             (int)s_recording, (void *)s_pkt_mb, (void *)s_enc);
}

/**
 * @brief 松开：停止录音，通知服务端结束。
 */
void ui_xiaozhi_record_end(void)
{
    if (!s_recording && !s_rec_start_pending) {
        return;
    }
    s_rec_stop_pending = true;
    report_state(XZ_STATE_THINKING);
    ESP_LOGW(TAG, ">>> BTN UP: stop_pending=1");
    ESP_LOGW(TAG, "    recording=%d reced=%u",
             (int)s_recording, (unsigned)s_uploaded);
}

/**
 * @brief 启动小智助手（异步：先激活，再连 WebSocket）。
 */
void ui_xiaozhi_task_start(void)
{
    if (s_running) {
        return;
    }

    if (s_pcm == NULL) {
        s_pcm = heap_caps_malloc(XZ_FRAME_SAMPLES * sizeof(int16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_opus = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        /* 按最高可能采样率分配：48000 * 60ms = 2880 样本 */
        s_dec_pcm = heap_caps_malloc(XZ_MAX_SAMPLES * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_pcm == NULL || s_opus == NULL) {
        report_error(XZ_ERR_NO_MEM);
        return;
    }

    s_running = true;
    report_state(XZ_STATE_CONNECTING);

    if (xTaskCreatePinnedToCore(xz_boot_task, "xz_boot", XZ_TASK_STACK, NULL, 5, NULL, 0) != pdPASS) {
        report_error(XZ_ERR_NO_MEM);
        s_running = false;
        return;
    }
}

/**
 * @brief 停止小智助手并释放资源。
 */
void ui_xiaozhi_task_stop(void)
{
    ESP_LOGW(TAG, ">>> STOP: 开始关闭 (ws=%p enc=%p dec=%p mb=%p gone=%d)",
             (void *)s_ws, (void *)s_enc, (void *)s_dec, (void *)s_pkt_mb, (int)s_rec_gone);
    s_recording = false;
    s_rec_start_pending = false;
    s_rec_stop_pending  = false;
    s_running = false;


    if (!s_rec_gone && s_running) {
        s_rec_exit = true;
        for (int i = 0; i < 100 && !s_rec_gone; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));   /* 最多等 1 秒 */
        }
        if (!s_rec_gone) {
            ESP_LOGE(TAG, "record_task 未能在 1 秒内退出");
        } else {
            ESP_LOGW(TAG, ">>> STOP: record_task 已退出");
        }
        s_rec_exit = false;
    }

    pa_en(0);   /* 关闭功放，和音乐 App 退出时的做法一致 */

    if (s_pkt_mb != NULL) {
        vMessageBufferDelete(s_pkt_mb);   /* 只删句柄，PSRAM 缓冲保留复用 */
        s_pkt_mb = NULL;
    }

    if (s_ws != NULL) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    if (s_enc != NULL) {
        opus_encoder_destroy(s_enc);
        s_enc = NULL;
    }
    if (s_dec != NULL) {
        opus_decoder_destroy(s_dec);
        s_dec = NULL;
    }

    /* 清掉缓存的连接信息，下次打开重新走 OTA 拿地址 */
    s_ws_url[0] = '\0';
    s_token[0]  = '\0';

    {
        ui_msg_t msg = { .id = UI_MSG_XZ_CLOSE };
        ui_msg_send(&msg, 0);
    }
}
