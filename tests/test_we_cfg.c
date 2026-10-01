// tests/test_we_cfg.c — we_cfg 纯逻辑 host 测试(assert,直接 cc 编译运行)
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "we_cfg.h"

static void test_default_invalid_before_magic(void)
{
    we_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    assert(!we_cfg_validate(&cfg));           // 无 magic 非法
    we_cfg_init(&cfg);
    assert(we_cfg_validate(&cfg));            // init 后为空但合法
    assert(cfg.wifi_count == 0 && cfg.prov_count == 0);
    printf("ok: default init/validate\n");
}

static void test_add_wifi_dedupe_and_caps(void)
{
    we_cfg_t cfg;
    we_cfg_init(&cfg);

    assert(we_cfg_add_wifi(&cfg, "HomeWiFi", "pass1", "192.168.1.10"));
    assert(we_cfg_add_wifi(&cfg, "OPPO Find N6", "pass2", "air.local"));
    assert(we_cfg_add_wifi(&cfg, "Office", "pass3", ""));
    assert(cfg.wifi_count == 3);
    assert(!we_cfg_add_wifi(&cfg, "TooMany", "x", ""));          // 满
    assert(we_cfg_validate(&cfg));

    // 同名覆盖,不新增
    assert(we_cfg_add_wifi(&cfg, "HomeWiFi", "newpass", "10.0.0.2"));
    assert(cfg.wifi_count == 3);
    assert(strcmp(cfg.wifi[0].pass, "newpass") == 0);
    assert(we_cfg_validate(&cfg));

    // 空 SSID / 超长拒绝
    assert(!we_cfg_add_wifi(&cfg, "", "p", ""));
    char longssid[40];
    memset(longssid, 'A', sizeof(longssid));
    longssid[39] = '\0';
    assert(!we_cfg_add_wifi(&cfg, longssid, "p", ""));
    printf("ok: add_wifi dedupe/caps\n");
}

static void test_add_prov_dedupe_and_caps(void)
{
    we_cfg_t cfg;
    we_cfg_init(&cfg);

    assert(we_cfg_add_prov(&cfg, "deepseek", "sk-a", "DeepSeek"));
    assert(we_cfg_add_prov(&cfg, "kimi", "sk-b", NULL));         // label 可空
    assert(we_cfg_add_prov(&cfg, "deepseek", "sk-a2", "DS"));    // 覆盖
    assert(cfg.prov_count == 2);
    assert(strcmp(cfg.prov[0].api_key, "sk-a2") == 0);
    assert(we_cfg_validate(&cfg));

    for (uint8_t i = 2; i < WE_CFG_MAX_PROV; i++) {
        char id[10];
        snprintf(id, sizeof(id), "p%d", i);
        assert(we_cfg_add_prov(&cfg, id, "k", NULL));
    }
    assert(cfg.prov_count == WE_CFG_MAX_PROV);
    assert(!we_cfg_add_prov(&cfg, "overflow", "k", NULL));
    assert(we_cfg_validate(&cfg));
    printf("ok: add_prov dedupe/caps\n");
}

static void test_clear_keeps_header(void)
{
    we_cfg_t cfg;
    we_cfg_init(&cfg);
    we_cfg_add_wifi(&cfg, "W", "P", "");
    we_cfg_add_prov(&cfg, "deepseek", "sk", NULL);
    we_cfg_clear(&cfg);
    assert(cfg.wifi_count == 0 && cfg.prov_count == 0);
    assert(cfg.magic == WE_CFG_MAGIC && cfg.version == WE_CFG_VERSION);
    assert(we_cfg_validate(&cfg));
    printf("ok: clear keeps header\n");
}

// ---------------------------------------------------------------- Key 清洗
// 门户里 Key 是手机上粘贴进去的,复制时经常带尾随换行/空格。换行会进 HTTP 头
// 把请求搞成非法请求,现象和"Key 填错了"完全一样,所以必须在这里拦掉。
static void test_sanitize_key(void)
{
    char out[WE_CFG_MAX_KEY];

    assert(we_cfg_sanitize_key("sk-abc123", out, sizeof(out)));
    assert(strcmp(out, "sk-abc123") == 0);

    // 尾随换行 / 空格 / 制表 / 前导空白
    assert(we_cfg_sanitize_key("sk-abc123\r\n", out, sizeof(out)));
    assert(strcmp(out, "sk-abc123") == 0);
    assert(we_cfg_sanitize_key("  sk-abc123  ", out, sizeof(out)));
    assert(strcmp(out, "sk-abc123") == 0);
    assert(we_cfg_sanitize_key("\tsk-a\tb\nc\n", out, sizeof(out)));
    assert(strcmp(out, "sk-abc") == 0);              // 中间的换行也要去掉

    // 整段 "Bearer xxx" 粘进来
    assert(we_cfg_sanitize_key("Bearer sk-abc123", out, sizeof(out)));
    assert(strcmp(out, "sk-abc123") == 0);
    assert(we_cfg_sanitize_key("bearer\tsk-abc123", out, sizeof(out)));
    assert(strcmp(out, "sk-abc123") == 0);
    // 只是名字里带 bearer 的 Key 不能被误截
    assert(we_cfg_sanitize_key("bearerx-1", out, sizeof(out)));
    assert(strcmp(out, "bearerx-1") == 0);

    // 只剩空白 = 没填
    assert(!we_cfg_sanitize_key("   \r\n\t ", out, sizeof(out)));
    assert(!we_cfg_sanitize_key("", out, sizeof(out)));
    assert(!we_cfg_sanitize_key(NULL, out, sizeof(out)));
    assert(!we_cfg_sanitize_key("Bearer ", out, sizeof(out)));

    // 截断后仍然是以 '\0' 结尾的合法串
    char small[6];
    memset(small, 'x', sizeof(small));
    assert(we_cfg_sanitize_key("sk-abcdefgh", small, sizeof(small)));
    assert(small[sizeof(small) - 1] == '\0');
    assert(strcmp(small, "sk-ab") == 0);
    printf("ok: sanitize_key\n");
}

// 门户保存语义:留空 = 不改,非空才覆盖(这条曾经把已配的 Key 静默抹掉)
static void test_set_prov_key_incremental(void)
{
    we_cfg_t cfg;
    we_cfg_init(&cfg);
    we_cfg_add_wifi(&cfg, "Home", "secret", "");

    assert(we_cfg_set_prov_key(&cfg, "deepseek", "sk-original", NULL));
    assert(cfg.prov_count == 1);
    assert(strcmp(cfg.prov[0].api_key, "sk-original") == 0);

    // 空 = 不修改(只改 WiFi 的那次提交)
    assert(we_cfg_set_prov_key(&cfg, "deepseek", "", NULL));
    assert(strcmp(cfg.prov[0].api_key, "sk-original") == 0);
    assert(!we_cfg_set_prov_key(&cfg, "kimi", "", NULL));      // 不存在的平台 + 空 = false
    assert(cfg.prov_count == 1);

    // 只剩空白 = 视作没填,同样不该改
    assert(we_cfg_set_prov_key(&cfg, "deepseek", "  \r\n", NULL));
    assert(strcmp(cfg.prov[0].api_key, "sk-original") == 0);

    // 非空才覆盖,并且顺手清洗
    assert(we_cfg_set_prov_key(&cfg, "deepseek", " sk-new\n", NULL));
    assert(strcmp(cfg.prov[0].api_key, "sk-new") == 0);
    assert(cfg.prov_count == 1);                               // 同 id 覆盖,不新增

    assert(we_cfg_has_prov(&cfg, "deepseek"));
    assert(!we_cfg_has_prov(&cfg, "kimi"));
    assert(we_cfg_validate(&cfg));
    printf("ok: set_prov_key incremental\n");
}

int main(void)
{
    test_default_invalid_before_magic();
    test_add_wifi_dedupe_and_caps();
    test_add_prov_dedupe_and_caps();
    test_clear_keeps_header();
    test_sanitize_key();
    test_set_prov_key_incremental();
    printf("ALL PASS\n");
    return 0;
}
