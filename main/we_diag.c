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

void we_diag_set_net(const we_diag_net_t *net)
{
    if (!net || !net->probed) return;
    s_diag.net = *net;
    /* 逐字段兜底补 '\0':结构是调用方在栈上拼的,不能假定它已经终止 */
    s_diag.net.ip[sizeof(s_diag.net.ip) - 1]       = '\0';
    s_diag.net.gw[sizeof(s_diag.net.gw) - 1]       = '\0';
    s_diag.net.dns1[sizeof(s_diag.net.dns1) - 1]   = '\0';
    s_diag.net.dns2[sizeof(s_diag.net.dns2) - 1]   = '\0';
    s_diag.net.host[sizeof(s_diag.net.host) - 1]   = '\0';
}

void we_diag_refine_conn_tag(int idx, const char *tag)
{
    if (idx < 0 || idx >= WE_DIAG_ROWS) return;
    if (!tag || !tag[0]) return;
    /* 只在"只有兜底码、esp_tls 什么都没记下"时细化:
       tls_err 非 0 说明底层已经给出具体原因,那是更权威的结论,不覆盖。 */
    if (s_diag.row[idx].tls_err != 0) return;
    if (strcmp(s_diag.row[idx].tag, "CONN") != 0) return;
    str_trunc(s_diag.row[idx].tag, sizeof(s_diag.row[idx].tag), tag);
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
// 两段字面值取自 ESP-IDF 5.5.3:
//   esp_http_client.h : ESP_ERR_HTTP_BASE = 0x7000(下面 0x700x 段)
//   esp_tls_errors.h  : ESP_ERR_ESP_TLS_BASE = 0x8000(下面 0x800x 段)
// 这里刻意写成字面量而不是 include 那两个头:本文件要保持零 ESP-IDF 依赖,
// 才能扔进 host 测试台直接跑。偏移量是稳定的宏定义,不会悄悄变。
//
// 为什么要认 0x7000 段:esp_http_client_open() 在连接阶段失败时只会回一个
// ESP_ERR_HTTP_CONNECT(0x7002) —— 它的实现是
//     if (esp_transport_connect(...) < 0) return ESP_ERR_HTTP_CONNECT;
// 底层到底是 DNS / TCP / TLS 全被折叠掉了(见 esp_http_client.c:1606)。
// 所以 0x7002 只能当"连接阶段"看,真正的原因得看 tls_err,由
// we_diag_tag_from_pair() 负责挑。
//
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
    /* ---- 0x7000 段:esp_http_client 自己的错误码 ---- */
    case 0x7001: tag = "REDIR";   break;  // ESP_ERR_HTTP_MAX_REDIRECT
    case 0x7002: tag = "CONN";    break;  // ESP_ERR_HTTP_CONNECT(连接阶段失败,原因见 tls_err)
    case 0x7003: tag = "WRITE";   break;  // ESP_ERR_HTTP_WRITE_DATA
    case 0x7004: tag = "HDR";     break;  // ESP_ERR_HTTP_FETCH_HEADER
    case 0x7005: tag = "NOTRANS"; break;  // ESP_ERR_HTTP_INVALID_TRANSPORT(多半是没开 HTTPS)
    case 0x7006: tag = "CONNING"; break;  // ESP_ERR_HTTP_CONNECTING
    case 0x7007: tag = "EAGAIN";  break;  // ESP_ERR_HTTP_EAGAIN
    case 0x7008: tag = "CLOSED";  break;  // ESP_ERR_HTTP_CONNECTION_CLOSED
    case 0x7009: tag = "304";     break;  // ESP_ERR_HTTP_NOT_MODIFIED
    case 0x700A: tag = "416";     break;  // ESP_ERR_HTTP_RANGE_NOT_SATISFIABLE
    case 0x700B: tag = "RDTO";    break;  // ESP_ERR_HTTP_READ_TIMEOUT
    case 0x700C: tag = "SHORT";   break;  // ESP_ERR_HTTP_INCOMPLETE_DATA
    /* ---- 0x8000 段:esp_tls / mbedTLS ---- */
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

void we_diag_tag_from_pair(int open_err, int tls_err, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';

    /* tls_err 是分类码(0x8001 DNS / 0x8004 CONN / 0x8006 TOUT / 0x801A TLS…),
       只有它是 0x8000 段才说明 esp_tls 真的记下了原因。0x8000 本身是"基址",
       落在这里等于没记,不当有效值。 */
    if (tls_err > 0x8000) { we_diag_tag_from_err(tls_err, out, cap); return; }
    if (open_err)         { we_diag_tag_from_err(open_err, out, cap); return; }
    str_trunc(out, cap, "?");
}

void we_diag_tag_from_status(int status, bool parsed, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (parsed) { str_trunc(out, cap, "ok"); return; }
    if (status >= 400) { num_to_str(out, cap, (unsigned)status, 10, 0); return; }
    if (status >= 100) { str_trunc(out, cap, "JSON"); return; }   // 通了但字段对不上
    str_trunc(out, cap, "NOSTAT");
}

void we_diag_ip4_str(uint32_t addr, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    size_t o = 0;
    for (int i = 0; i < 4; i++) {              // 低 8 位是第 1 段(lwIP 的存放顺序)
        unsigned v = (addr >> (8 * i)) & 0xFFu;
        if (i > 0 && o + 1 < cap) out[o++] = '.';
        char tmp[3];
        int n = 0;
        do {
            tmp[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        } while (v && n < 3);
        while (n > 0 && o + 1 < cap) out[o++] = tmp[--n];
    }
    out[o] = '\0';                             // o <= cap-1,循环里一直留了终止位
}

void we_diag_tag_from_errno(int err, char *out, size_t cap)
{
    const char *tag = NULL;
    if (err == 0) { str_trunc(out, cap, "ok"); return; }
    switch (err) {
    case 111: tag = "REFUSED"; break;   // ECONNREFUSED
    case 110: tag = "TIMEOUT"; break;   // ETIMEDOUT
    case 101: tag = "NETDOWN"; break;   // ENETUNREACH
    case 113: tag = "HOSTDOWN";break;   // EHOSTUNREACH
    case 104: tag = "RESET";   break;   // ECONNRESET
    case 105: tag = "NOBUF";   break;   // ENOBUFS
    default: break;
    }
    if (tag) { str_trunc(out, cap, tag); return; }
    if (!out || cap == 0) return;
    out[0] = 'E';
    if (cap > 1)
        num_to_str(out + 1, cap - 1, (unsigned)(err < 0 ? -err : err), 10, 0);
}
