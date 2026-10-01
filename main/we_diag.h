// main/we_diag.h — 上一次余额刷新的诊断快照。
//
// 为什么要有它:设备在别人家里出问题时,唯一能拿到的信息往往只有屏幕上那行
// "DIRECT FAIL"。这条消息只说明"WiFi 连上了但一个平台都没查到",而 DNS 解析失败、
// TCP 超时、TLS 握手失败、证书链校验失败、Key 无效(401)、响应 JSON 变了
// (200 但字段对不上)这几种完全不同的病因,屏幕上原本长得一模一样。
//
// 本模块把每次刷新的关键中间量记下来(阶段 / esp_err / mbedTLS 错误码 /
// 证书校验标志 / HTTP 状态码 / 响应体预览 / 堆余量),门户的 /diag 页面直接把它
// 打成纯文本,用户复制一段文本就能定位问题。
//
// 零 ESP-IDF / LVGL 依赖:纯数据 + 字符串映射,可在 PC 上直接跑 host 测试。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_DIAG_ROWS      3      // 与余额页卡槽数(ROW_COUNT / HIST_ROWS)一致
#define WE_DIAG_TAG_MAX   16     // 屏幕短码缓冲
#define WE_DIAG_STAGE_MAX 12     // 阶段名缓冲
#define WE_DIAG_ID_MAX    16
#define WE_DIAG_BODY_MAX  160    // 每个平台保留的响应体预览(含结尾 '\0')

// 单个平台的取数结果
typedef struct {
    char id[WE_DIAG_ID_MAX];        // provider_id
    char tag[WE_DIAG_TAG_MAX];      // 屏幕短码:"DNS" / "TLS" / "401" / "JSON" / "ok"
    int  attempts;                  // 本次刷新尝试了几次(含重试)
    int  open_err;                  // esp_http_client_open 的返回值(0 = 打开成功)
    int  tls_code;                  // esp_http_client_get_and_clear_last_tls_error 的 mbedTLS 码
    int  tls_flags;                 // 同上:证书校验标志位,非 0 = 证书链被拒
    int  http_status;               // HTTP 状态码(-1 = 没拿到)
    int  body_len;                  // 实际读到的字节数
    bool parsed;                    // 是否成功解析出金额
    int  millis;                    // 该平台耗时(ms)
    char body[WE_DIAG_BODY_MAX];    // 响应体前若干字节(只留可打印字符)
} we_diag_row_t;

// 整次刷新的诊断快照
typedef struct {
    uint32_t seq;                     // 第几次刷新(进程内自增,从 1 开始)
    char     stage[WE_DIAG_STAGE_MAX];// 卡在哪一步:CONFIG/WIFI/SNTP/FETCH/RETRY/DONE/FAIL/CANCEL
    bool     clock_ok;                // 取数时系统时间是否已校时(>= 2023-11)
    int      wifi_rc;                 // 0 = 已连上;其余为 wifi_connect_once 的错误码
    int      n_ok;                    // 成功平台数
    int      n_total;                 // 已配置平台数
    uint32_t free_heap;               // 取数前的空闲堆
    uint32_t min_free_heap;           // 历史最小空闲堆(含 TLS 握手峰值)
    uint32_t largest_block;           // 最大连续可分配块(TLS 握手最吃这个)
    we_diag_row_t row[WE_DIAG_ROWS];
} we_diag_t;

// 开新一轮刷新(seq 自增,其余清零)。上一次的结果会被覆盖。
void we_diag_reset(void);

// 记录当前阶段(见 stage 字段注释)
void we_diag_set_stage(const char *stage);

// 记录环境:WiFi 连接结果、是否已校时、三项堆统计
void we_diag_set_env(int wifi_rc, bool clock_ok, uint32_t free_heap,
                     uint32_t min_free_heap, uint32_t largest_block);

// 记录第 idx 个平台的取数结果(idx 超范围忽略)
void we_diag_set_row(int idx, const we_diag_row_t *row);

// 收尾:记下成功/总数
void we_diag_finish(int n_ok, int n_total);

// 只读访问(门户 /diag 用)。返回的指针始终有效,内容可能被刷新任务并发改写,
// 读取方需要自己拷一份并强制补 '\0'。
const we_diag_t *we_diag_get(void);

// ---- 短码映射(纯逻辑,host 可测)----
// esp_err_t → 屏幕短码。0x8000 段是 ESP-TLS/esp-tls 的错误码
// (见 esp_tls_errors.h 的 ESP_ERR_ESP_TLS_* / ESP_ERR_MBEDTLS_*)。
void we_diag_tag_from_err(int err, char *out, size_t cap);

// HTTP 状态码 + 是否解析成功 → 屏幕短码。
void we_diag_tag_from_status(int status, bool parsed, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
