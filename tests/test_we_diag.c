// tests/test_we_diag.c — we_diag 纯逻辑 host 测试(assert,直接 cc 编译运行)
//
// 这些断言守的是"屏幕上那个短码能不能被信":设备出问题时人只能把屏幕上的
// 几个字母抄下来,映射错了等于把人带沟里。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "we_diag.h"

static const char *tag_err(int err)
{
    static char buf[WE_DIAG_TAG_MAX];
    memset(buf, 'x', sizeof(buf));
    we_diag_tag_from_err(err, buf, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

static const char *tag_status(int status, int parsed)
{
    static char buf[WE_DIAG_TAG_MAX];
    memset(buf, 'x', sizeof(buf));
    we_diag_tag_from_status(status, parsed != 0, buf, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

static void test_err_mapping(void)
{
    // 值取自 esp_tls_errors.h:ESP_ERR_ESP_TLS_BASE(0x8000) + 偏移
    assert(strcmp(tag_err(0x8001), "DNS") == 0);     // CANNOT_RESOLVE_HOSTNAME
    assert(strcmp(tag_err(0x8002), "SOCK") == 0);    // CANNOT_CREATE_SOCKET
    assert(strcmp(tag_err(0x8004), "CONN") == 0);    // FAILED_CONNECT_TO_HOST
    assert(strcmp(tag_err(0x8006), "TOUT") == 0);    // CONNECTION_TIMEOUT
    assert(strcmp(tag_err(0x8009), "TLSTO") == 0);   // SERVER_HANDSHAKE_TIMEOUT
    assert(strcmp(tag_err(0x8015), "X509") == 0);    // X509_CRT_PARSE_FAILED
    assert(strcmp(tag_err(0x8017), "TLS") == 0);     // SSL_SETUP_FAILED(常=内存不够)
    assert(strcmp(tag_err(0x801A), "TLS") == 0);     // SSL_HANDSHAKE_FAILED
    assert(strcmp(tag_err(0x801F), "TLS") == 0);     // 未来新增的 esp-tls 码也归 TLS
    // 0x7000 段(esp_http_client.h 的 ESP_ERR_HTTP_*)。设备报的第一个码就是这里的
    // 0x7002,不认它就只能显示一个没人看得懂的 "E7002"。
    assert(strcmp(tag_err(0x7001), "REDIR") == 0);   // ESP_ERR_HTTP_MAX_REDIRECT
    assert(strcmp(tag_err(0x7002), "CONN") == 0);    // ESP_ERR_HTTP_CONNECT(兜底码)
    assert(strcmp(tag_err(0x7003), "WRITE") == 0);
    assert(strcmp(tag_err(0x7004), "HDR") == 0);
    assert(strcmp(tag_err(0x7005), "NOTRANS") == 0); // INVALID_TRANSPORT(没开 HTTPS)
    assert(strcmp(tag_err(0x7006), "CONNING") == 0);
    assert(strcmp(tag_err(0x7007), "EAGAIN") == 0);
    assert(strcmp(tag_err(0x7008), "CLOSED") == 0);
    assert(strcmp(tag_err(0x700B), "RDTO") == 0);
    assert(strcmp(tag_err(0x700C), "SHORT") == 0);
    // 非 esp-tls 段:保留原始码,方便直接查表
    assert(strcmp(tag_err(0x101), "E0101") == 0);    // ESP_ERR_NO_MEM
    assert(strcmp(tag_err(0), "E0000") == 0);
    printf("ok: err -> tag\n");
}

static void test_pair_choice(void)
{
    static char b[WE_DIAG_TAG_MAX];
    // tls_err 是分类码,只要它 > 0x8000 就优先:0x7002 只说明"连接阶段失败"
    we_diag_tag_from_pair(0x7002, 0x8001, b, sizeof(b));
    assert(strcmp(b, "DNS") == 0);                   // 域名没解析出来
    we_diag_tag_from_pair(0x7002, 0x8004, b, sizeof(b));
    assert(strcmp(b, "CONN") == 0);
    we_diag_tag_from_pair(0x7002, 0x8006, b, sizeof(b));
    assert(strcmp(b, "TOUT") == 0);
    we_diag_tag_from_pair(0x7002, 0x801A, b, sizeof(b));
    assert(strcmp(b, "TLS") == 0);
    // esp_tls 什么都没记下 → 退回 open_err(至少还知道是连接阶段)
    we_diag_tag_from_pair(0x7002, 0, b, sizeof(b));
    assert(strcmp(b, "CONN") == 0);
    // 0x8000 是基址本身,等于没信息,不能当成有效分类码
    we_diag_tag_from_pair(0x7002, 0x8000, b, sizeof(b));
    assert(strcmp(b, "CONN") == 0);
    // 两个都没有
    we_diag_tag_from_pair(0, 0, b, sizeof(b));
    assert(strcmp(b, "?") == 0);
    we_diag_tag_from_pair(0, 0, NULL, 0);            // 空指针不能炸
    printf("ok: pair -> tag\n");
}

static void test_errno_mapping(void)
{
    static char b[WE_DIAG_TAG_MAX];
    we_diag_tag_from_errno(0, b, sizeof(b));
    assert(strcmp(b, "ok") == 0);
    we_diag_tag_from_errno(111, b, sizeof(b));
    assert(strcmp(b, "REFUSED") == 0);               // ECONNREFUSED
    we_diag_tag_from_errno(110, b, sizeof(b));
    assert(strcmp(b, "TIMEOUT") == 0);               // ETIMEDOUT
    we_diag_tag_from_errno(101, b, sizeof(b));
    assert(strcmp(b, "NETDOWN") == 0);               // ENETUNREACH
    we_diag_tag_from_errno(113, b, sizeof(b));
    assert(strcmp(b, "HOSTDOWN") == 0);              // EHOSTUNREACH
    we_diag_tag_from_errno(104, b, sizeof(b));
    assert(strcmp(b, "RESET") == 0);                 // ECONNRESET
    we_diag_tag_from_errno(42, b, sizeof(b));        // 认不出就报原值,别编
    assert(strcmp(b, "E42") == 0);
    we_diag_tag_from_errno(-9, b, sizeof(b));
    assert(strcmp(b, "E9") == 0);
    we_diag_tag_from_errno(9, NULL, 0);              // 空指针不能炸
    printf("ok: errno -> tag\n");
}

// 细化只应该发生在"只有兜底码"的行上:拔掉这个约束就会把更权威的结论覆盖掉。
static void test_refine_conn_tag(void)
{
    we_diag_reset();

    we_diag_row_t row;
    memset(&row, 0, sizeof(row));
    snprintf(row.id, sizeof(row.id), "%s", "deepseek");

    // tls_err == 0(esp_tls 没记下原因)+ tag == CONN → 允许细化
    snprintf(row.tag, sizeof(row.tag), "%s", "CONN");
    row.tls_err = 0;
    we_diag_set_row(0, &row);
    we_diag_refine_conn_tag(0, "DNS");
    assert(strcmp(we_diag_get()->row[0].tag, "DNS") == 0);

    // tls_err 已经给出了具体原因 → 不许覆盖
    snprintf(row.tag, sizeof(row.tag), "%s", "CONN");
    row.tls_err = 0x8004;
    we_diag_set_row(1, &row);
    we_diag_refine_conn_tag(1, "DNS");
    assert(strcmp(we_diag_get()->row[1].tag, "CONN") == 0);

    // 非 CONN 的短码(401 之类)本来就更具体 → 不许动
    snprintf(row.tag, sizeof(row.tag), "%s", "401");
    row.tls_err = 0;
    we_diag_set_row(2, &row);
    we_diag_refine_conn_tag(2, "DNS");
    assert(strcmp(we_diag_get()->row[2].tag, "401") == 0);

    // 越界 / 空串必须被忽略
    we_diag_refine_conn_tag(-1, "DNS");
    we_diag_refine_conn_tag(WE_DIAG_ROWS, "DNS");
    we_diag_refine_conn_tag(0, NULL);
    we_diag_refine_conn_tag(0, "");
    assert(strcmp(we_diag_get()->row[0].tag, "DNS") == 0);
    printf("ok: refine conn tag\n");
}

static void test_net_snapshot(void)
{
    we_diag_reset();

    // probed == false 的探测结果要整段忽略(别把空结构当成结论)
    we_diag_net_t net;
    memset(&net, 0, sizeof(net));
    we_diag_set_net(&net);
    assert(!we_diag_get()->net.probed);

    memset(&net, 'X', sizeof(net));                  // 故意不带 '\0' 的脏数据
    net.probed = true;
    net.rc = 0x8001;
    net.sock_errno = 0;
    net.ms = 321;
    we_diag_set_net(&net);
    const we_diag_net_t *got = &we_diag_get()->net;
    assert(got->probed && got->rc == 0x8001 && got->ms == 321);
    assert(got->ip[sizeof(got->ip) - 1] == '\0');     // 必须被补齐终止符
    assert(got->host[sizeof(got->host) - 1] == '\0');
    assert(got->dns2[sizeof(got->dns2) - 1] == '\0');

    we_diag_set_net(NULL);                            // 空指针不能炸
    printf("ok: net snapshot\n");
}

static void test_status_mapping(void)
{
    assert(strcmp(tag_status(200, 1), "ok") == 0);
    assert(strcmp(tag_status(200, 0), "JSON") == 0);   // 通了但字段对不上
    assert(strcmp(tag_status(401, 0), "401") == 0);    // Key 无效
    assert(strcmp(tag_status(403, 0), "403") == 0);
    assert(strcmp(tag_status(429, 0), "429") == 0);
    assert(strcmp(tag_status(500, 0), "500") == 0);
    assert(strcmp(tag_status(-1, 0), "NOSTAT") == 0);
    assert(strcmp(tag_status(0, 0), "NOSTAT") == 0);
    printf("ok: status -> tag\n");
}

static void test_tiny_buffer_is_terminated(void)
{
    char small[5];
    memset(small, 'x', sizeof(small));
    we_diag_tag_from_err(0x8015, small, sizeof(small));   // "X509" 正好放得下
    assert(memchr(small, '\0', sizeof(small)) != NULL);
    assert(strcmp(small, "X509") == 0);

    memset(small, 'x', sizeof(small));
    we_diag_tag_from_err(0x101, small, sizeof(small));    // "E0101" 放不下 → 截成 "E010"
    assert(small[sizeof(small) - 1] == '\0');
    assert(strcmp(small, "E010") == 0);

    memset(small, 'x', sizeof(small));
    we_diag_tag_from_status(401, 0, small, sizeof(small));   // "401" 放得下
    assert(memchr(small, '\0', sizeof(small)) != NULL);
    assert(strcmp(small, "401") == 0);

    we_diag_tag_from_err(1, NULL, 0);                     // 空指针不能炸
    we_diag_tag_from_status(1, 0, NULL, 0);
    printf("ok: tiny/NULL buffers\n");
}

static void test_snapshot_roundtrip(void)
{
    we_diag_reset();
    const we_diag_t *d = we_diag_get();
    assert(d->seq == 1);
    assert(d->n_ok == 0 && d->n_total == 0);
    assert(d->row[0].http_status == -1);            // -1 = 没拿到状态码

    we_diag_reset();                                 // 第二次刷新:seq 自增,数据清零
    d = we_diag_get();
    assert(d->seq == 2);
    assert(d->row[1].http_status == -1);

    we_diag_set_stage("FETCH");
    assert(strcmp(we_diag_get()->stage, "FETCH") == 0);

    we_diag_row_t row;
    memset(&row, 0, sizeof(row));
    snprintf(row.id, sizeof(row.id), "%s", "deepseek");
    snprintf(row.tag, sizeof(row.tag), "%s", "401");
    row.http_status = 401;
    row.open_err = 0;
    row.attempts = 2;
    row.millis = 1234;
    we_diag_set_row(0, &row);
    d = we_diag_get();
    assert(strcmp(d->row[0].id, "deepseek") == 0);
    assert(strcmp(d->row[0].tag, "401") == 0);
    assert(d->row[0].http_status == 401);
    assert(d->row[0].attempts == 2);

    // 越界/空指针必须被忽略而不是踩内存
    we_diag_set_row(-1, &row);
    we_diag_set_row(WE_DIAG_ROWS, &row);
    we_diag_set_row(0, NULL);

    we_diag_set_env(0, true, 1000, 500, 400);
    we_diag_finish(1, 2);
    d = we_diag_get();
    assert(d->wifi_rc == 0 && d->clock_ok);
    assert(d->free_heap == 1000 && d->min_free_heap == 500 && d->largest_block == 400);
    assert(d->n_ok == 1 && d->n_total == 2);
    printf("ok: snapshot roundtrip\n");
}

static void test_overlong_strings_are_clamped(void)
{
    we_diag_row_t row;
    memset(&row, 0, sizeof(row));
    memset(row.id, 'A', sizeof(row.id));            // 故意不带 '\0'
    memset(row.tag, 'B', sizeof(row.tag));
    memset(row.body, 'C', sizeof(row.body));
    we_diag_set_row(1, &row);

    const we_diag_row_t *got = &we_diag_get()->row[1];
    assert(got->id[sizeof(got->id) - 1] == '\0');
    assert(got->tag[sizeof(got->tag) - 1] == '\0');
    assert(got->body[sizeof(got->body) - 1] == '\0');
    assert(strlen(got->id) == sizeof(got->id) - 1);
    assert(strlen(got->body) == sizeof(got->body) - 1);
    printf("ok: overlong strings clamped\n");
}

int main(void)
{
    test_err_mapping();
    test_pair_choice();
    test_errno_mapping();
    test_status_mapping();
    test_tiny_buffer_is_terminated();
    test_snapshot_roundtrip();
    test_overlong_strings_are_clamped();
    test_refine_conn_tag();
    test_net_snapshot();
    printf("ALL PASS\n");
    return 0;
}
