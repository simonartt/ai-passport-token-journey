// main/we_diag.h — 上一次余额刷新的诊断快照。
//
// 为什么要有它:设备在别人家里出问题时,唯一能拿到的信息往往只有屏幕上那行
// "DIRECT FAIL"。这条消息只说明"WiFi 连上了但一个平台都没查到",而 DNS 解析失败、
// TCP 超时、TLS 握手失败、证书链校验失败、Key 无效(401)、响应 JSON 变了
// (200 但字段对不上)这几种完全不同的病因,屏幕上原本长得一模一样。
//
// 本模块把每次刷新的关键中间量记下来(阶段 / esp_err / esp_tls 分类码 / mbedTLS
// 错误码 / 证书校验标志 / HTTP 状态码 / 响应体预览 / 堆余量),门户的 /diag 页面
// 直接把它打成纯文本,用户复制一段文本就能定位问题。
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
#define WE_DIAG_IP_MAX    16     // "255.255.255.255" + '\0'
#define WE_DIAG_HOST_MAX  32     // "api.moonshot.cn" + 余量
#define WE_DIAG_BODY_MAX  160    // 每个平台保留的响应体预览(含结尾 '\0')

// 单个平台的取数结果
typedef struct {
    char id[WE_DIAG_ID_MAX];        // provider_id
    char tag[WE_DIAG_TAG_MAX];      // 屏幕短码:"DNS" / "CONN" / "TLS" / "401" / "JSON" / "ok"
    int  attempts;                  // 本次刷新尝试了几次(含重试)
    int  open_err;                  // esp_http_client_open 的返回值(0x7000 段)
    int  tls_err;                   // esp_tls 分类码(0x8000 段)
    int  tls_code;                  // mbedTLS 错误码
    int  tls_flags;                 // 证书校验标志位,非 0 = 证书链被拒
    int  http_status;               // HTTP 状态码(-1 = 没拿到)
    int  body_len;                  // 实际读到的字节数
    bool parsed;                    // 是否成功解析出金额
    int  millis;                    // 该平台耗时(ms)
    char body[WE_DIAG_BODY_MAX];    // 响应体前若干字节(只留可打印字符)
} we_diag_row_t;

// 网络环境自身的探测结果。
//
// 为什么要独立探测:esp_http_client_open 失败时只回一个笼统的
// ESP_ERR_HTTP_CONNECT(0x7002) —— 它的实现就一句
//     if (esp_transport_connect(...) < 0) return ESP_ERR_HTTP_CONNECT;
// "域名没解析出来""TCP 连不上""TCP 超时"全被折叠成同一个值。
// 这里用自己持有的 esp_tls 错误句柄、以 is_plain_tcp 重跑一次连接,就能拿到
// 0x8000 段的分类码(0x8001 DNS / 0x8004 CONN / 0x8006 TOUT),还能单独取到
// socket errno。全程不握手、不碰证书,所以也是一次内存低压探测。
typedef struct {
    bool probed;                    // 是否跑过(只有整轮取数全失败才跑)
    char ip[WE_DIAG_IP_MAX];        // 本机 IPv4
    char gw[WE_DIAG_IP_MAX];        // 默认网关
    char dns1[WE_DIAG_IP_MAX];      // DHCP 下发的主 DNS
    char dns2[WE_DIAG_IP_MAX];      // DHCP 下发的备用 DNS
    char host[WE_DIAG_HOST_MAX];    // 被探测的域名(第一个已配置平台)
    int  rc;                        // 0 = DNS+TCP 都通;否则是 0x8000 段分类码
    int  sock_errno;                // socket errno(0 = 无);用 we_diag_tag_from_errno 解释
    int  ms;                        // 探测耗时(ms)
} we_diag_net_t;

// 整次刷新的诊断快照
typedef struct {
    uint32_t seq;                     // 第几次刷新(进程内自增,从 1 开始)
    char     stage[WE_DIAG_STAGE_MAX];// 卡在哪一步:CONFIG/WIFI/SNTP/RATE/FETCH/RETRY/PROBE/DONE/FAIL/CANCEL
    bool     clock_ok;                // 取数时系统时间是否已校时(>= 2023-11)
    int      wifi_rc;                 // 0 = 已连上;其余为 wifi_connect_once 的错误码
    int      n_ok;                    // 成功平台数
    int      n_total;                 // 已配置平台数
    uint32_t free_heap;               // 取数前的空闲堆
    uint32_t min_free_heap;           // 历史最小空闲堆(含 TLS 握手峰值)
    uint32_t largest_block;           // 最大连续可分配块(TLS 握手最吃这个)
    int      rate_milli;              // 本次折算用的美元汇率 ×1000(0 = 本轮没用到)
    char     rate_src[8];             // live / cache / default
    we_diag_net_t net;
    we_diag_row_t row[WE_DIAG_ROWS];
} we_diag_t;

// 开新一轮刷新(seq 自增,其余清零)。上一次的结果会被覆盖。
void we_diag_reset(void);

// 记录当前阶段(见 stage 字段注释)
void we_diag_set_stage(const char *stage);

// 记录环境:WiFi 连接结果、是否已校时、三项堆统计
void we_diag_set_env(int wifi_rc, bool clock_ok, uint32_t free_heap,
                     uint32_t min_free_heap, uint32_t largest_block);

// 记录网络探测结果(probed = false 时忽略)
void we_diag_set_net(const we_diag_net_t *net);

// 记录本轮折算用的汇率(milli = USD/CNY ×1000)与来源("live"/"cache"/"default")
void we_diag_set_rate(int milli, const char *src);

// 用自检得到的确定结论细化第 idx 行的短码。
// 只在"esp_tls 没记下具体原因(tls_err == 0)且当前短码就是笼统的连接失败(CONN)"
// 时才生效 —— 也就是只改拿兜底码 0x7002 的那些行,更精确的结论不会被覆盖。
void we_diag_refine_conn_tag(int idx, const char *tag);

// 记录第 idx 个平台的取数结果(idx 超范围忽略)
void we_diag_set_row(int idx, const we_diag_row_t *row);

// 收尾:记下成功/总数
void we_diag_finish(int n_ok, int n_total);

// 只读访问(门户 /diag 用)。返回的指针始终有效,内容可能被刷新任务并发改写,
// 读取方需要自己拷一份并强制补 '\0'。
const we_diag_t *we_diag_get(void);

// ---- 短码映射(纯逻辑,host 可测)----
// esp_err_t → 屏幕短码。同时认识两段:
//   0x7000 段 = esp_http_client 自己的错误码(esp_http_client.h 的 ESP_ERR_HTTP_*)
//   0x8000 段 = esp_tls / mbedTLS 错误码(esp_tls_errors.h 的 ESP_ERR_ESP_TLS_*)
// 两段的字面值都直接写死:本文件必须保持零 ESP-IDF 依赖才能进 host 测试台,
// 而这些偏移量是稳定的宏定义,不会悄悄变。
void we_diag_tag_from_err(int err, char *out, size_t cap);

// 两个错误码都拿到时挑更有信息量的那个(tls_err 优先;它才是"为什么连不上",
// open_err 在连接阶段只会是笼统的 0x7002)。
void we_diag_tag_from_pair(int open_err, int tls_err, char *out, size_t cap);

// HTTP 状态码 + 是否解析成功 → 屏幕短码。
void we_diag_tag_from_status(int status, bool parsed, char *out, size_t cap);

// socket errno → 短码(裸 TCP 探测的结果)。0 → "ok",认不出 → "E<dec>"。
void we_diag_tag_from_errno(int err, char *out, size_t cap);

// IPv4 原始地址(小端存放,与 esp_ip4_addr_t.addr / struct in_addr.s_addr 同构)
// → 点分字符串。
//
// 特意自己拼而不是 snprintf(IPSTR, ...):GCC 的 -Wformat-truncation 会按 %d 的
// 类型宽度去估最坏输出(4 个 %d 能算到 40 字节以上),16 字节的缓冲必然被判成
// "可能截断",在 IDF 的 -Werror 下直接编译不过。这里的位数是可控的。
// addr 每 8 位一个八位组,从低到高依次是第 1~4 段。
void we_diag_ip4_str(uint32_t addr, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
