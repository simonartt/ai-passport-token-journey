// tests/test_we_hermes.c — Hermes 响应解析 host 测试
// 样本形态取自 2026-10-03 用户网关(v0.21.5)真实响应。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "we_hermes.h"

static void test_usage(void)
{
    const char *body =
        "{\"daily\":["
        "{\"day\":\"2026-09-27\",\"input_tokens\":860648,\"output_tokens\":13358,"
        "\"cache_read_tokens\":0,\"reasoning_tokens\":5543,\"estimated_cost\":0.0,"
        "\"actual_cost\":0,\"sessions\":3,\"api_calls\":37},"
        "{\"day\":\"2026-09-28\",\"input_tokens\":2176463,\"output_tokens\":21938,"
        "\"cache_read_tokens\":67116800,\"reasoning_tokens\":9388,\"estimated_cost\":0.5,"
        "\"actual_cost\":0,\"sessions\":14,\"api_calls\":58},"
        "{\"day\":\"2026-10-03\",\"input_tokens\":32674,\"output_tokens\":4880,"
        "\"cache_read_tokens\":1408,\"reasoning_tokens\":0,\"estimated_cost\":0.0,"
        "\"actual_cost\":0,\"sessions\":2,\"api_calls\":10}"
        "]}";
    we_hm_usage_t u;
    assert(we_hm_parse_usage(body, &u));
    assert(u.ok);
    assert(u.days == 3);
    // cache_read 不计入(它是重复读取的缓存,不是新增 token)
    assert(u.tokens == (860648u + 13358 + 5543) + (2176463u + 21938 + 9388) + (32674u + 4880 + 0));
    assert(u.calls == 37 + 58 + 10);
    assert(u.sessions_peak == 14);           // 峰值,不是 3+14+2
    // 无 daily → false
    assert(!we_hm_parse_usage("{\"foo\":1}", &u));
    assert(!we_hm_parse_usage(NULL, &u));
    printf("ok: usage\n");
}

static void test_models(void)
{
    const char *body =
        "{\"models\":["
        "{\"model\":\"deepseek-flash\",\"provider\":\"deepseek\",\"input_tokens\":1000,"
        "\"output_tokens\":100,\"cache_read_tokens\":500,\"reasoning_tokens\":50,"
        "\"estimated_cost\":0.1,\"actual_cost\":0,\"sessions\":6,\"api_calls\":619,"
        "\"tool_calls\":577,\"last_used_at\":1790692047.0,\"capabilities\":{\"supports_tools\":true}},"
        "{\"model\":\"ornith-35b\",\"provider\":\"lmstudio\",\"input_tokens\":9000,"
        "\"output_tokens\":400,\"cache_read_tokens\":0,\"reasoning_tokens\":0,"
        "\"sessions\":2,\"api_calls\":315,\"capabilities\":{}},"
        "{\"model\":\"qwen-local\",\"provider\":\"lmstudio\",\"input_tokens\":100,"
        "\"output_tokens\":10,\"reasoning_tokens\":0,\"sessions\":1,\"api_calls\":5}"
        "]}";
    we_hm_models_t m;
    assert(we_hm_parse_models(body, &m));
    assert(m.count == 3);
    // 每对象独立切片:provider/tokens 不能串到邻居对象
    assert(strcmp(m.item[0].name, "deepseek-flash") == 0);
    assert(strcmp(m.item[0].provider, "deepseek") == 0);
    assert(m.item[0].tokens == 1000 + 100 + 50);      // cache_read 不算
    assert(m.item[0].calls == 619);
    assert(strcmp(m.item[1].name, "ornith-35b") == 0);
    assert(m.item[1].tokens == 9400);
    assert(m.item[2].tokens == 110);
    we_hm_models_sort(&m);
    assert(strcmp(m.item[0].name, "ornith-35b") == 0);   // tokens 降序
    assert(strcmp(m.item[1].name, "deepseek-flash") == 0);
    assert(strcmp(m.item[2].name, "qwen-local") == 0);
    assert(!we_hm_parse_models("{\"foo\":[]}", &m));
    printf("ok: models + sort\n");
}

static void test_status(void)
{
    const char *body =
        "{\"version\":\"0.21.5\",\"gateway_running\":true,"
        "\"gateway_platforms\":{"
        "\"feishu\":{\"state\":\"connected\",\"error_code\":null},"
        "\"telegram\":{\"state\":\"retrying\",\"error_code\":\"telegram_connect_error\"},"
        "\"weixin\":{\"state\":\"connected\"},"
        "\"discord\":{\"state\":\"connected\"}},"
        "\"active_agents\":0,\"overall\":\"ok\"}";
    we_hm_status_t s;
    assert(we_hm_parse_status(body, &s));
    assert(strcmp(s.overall, "ok") == 0);
    assert(s.plat_count == 4);
    assert(strcmp(s.plat[0], "feishu") == 0 && strcmp(s.state[0], "connected") == 0);
    assert(strcmp(s.plat[1], "telegram") == 0 && strcmp(s.state[1], "retrying") == 0);
    assert(strcmp(s.plat[3], "discord") == 0);
    // degraded 场景(用户实测撞过):标题必须恒定,降级只反映在 overall
    const char *deg = "{\"overall\":\"degraded\",\"gateway_platforms\":{"
                      "\"feishu\":{\"state\":\"connected\"}}}";
    assert(we_hm_parse_status(deg, &s));
    assert(strcmp(s.overall, "degraded") == 0);
    assert(s.plat_count == 1);
    // 无平台段:只有 overall 也算解析成功
    assert(we_hm_parse_status("{\"overall\":\"ok\"}", &s) && s.plat_count == 0);
    printf("ok: status\n");
}

static void test_login(void)
{
    assert(we_hm_parse_login("{\"ok\":true,\"next\":\"/\"}"));
    assert(we_hm_parse_login("{\"ok\": true }"));
    assert(!we_hm_parse_login("{\"ok\":false}"));
    assert(!we_hm_parse_login("{}"));
    assert(!we_hm_parse_login(NULL));
    printf("ok: login\n");
}

// 大响应冒烟:模拟 30 天 × 长字段(接近真实 18KB),确认无栈溢出/无截断逻辑错误
static void test_big(void)
{
    static char body[40000];
    size_t o = 0;
    o += (size_t)snprintf(body + o, sizeof(body) - o, "{\"daily\":[");
    for (int i = 0; i < 30; i++)
        o += (size_t)snprintf(body + o, sizeof(body) - o,
            "%s{\"day\":\"2026-09-%02d\",\"input_tokens\":1000000,\"output_tokens\":10000,"
            "\"cache_read_tokens\":5000000,\"reasoning_tokens\":1000,\"estimated_cost\":0.01,"
            "\"actual_cost\":0,\"sessions\":%d,\"api_calls\":%d}",
            i ? "," : "", i + 1, i, i * 10);
    snprintf(body + o, sizeof(body) - o, "]}");
    we_hm_usage_t u;
    assert(we_hm_parse_usage(body, &u));
    assert(u.days == 30);
    assert(u.tokens == 30ull * (1000000ull + 10000ull + 1000ull));
    assert(u.calls == 4350);                          // Σ i*10 (i=0..29)
    assert(u.sessions_peak == 29);
    printf("ok: big payload\n");
}

int main(void)
{
    test_usage();
    test_models();
    test_status();
    test_login();
    test_big();
    printf("ALL PASS\n");
    return 0;
}
