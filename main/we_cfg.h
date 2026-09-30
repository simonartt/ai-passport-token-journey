// main/we_cfg.h — 社区版配置模型(纯逻辑,零 ESP-IDF/LVGL 依赖,可 host 测试)
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_CFG_MAGIC      0x57454231u   // 'WEB1'
#define WE_CFG_VERSION    1u
#define WE_CFG_MAX_WIFI   3u
#define WE_CFG_MAX_PROV   6u
#define WE_CFG_MAX_KEY    96u           // API Key 上限

// 一组 WiFi 档案(host 可填 IP 或 mDNS 域名)
typedef struct {
    char ssid[33];
    char pass[65];
    char host[48];
} we_wifi_t;

// 一个平台槽位
typedef struct {
    char provider_id[16];   // "deepseek" / "kimi"
    char api_key[WE_CFG_MAX_KEY];
    char label[24];         // 自定义显示名,空=默认
} we_prov_t;

// 整份配置:直接 memcpy 成 NVS blob;头部 magic+version 校验
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t  wifi_count;
    uint8_t  prov_count;
    char     nickname[24];               // 锁屏签名,空=不显示
    we_wifi_t wifi[WE_CFG_MAX_WIFI];
    we_prov_t prov[WE_CFG_MAX_PROV];
} we_cfg_t;

// 默认清空(全 0,含 magic)
void we_cfg_init(we_cfg_t *cfg);

// 校验结构合理性:magic/version/counts/字符串均合法 → true
bool we_cfg_validate(const we_cfg_t *cfg);

// 追加一组 WiFi;重复 ssid 时覆盖旧档;满返回 false
bool we_cfg_add_wifi(we_cfg_t *cfg, const char *ssid, const char *pass, const char *host);

// 追加平台;同 provider_id 覆盖;满返回 false;label 可空
bool we_cfg_add_prov(we_cfg_t *cfg, const char *provider_id, const char *api_key,
                     const char *label);

// 移除此前所有配置(保留 magic/version 已设状态供复用)
void we_cfg_clear(we_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
