// tests/test_we_provider.c — 平台适配解析 host 测试
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "we_provider.h"

static void test_lookup(void)
{
    const we_provider_t *p = NULL;
    assert(we_provider_lookup("deepseek", &p));
    assert(strcmp(p->id, "deepseek") == 0);
    assert(strstr(p->url, "deepseek.com"));
    assert(we_provider_lookup("kimi", &p));
    assert(strstr(p->url, "moonshot.cn"));
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

static void test_unsupported(void)
{
    char amt[32];
    assert(!we_provider_parse("openai", "{}", amt, sizeof(amt)));
    assert(!we_provider_parse("doubao", "{}", amt, sizeof(amt)));
    printf("ok: unsupported\n");
}

int main(void)
{
    test_lookup();
    test_deepseek_parse();
    test_kimi_parse();
    test_unsupported();
    printf("ALL PASS\n");
    return 0;
}
