// main/we_diag.c — 余额刷新诊断快照(纯数据,零 ESP-IDF/LVGL 依赖)
#include "we_diag.h"

#include <stdio.h>
#include <string.h>

static we_diag_t s_diag;

static void str_trunc(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void we_diag_reset(void)
{
    uint32_t seq = s_diag.seq + 1;      // 保留计数,方便对照"第几次刷新"
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.seq = seq;
    for (int i = 0; i < WE_DIAG_ROWS; i++) s_diag.row[i].http_status = -1;
    s_diag.wifi_rc = 1;                 // 1 = 还没尝试连接
}

void we_diag_set_stage(const char *stage)
{
    str_trunc(s_diag.stage, sizeof(s_diag.stage), stage);
}

void we_diag_set_env(int wifi_rc, bool clock_ok, uint32_t free_heap,
                     uint32_t min_free_heap, uint32_t largest_block)
{
    s_diag.wifi_rc      = wifi_rc;
    s_diag.clock_ok     = clock_ok;
    s_diag.free_heap    = free_heap;
    s_diag.min_free_heap = min_free_heap;
    s_diag.largest_block = largest_block;
}

void we_diag_set_row(int idx, const we_diag_row_t *row)
{
    if (!row || idx < 0 || idx >= WE_DIAG_ROWS) return;
    s_diag.row[idx] = *row;
    s_diag.row[idx].id[sizeof(s_diag.row[idx].id) - 1]     = '\0';
    s_diag.row[idx].tag[sizeof(s_diag.row[idx].tag) - 1]   = '\0';
    s_diag.row[idx].body[sizeof(s_diag.row[idx].body) - 1] = '\0';
}

void we_diag_finish(int n_ok, int n_total)
{
    s_diag.n_ok    = n_ok;
    s_diag.n_total = n_total;
}

const we_diag_t *we_diag_get(void)
{
    return &s_diag;
}

// ---------------------------------------------------------------- 短码映射
// 值取自 ESP-IDF 5.5.3 components/esp-tls/esp_tls_errors.h:
//   ESP_ERR_ESP_TLS_BASE = 0x8000,各错误码就是 BASE + 偏移。
// 这里刻意写成字面量而不是 include 那个头:本文件要保持零 ESP-IDF 依赖,
// 才能扔进 host 测试台直接跑。偏移量是稳定的宏定义,不会悄悄变。
// 自写数字转字符串:不用 snprintf。IDF 的 main 组件开着 -Werror=format-truncation,
// 而这里的缓冲本来就是"故意很小、故意要截断"的,交给 snprintf 在编译期容易被挑刺
// (截断与否取决于运行时的值,编译器只能按最坏宽度报警)。cap 保证以 '\0' 结尾。
static void num_to_str(char *out, size_t cap, unsigned value, unsigned base, int min_digits)
{
    static const char digits[] = "0123456789ABCDEF";
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = digits[value % base];
        value /= base;
    } while (value && n < (int)sizeof(tmp));
    while (n < min_digits && n < (int)sizeof(tmp)) tmp[n++] = '0';

    size_t o = 0;
    while (n > 0 && o + 1 < cap) out[o++] = tmp[--n];
    if (cap) out[o] = '\0';
}

void we_diag_tag_from_err(int err, char *out, size_t cap)
{
    const char *tag = NULL;
    switch (err) {
    case 0x8001: tag = "DNS";   break;   // CANNOT_RESOLVE_HOSTNAME
    case 0x8002: tag = "SOCK";  break;   // CANNOT_CREATE_SOCKET
    case 0x8003: tag = "PROTO"; break;   // UNSUPPORTED_PROTOCOL_FAMILY
    case 0x8004: tag = "CONN";  break;   // FAILED_CONNECT_TO_HOST(被防火墙/网络阻断)
    case 0x8005: tag = "SETOPT";break;   // SOCKET_SETOPT_FAILED
    case 0x8006: tag = "TOUT";  break;   // CONNECTION_TIMEOUT(TCP 连不上)
    case 0x8007: tag = "SE";    break;   // SE_FAILED
    case 0x8008: tag = "FIN";   break;   // TCP_CLOSED_FIN
    case 0x8009: tag = "TLSTO"; break;   // SERVER_HANDSHAKE_TIMEOUT
    case 0x8010: tag = "CERT";  break;   // MBEDTLS_CERT_PARTLY_OK
    case 0x8015: tag = "X509";  break;   // MBEDTLS_X509_CRT_PARSE_FAILED
    case 0x8011: case 0x8012: case 0x8013: case 0x8014:
    case 0x8016: case 0x8017: case 0x8018: case 0x8019:
    case 0x801A: case 0x801B: case 0x801C: case 0x801D:
        tag = "TLS"; break;              // mbedTLS 系列(含 SSL_HANDSHAKE_FAILED)
    default: break;
    }
    if (tag) { str_trunc(out, cap, tag); return; }
    if (err >= 0x8000) { str_trunc(out, cap, "TLS"); return; }
    if (!out || cap == 0) return;
    out[0] = 'E';
    if (cap > 1)
        num_to_str(out + 1, cap - 1, (unsigned)(err < 0 ? -err : err), 16, 4);
}

void we_diag_tag_from_status(int status, bool parsed, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (parsed) { str_trunc(out, cap, "ok"); return; }
    if (status >= 400) { num_to_str(out, cap, (unsigned)status, 10, 0); return; }
    if (status >= 100) { str_trunc(out, cap, "JSON"); return; }   // 通了但字段对不上
    str_trunc(out, cap, "NOSTAT");
}
