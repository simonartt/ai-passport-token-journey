// tests/test_we_provider.c — 平台适配解析 host 测试
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "we_diag.h"      // 只用 WE_DIAG_HOST_MAX 这个常量(host 要能塞进探测缓冲)
#include "we_provider.h"

static void test_lookup(void)
{
    const we_provider_t *p = NULL;
    assert(we_provider_lookup("deepseek", &p));
    assert(strcmp(p->id, "deepseek") == 0);
    assert(strstr(p->url, "deepseek.com"));
    // host 要能塞进 WE_DIAG_HOST_MAX 的网络探测缓冲,而且必须是纯域名
    assert(strcmp(p->host, "api.deepseek.com") == 0);
    assert(strchr(p->host, '/') == NULL);
    assert(strlen(p->host) < WE_DIAG_HOST_MAX);
    assert(we_provider_lookup("kimi", &p));
    assert(strstr(p->url, "moonshot.cn"));
    assert(strcmp(p->host, "api.moonshot.cn") == 0);
    assert(strlen(p->host) < WE_DIAG_HOST_MAX);
    assert(strstr(p->url, p->host) != NULL);        // host 必须真是 url 里那一段
    assert(we_provider_lookup("openrouter", &p));
    assert(strstr(p->url, "/api/v1/credits"));
    assert(strcmp(p->host, "openrouter.ai") == 0);
    assert(strlen(p->host) < WE_DIAG_HOST_MAX);
    assert(strstr(p->url, p->host) != NULL);
    // 币种标签:美元平台靠它决定要不要折算
    assert(strcmp(p->currency, "USD") == 0);
    assert(we_provider_lookup("deepseek", &p));
    assert(strcmp(p->currency, "CNY") == 0);
    assert(we_provider_lookup("kimi", &p));
    assert(strcmp(p->currency, "CNY") == 0);
    assert(!we_provider_lookup("openai", &p));
    assert(!we_provider_lookup("doubao", &p));
    assert(!we_provider_lookup("", &p));
    printf("ok: lookup\n");
}

static void test_deepseek_parse(void)
{
    char amt[32];
    const char *body =
        "{\"is_available\":true,\"balance_infos\":[{\"currency\":\"CNY\","
        "\"total_balance\":\"146.09\",\"topped_up_balance\":\"146.09\","
        "\"granted_balance\":\"0.00\"}]}";
    assert(we_provider_parse("deepseek", body, amt, sizeof(amt)));
    assert(strcmp(amt, "146.09") == 0);

    // 无符号数值形式也应支持
    const char *body2 = "{\"balance_infos\":[{\"total_balance\": 12.5}]}";
    assert(we_provider_parse("deepseek", body2, amt, sizeof(amt)));
    assert(strcmp(amt, "12.5") == 0);

    // 错误体:无字段
    assert(!we_provider_parse("deepseek", "{\"error\":{\"message\":\"bad key\"}}", amt, sizeof(amt)));
    assert(!we_provider_parse("deepseek", "", amt, sizeof(amt)));
    printf("ok: deepseek parse\n");
}

static void test_kimi_parse(void)
{
    char amt[32];
    const char *body =
        "{\"status\":true,\"data\":{\"available_balance\":\"51.83\","
        "\"cash_balance\":\"51.83\",\"voucher_balance\":\"0.00\"}}";
    assert(we_provider_parse("kimi", body, amt, sizeof(amt)));
    assert(strcmp(amt, "51.83") == 0);

    assert(!we_provider_parse("kimi", "{\"status\":false}", amt, sizeof(amt)));
    printf("ok: kimi parse\n");
}

static void test_openrouter_parse(void)
{
    char amt[32];
    // 官方示例:{"data":{"total_credits":100.5,"total_usage":25.75}} → 剩余 74.75
    assert(we_provider_parse("openrouter",
        "{\"data\":{\"total_credits\":100.5,\"total_usage\":25.75}}", amt, sizeof(amt)));
    assert(strcmp(amt, "74.75") == 0);

    // 整数补两位小数
    assert(we_provider_parse("openrouter",
        "{\"data\":{\"total_credits\":30,\"total_usage\":0}}", amt, sizeof(amt)));
    assert(strcmp(amt, "30.00") == 0);

    // 小数减法(注意:二进制浮点在半美分边界可能差 0.01,余额显示可接受)
    assert(we_provider_parse("openrouter",
        "{\"data\":{\"total_credits\":10,\"total_usage\":0.25}}", amt, sizeof(amt)));
    assert(strcmp(amt, "9.75") == 0);
    assert(we_provider_parse("openrouter",
        "{\"data\":{\"total_credits\":0.01,\"total_usage\":0}}", amt, sizeof(amt)));
    assert(strcmp(amt, "0.01") == 0);

    // 用到只剩 0:显示 0.00
    assert(we_provider_parse("openrouter",
        "{\"data\":{\"total_credits\":12.34,\"total_usage\":12.34}}", amt, sizeof(amt)));
    assert(strcmp(amt, "0.00") == 0);

    // 缺任一字段 = 解析失败(比如 403 的 error 响应)
    assert(!we_provider_parse("openrouter",
        "{\"error\":{\"code\":403,\"message\":\"Only management keys\"}}", amt, sizeof(amt)));
    assert(!we_provider_parse("openrouter",
        "{\"data\":{\"total_credits\":100.5}}", amt, sizeof(amt)));
    assert(!we_provider_parse("openrouter", "{}", amt, sizeof(amt)));
    printf("ok: openrouter parse\n");
}

static void test_unsupported(void)
{
    char amt[32];
    assert(!we_provider_parse("openai", "{}", amt, sizeof(amt)));
    assert(!we_provider_parse("doubao", "{}", amt, sizeof(amt)));
    printf("ok: unsupported\n");
}

static void test_rate_parse(void)
{
    double r = 0;
    // frankfurter.dev 形态
    assert(we_rate_parse("{\"amount\":1.0,\"base\":\"USD\",\"date\":\"2026-10-02\","
                         "\"rates\":{\"CNY\":6.7046}}", &r));
    assert(r > 6.70 && r < 6.71);
    // er-api.com 形态(rates 里夹一大串其它币种,CNY 在中间)
    assert(we_rate_parse("{\"result\":\"success\",\"rates\":"
                         "{\"USD\":1,\"EUR\":0.9,\"CNY\":6.714383,\"JPY\":146.0}}", &r));
    assert(r > 6.71 && r < 6.72);
    // 出界拒绝(4.0~12.0):防抓错字段/脏数据
    assert(!we_rate_parse("{\"rates\":{\"CNY\":1756789012}}", &r));   // 时间戳级
    assert(!we_rate_parse("{\"rates\":{\"CNY\":0.5}}", &r));
    assert(!we_rate_parse("{\"rates\":{\"CNY\":13.5}}", &r));
    // 没有 CNY / 非数字 / 空
    assert(!we_rate_parse("{\"rates\":{\"EUR\":0.9}}", &r));
    assert(!we_rate_parse("{\"rates\":{\"CNY\":\"abc\"}}", &r));
    assert(!we_rate_parse("{}", &r));
    assert(!we_rate_parse(NULL, &r));
    printf("ok: rate parse\n");
}

static void test_to_cny(void)
{
    // CNY 原样;USD 乘汇率;未知/空原样;没汇率(rate<=0)不折
    assert(we_provider_to_cny("CNY", 146.09, 6.7) == 146.09);
    double v = we_provider_to_cny("USD", 10.0, 6.7);
    assert(v > 66.9 && v < 67.1);
    assert(we_provider_to_cny("USD", 10.0, 0) == 10.0);      // 没汇率:宁可标价不折
    assert(we_provider_to_cny("GBP", 10.0, 6.7) == 10.0);    // 未知币种原样
    assert(we_provider_to_cny(NULL, 10.0, 6.7) == 10.0);
    printf("ok: to_cny\n");
}

int main(void)
{
    test_lookup();
    test_deepseek_parse();
    test_kimi_parse();
    test_openrouter_parse();
    test_unsupported();
    test_rate_parse();
    test_to_cny();
    printf("ALL PASS\n");
    return 0;
}
