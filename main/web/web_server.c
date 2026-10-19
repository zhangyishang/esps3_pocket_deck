#include "web_server.h"
#include "web_index.h"
#include "hal_sd.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"

static const char *TAG = "web";

/** 所有对外路径的前缀。任何请求路径都必须落在这棵子树下。 */
#define WEB_ROOT        "/sdcard"

/** 单个上传文件的大小上限（防止把卡写满、或长时间占住连接）。 */
#define WEB_MAX_UPLOAD  (16 * 1024 * 1024)

/** 上传/下载分块大小。放 PSRAM，不占内部 RAM。 */
#define WEB_CHUNK       (4 * 1024)

/** 路径与查询串的最大长度。 */
#define WEB_PATH_MAX    256
#define WEB_QS_MAX      256

static httpd_handle_t s_server = NULL;

/**
 * @brief 把 %XX 就地解码成字符（中文文件名必需）。
 * @param s 待解码字符串（就地修改）
 */
static void url_decode(char *s)
{
    char *d = s;
    while (*s) {
        if (s[0] == '%' && s[1] && s[2]) {
            int hi = s[1] >= 'a' ? s[1] - 'a' + 10 : (s[1] >= 'A' ? s[1] - 'A' + 10 : s[1] - '0');
            int lo = s[2] >= 'a' ? s[2] - 'a' + 10 : (s[2] >= 'A' ? s[2] - 'A' + 10 : s[2] - '0');
            *d++ = (char)((hi << 4) | lo);
            s += 3;
        } else if (s[0] == '+') {
            *d++ = ' ';
            s++;
        } else {
            *d++ = *s++;
        }
    }
    *d = '\0';
}

/**
 * @brief 取查询参数并做 URL 解码。
 * @param req 请求
 * @param key 参数名
 * @param out 输出缓冲
 * @param cap 容量
 * @return true 找到
 */
static bool query_get(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    char qs[WEB_QS_MAX];
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(qs, key, out, cap) != ESP_OK) {
        return false;
    }
    url_decode(out);
    return true;
}

/**
 * @brief 校验并规范化请求路径：必须落在 /sdcard 下，且不允许出现 ".."。
 * @param raw 请求里的 path 参数（已解码）
 * @param out 输出规范路径
 * @param cap 容量
 * @return true 合法
 */
static bool safe_path(const char *raw, char *out, size_t cap)
{
    if (raw == NULL || raw[0] == '\0') {
        snprintf(out, cap, "%s", WEB_ROOT);
        return true;
    }
    if (strstr(raw, "..") != NULL) {
        return false;
    }
    // 只接受绝对路径；不以前缀开头的（例如 "/"）一律落到根
    if (strncmp(raw, WEB_ROOT, strlen(WEB_ROOT)) != 0) {
        snprintf(out, cap, "%s", WEB_ROOT);
        return true;
    }
    // 去掉结尾多余的 '/'
    snprintf(out, cap, "%s", raw);
    size_t n = strlen(out);
    while (n > strlen(WEB_ROOT) && out[n - 1] == '/') {
        out[--n] = '\0';
    }
    return true;
}

/**
 * @brief 把字符串按 JSON 规则转义后追加写入缓冲。
 * @param dst 目标
 * @param cap 容量
 * @param src 源
 */
static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 7 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            o += snprintf(dst + o, cap - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/**
 * @brief 发一个简单的 JSON 错误响应。
 * @param req  请求
 * @param code HTTP 状态码
 * @param msg  错误说明
 * @return 总是 ESP_OK
 */
static esp_err_t send_err(httpd_req_t *req, const char *code, const char *msg)
{
    httpd_resp_set_status(req, code);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    char body[160];
    char esc[128];
    json_escape(esc, sizeof(esc), msg);
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", esc);
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

/* ============================ GET / ============================ */

/**
 * @brief 返回内嵌的网页（gzip）。
 * @param req 请求
 * @return ESP_OK
 */
static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)web_index_gz, WEB_INDEX_GZ_LEN);
}

/* ======================= GET /api/list ======================== */

/**
 * @brief 列目录：返回 JSON（items + 剩余空间）。可选递归子目录在 items 里标记 type。
 * @param req 请求
 * @return ESP_OK
 */
static esp_err_t h_list(httpd_req_t *req)
{
    char raw[WEB_PATH_MAX] = {0};
    char path[WEB_PATH_MAX];

    if (!query_get(req, "path", raw, sizeof(raw))) {
        snprintf(raw, sizeof(raw), "%s", WEB_ROOT);
    }
    if (!safe_path(raw, path, sizeof(path))) {
        return send_err(req, "400 Bad Request", "非法路径");
    }

    if (hal_sdcard_lock(3000) != ESP_OK) {
        return send_err(req, "503 Service Unavailable", "SD 卡忙");
    }
    if (!bsp_sdcard_is_mounted() && bsp_sdcard_mount() != ESP_OK) {
        hal_sdcard_unlock();
        return send_err(req, "503 Service Unavailable", "没有 SD 卡");
    }

    DIR *d = opendir(path);
    if (d == NULL) {
        hal_sdcard_unlock();
        return send_err(req, "404 Not Found", "目录不存在");
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    {
        char esc[WEB_PATH_MAX * 2];
        json_escape(esc, sizeof(esc), path);
        char head[WEB_PATH_MAX * 2 + 32];
        snprintf(head, sizeof(head), "{\"path\":\"%s\",\"items\":[", esc);
        httpd_resp_sendstr_chunk(req, head);
    }

    bool first = true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char full[WEB_PATH_MAX + 300];
        strlcpy(full, path, sizeof(full));
        strlcat(full, "/", sizeof(full));
        strlcat(full, e->d_name, sizeof(full));

        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        const char *type = S_ISDIR(st.st_mode) ? "dir" : "file";

        char esc[600];
        json_escape(esc, sizeof(esc), e->d_name);

        char item[700];
        snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"type\":\"%s\",\"size\":%lu}",
                 first ? "" : ",", esc, type, (unsigned long)st.st_size);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    closedir(d);

    // 剩余空间（ESP-IDF 的 FAT VFS 用 esp_vfs_fat_info 查询）
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(WEB_ROOT, &total_bytes, &free_bytes) != ESP_OK) {
        free_bytes = 0;
    }
    hal_sdcard_unlock();

    char tail[96];
    snprintf(tail, sizeof(tail), "],\"free_bytes\":%llu}", (unsigned long long)free_bytes);
    httpd_resp_sendstr_chunk(req, tail);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ========================== GET /dl =========================== */

/**
 * @brief 下载一个文件（分块发送）。
 * @param req 请求
 * @return ESP_OK
 */
static esp_err_t h_dl(httpd_req_t *req)
{
    char raw[WEB_PATH_MAX] = {0};
    char path[WEB_PATH_MAX];

    if (!query_get(req, "path", raw, sizeof(raw)) ||
        !safe_path(raw, path, sizeof(path))) {
        return send_err(req, "400 Bad Request", "非法路径");
    }

    if (hal_sdcard_lock(3000) != ESP_OK) {
        return send_err(req, "503 Service Unavailable", "SD 卡忙");
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        hal_sdcard_unlock();
        return send_err(req, "404 Not Found", "文件不存在");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    uint8_t *buf = heap_caps_malloc(WEB_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(f);
        hal_sdcard_unlock();
        return send_err(req, "500 Internal Server Error", "内存不足");
    }

    size_t n;
    esp_err_t ret = ESP_OK;
    while ((n = fread(buf, 1, WEB_CHUNK, f)) > 0) {
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
    }
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    free(buf);
    fclose(f);
    hal_sdcard_unlock();
    return ret;
}

/* ======================== PUT /api/upload ===================== */

/**
 * @brief 接收上传：请求体就是文件原始字节，分块写入 SD 卡。
 *        参数 path=目录, name=文件名。
 * @param req 请求
 * @return ESP_OK
 */
static esp_err_t h_upload(httpd_req_t *req)
{
    char dir[WEB_PATH_MAX] = {0};
    char name[128] = {0};
    char path[WEB_PATH_MAX];

    if (!query_get(req, "path", dir, sizeof(dir)) ||
        !query_get(req, "name", name, sizeof(name))) {
        return send_err(req, "400 Bad Request", "缺少 path 或 name");
    }
    if (name[0] == '\0' || strstr(name, "..") != NULL ||
        strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return send_err(req, "400 Bad Request", "非法文件名");
    }
    if (!safe_path(dir, path, sizeof(path))) {
        return send_err(req, "400 Bad Request", "非法路径");
    }
    if (req->content_len <= 0 || req->content_len > WEB_MAX_UPLOAD) {
        return send_err(req, "413 Payload Too Large", "文件太大");
    }

    if (hal_sdcard_lock(3000) != ESP_OK) {
        return send_err(req, "503 Service Unavailable", "SD 卡忙");
    }
    if (!bsp_sdcard_is_mounted() && bsp_sdcard_mount() != ESP_OK) {
        hal_sdcard_unlock();
        return send_err(req, "503 Service Unavailable", "没有 SD 卡");
    }

    char full[WEB_PATH_MAX + 160];
    snprintf(full, sizeof(full), "%s/%s", path, name);

    FILE *f = fopen(full, "wb");
    if (f == NULL) {
        hal_sdcard_unlock();
        return send_err(req, "500 Internal Server Error", "打不开文件");
    }

    uint8_t *buf = heap_caps_malloc(WEB_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(f);
        hal_sdcard_unlock();
        return send_err(req, "500 Internal Server Error", "内存不足");
    }

    size_t remaining = req->content_len;
    esp_err_t result = ESP_OK;
    while (remaining > 0) {
        size_t want = remaining > WEB_CHUNK ? WEB_CHUNK : remaining;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            ESP_LOGE(TAG, "接收中断，剩余 %u 字节", (unsigned)remaining);
            result = ESP_FAIL;
            break;
        }
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
            ESP_LOGE(TAG, "写卡失败");
            result = ESP_FAIL;
            break;
        }
        remaining -= (size_t)r;
        // 让出 CPU，避免长时间占住 httpd 任务把界面拖卡
        vTaskDelay(1);
    }

    free(buf);
    fclose(f);
    hal_sdcard_unlock();

    if (result != ESP_OK) {
        remove(full);
        return send_err(req, "500 Internal Server Error", "写入失败");
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ======================== POST /api/mkdir ===================== */

/**
 * @brief 建目录。参数 path=完整目录路径。
 * @param req 请求
 * @return ESP_OK
 */
static esp_err_t h_mkdir(httpd_req_t *req)
{
    char raw[WEB_PATH_MAX] = {0};
    char path[WEB_PATH_MAX];

    if (!query_get(req, "path", raw, sizeof(raw)) ||
        !safe_path(raw, path, sizeof(path))) {
        return send_err(req, "400 Bad Request", "非法路径");
    }

    if (hal_sdcard_lock(3000) != ESP_OK) {
        return send_err(req, "503 Service Unavailable", "SD 卡忙");
    }
    int rc = mkdir(path, 0777);
    hal_sdcard_unlock();

    if (rc != 0) {
        return send_err(req, "500 Internal Server Error", "创建失败");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ======================= DELETE /api/delete =================== */

/**
 * @brief 删除文件或空目录。参数 path=完整路径。
 * @param req 请求
 * @return ESP_OK
 */
static esp_err_t h_delete(httpd_req_t *req)
{
    char raw[WEB_PATH_MAX] = {0};
    char path[WEB_PATH_MAX];

    if (!query_get(req, "path", raw, sizeof(raw)) ||
        !safe_path(raw, path, sizeof(path))) {
        return send_err(req, "400 Bad Request", "非法路径");
    }
    // 不允许删根
    if (strcmp(path, WEB_ROOT) == 0) {
        return send_err(req, "400 Bad Request", "不能删根目录");
    }

    if (hal_sdcard_lock(3000) != ESP_OK) {
        return send_err(req, "503 Service Unavailable", "SD 卡忙");
    }
    int rc = remove(path);
    hal_sdcard_unlock();

    if (rc != 0) {
        return send_err(req, "500 Internal Server Error", "删除失败（目录必须为空）");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ============================ 生命周期 ========================= */

/**
 * @brief 启动 HTTP 服务并注册全部路由。
 * @return ESP_OK 成功
 */
esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 12;
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;
    cfg.keep_alive_enable = true;
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,    .handler = h_index  },
        { .uri = "/api/list",    .method = HTTP_GET,    .handler = h_list   },
        { .uri = "/dl",          .method = HTTP_GET,    .handler = h_dl     },
        { .uri = "/api/upload",  .method = HTTP_PUT,    .handler = h_upload },
        { .uri = "/api/mkdir",   .method = HTTP_POST,   .handler = h_mkdir  },
        { .uri = "/api/delete",  .method = HTTP_DELETE, .handler = h_delete },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(s_server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败", uris[i].uri);
        }
    }

    ESP_LOGI(TAG, "HTTP 文件管理已启动（端口 80）");
    return ESP_OK;
}

/**
 * @brief 停止 HTTP 服务。
 */
void web_server_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP 文件管理已停止");
    }
}

/**
 * @brief 查询服务是否在运行。
 * @return true 正在运行
 */
bool web_server_is_running(void)
{
    return s_server != NULL;
}
