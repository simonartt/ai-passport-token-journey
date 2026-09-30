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

int main(void)
{
    test_default_invalid_before_magic();
    test_add_wifi_dedupe_and_caps();
    test_add_prov_dedupe_and_caps();
    test_clear_keeps_header();
    printf("ALL PASS\n");
    return 0;
}
