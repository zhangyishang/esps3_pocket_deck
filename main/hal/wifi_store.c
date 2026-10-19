#include "wifi_store.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "wifi_store";

// 两个名字都在 NVS 的 15 字符限制内。
#define WIFI_STORE_NVS_NAMESPACE "wifi"
#define WIFI_STORE_NVS_KEY       "nets"
#define WIFI_STORE_VERSION       1u

// NVS 中存储的整体数据结构
typedef struct {
    uint16_t    version;              // 便于以后改格式时识别
    uint16_t    count;                // 有效项数，<= WIFI_STORE_MAX
    wifi_saved_t nets[WIFI_STORE_MAX]; // 下标 0 = 最近一次成功连接的
} wifi_store_blob_t;

// 静态互斥锁及其缓冲区，避免动态内存分配
static StaticSemaphore_t s_store_lock_buf;
static SemaphoreHandle_t s_store_lock;
// 用于保护锁本身初始化的一次性临界区
static portMUX_TYPE      s_store_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;

/**
  * @brief  获取存储互斥锁
  * @param  无
  * @retval 互斥锁句柄；创建失败时为 NULL
  */
static SemaphoreHandle_t store_lock_get(void)
{
    if (s_store_lock == NULL) {
        portENTER_CRITICAL(&s_store_lock_init_mux);
        if (s_store_lock == NULL) {
            s_store_lock = xSemaphoreCreateMutexStatic(&s_store_lock_buf);
        }
        portEXIT_CRITICAL(&s_store_lock_init_mux);
    }
    return s_store_lock;
}

/**
  * @brief  从 NVS 加载 WiFi 存档到内存
  * @param  blob 输出参数，用于接收加载的数据
  * @retval true 读取到有效存档；false 无有效存档（视为空）
  */
static bool store_load(wifi_store_blob_t *blob)
{
    // 先清零，并写入当前版本号
    memset(blob, 0, sizeof(*blob));
    blob->version = WIFI_STORE_VERSION;

    nvs_handle_t h;
    // 以只读方式打开命名空间；失败说明是第一次开机，无存档
    if (nvs_open(WIFI_STORE_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
     {
        return false;   // 第一次开机：命名空间还不存在
    }

    size_t len = sizeof(*blob);
    esp_err_t err = nvs_get_blob(h, WIFI_STORE_NVS_KEY, blob, &len);
    nvs_close(h);
    // 校验：读取失败、长度不符、版本不符、count 越界，均视为无效存档
    if (err != ESP_OK || len != sizeof(*blob) ||
        blob->version != WIFI_STORE_VERSION || blob->count > WIFI_STORE_MAX)
    {
        memset(blob, 0, sizeof(*blob));
        blob->version = WIFI_STORE_VERSION;
        return false;
    }
    // 强制字符串以 '\0' 结尾，防止损坏数据导致越界读
    for (uint16_t i = 0; i < blob->count; i++) 
    {
        blob->nets[i].ssid[sizeof(blob->nets[i].ssid) - 1] = '\0';
        blob->nets[i].password[sizeof(blob->nets[i].password) - 1] = '\0';
    }
    return true;
}

/**
  * @brief  将内存中的存档写入 NVS
  * @param  blob 待提交的存档数据
  * @retval true 写入并提交成功；false 失败
  */
static bool store_commit(const wifi_store_blob_t *blob)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_STORE_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open 失败：%s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(h, WIFI_STORE_NVS_KEY, blob, sizeof(*blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);   // set_blob 只写到缓存，commit 才落 flash
    } else {
        ESP_LOGE(TAG, "nvs_set_blob 失败：%s", esp_err_to_name(err));
    }
    nvs_close(h);
    return err == ESP_OK;
}
/**
  * @brief  查询已保存的 WiFi 数量
  * @param  无
  * @retval 已保存的条目数
  */
int wifi_store_count(void)
{
    // 获取锁；加锁保护 NVS 访问
    SemaphoreHandle_t lock = store_lock_get();
    if (lock != NULL) {
        xSemaphoreTake(lock, portMAX_DELAY);
    }

    wifi_store_blob_t blob;
    store_load(&blob);
    int count = (int)blob.count;

    if (lock != NULL) {
        xSemaphoreGive(lock);
    }
    return count;
}

/**
  * @brief  按索引读取一条已保存的 WiFi 记录
  * @param  index 索引（0 为最近一次成功连接的）
  * @param  out   输出参数，接收记录内容
  * @retval true 读取成功；false 参数非法或索引越界
  */
bool wifi_store_get(int index, wifi_saved_t *out)
{
    if (out == NULL || index < 0) {
        return false;
    }

    SemaphoreHandle_t lock = store_lock_get();
    if (lock != NULL) {
        xSemaphoreTake(lock, portMAX_DELAY);
    }

    wifi_store_blob_t blob;
    bool found = store_load(&blob) && index < (int)blob.count;
    if (found) {
        *out = blob.nets[index];
    }

    if (lock != NULL) {
        xSemaphoreGive(lock);
    }
    return found;
}
/**
  * @brief  读取最近一次成功连接的 WiFi 记录
  * @param  out 输出参数，接收记录内容
  * @retval true 读取成功；false 无记录
  */
bool wifi_store_last(wifi_saved_t *out)
{
    return wifi_store_get(0, out);
}
/**
  * @brief  根据 SSID 查询对应的密码
  * @param  ssid 要查询的 SSID
  * @param  buf  输出缓冲区，用于存放密码
  * @param  len  缓冲区长度
  * @retval true 找到并复制成功；false 未找到或参数非法
  */
bool wifi_store_get_password(const char *ssid, char *buf, size_t len)
{
    if (ssid == NULL || buf == NULL || len == 0) {
        return false;
    }
    buf[0] = '\0';

    SemaphoreHandle_t lock = store_lock_get();
    if (lock != NULL) {
        xSemaphoreTake(lock, portMAX_DELAY);
    }

    wifi_store_blob_t blob;
    bool found = false;
    // 遍历所有记录，匹配 SSID
    if (store_load(&blob)) {
        for (uint16_t i = 0; i < blob.count; i++) {
            if (strcmp(blob.nets[i].ssid, ssid) == 0) {
                strlcpy(buf, blob.nets[i].password, len);
                found = true;
                break;
            }
        }
    }

    if (lock != NULL) {
        xSemaphoreGive(lock);
    }
    return found;
}
/**
  * @brief  保存一条 WiFi 记录（已存在则移到最前，满了挤掉最旧）
  * @param  ssid     要保存的 SSID
  * @param  password 密码（可为 NULL，视为空密码）
  * @retval true 保存成功；false 参数非法或写入失败
  */
bool wifi_store_save(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    SemaphoreHandle_t lock = store_lock_get();
    if (lock != NULL) {
        xSemaphoreTake(lock, portMAX_DELAY);
    }

    wifi_store_blob_t blob;
    store_load(&blob);

    int idx = -1;
    for (uint16_t i = 0; i < blob.count; i++) 
    {
        if (strcmp(blob.nets[i].ssid, ssid) == 0) 
        {
            idx = (int)i;
            break;
        }
    }
    int head = (idx >= 0) ? idx : (int)blob.count;
    if (head > (int)WIFI_STORE_MAX - 1) 
    {
        head = (int)WIFI_STORE_MAX - 1;   // 满了：最旧的那个被挤出去
    }
    for (int i = head; i > 0; i--) {
        blob.nets[i] = blob.nets[i - 1];
    }
    if (idx < 0 && blob.count < WIFI_STORE_MAX)
    {
        blob.count++;
    }

    strlcpy(blob.nets[0].ssid, ssid, sizeof(blob.nets[0].ssid));
    strlcpy(blob.nets[0].password, (password != NULL) ? password : "",
            sizeof(blob.nets[0].password));

    bool ok = store_commit(&blob);
    if (ok) 
    {
        ESP_LOGI(TAG, "已记住 %s（共 %u 个）", blob.nets[0].ssid, (unsigned)blob.count);
    }

    if (lock != NULL) {
        xSemaphoreGive(lock);
    }
    return ok;
}
/**
  * @brief  删除指定 SSID 的 WiFi 记录（幂等：不存在也返回成功）
  * @param  ssid 要删除的 SSID
  * @retval true 删除成功或本就不存在；false 参数非法或写入失败
  */
bool wifi_store_forget(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') 
    {
        return false;
    }

    SemaphoreHandle_t lock = store_lock_get();
    if (lock != NULL)
    {
        xSemaphoreTake(lock, portMAX_DELAY);
    }

    wifi_store_blob_t blob;
    bool ok = true;
    if (store_load(&blob)) 
    {
         // 查找匹配的 SSID
        for (uint16_t i = 0; i < blob.count; i++) 
        {
            if (strcmp(blob.nets[i].ssid, ssid) != 0) 
            {
                continue;
            }
            // 用后续元素覆盖当前位置，实现删除
            for (uint16_t j = i + 1; j < blob.count; j++) 
            {
                blob.nets[j - 1] = blob.nets[j];
            }
            blob.count--;
            // 清空尾部冗余项
            memset(&blob.nets[blob.count], 0, sizeof(blob.nets[0]));
            ok = store_commit(&blob);
            if (ok) 
            {
                ESP_LOGI(TAG, "已忘记 %s（剩 %u 个）", ssid, (unsigned)blob.count);
            }
            break;   // 找不到也算成功
        }
    }

    if (lock != NULL) {
        xSemaphoreGive(lock);
    }
    return ok;
}
