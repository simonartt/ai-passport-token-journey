// main/we_cfg.c — 社区版配置模型实现(纯逻辑)
#include "we_cfg.h"

#include <string.h>

void we_cfg_init(we_cfg_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic = WE_CFG_MAGIC;
    cfg->version = WE_CFG_VERSION;
}

static bool str_ok(const char *s, size_t cap)
{
    // 允许空串;非空必须能在 cap 内放得下并带终结符
    for (size_t i = 0; i < cap; i++) {
        if (s[i] == '\0') return true;
    }
    return false;   // 没找到终结符 = 越界
}

bool we_cfg_validate(const we_cfg_t *cfg)
{
    if (!cfg) return false;
    if (cfg->magic != WE_CFG_MAGIC) return false;
    if (cfg->version != WE_CFG_VERSION) return false;
    if (cfg->wifi_count > WE_CFG_MAX_WIFI) return false;
    if (cfg->prov_count > WE_CFG_MAX_PROV) return false;
    if (!str_ok(cfg->nickname, sizeof(cfg->nickname))) return false;

    for (uint8_t i = 0; i < cfg->wifi_count; i++) {
        const we_wifi_t *w = &cfg->wifi[i];
        if (!str_ok(w->ssid, sizeof(w->ssid))) return false;
        if (!str_ok(w->pass, sizeof(w->pass))) return false;
        if (!str_ok(w->host, sizeof(w->host))) return false;
        if (w->ssid[0] == '\0') return false;   // 档案必须有 SSID
    }
    for (uint8_t i = 0; i < cfg->prov_count; i++) {
        const we_prov_t *p = &cfg->prov[i];
        if (!str_ok(p->provider_id, sizeof(p->provider_id))) return false;
        if (!str_ok(p->api_key, sizeof(p->api_key))) return false;
        if (!str_ok(p->label, sizeof(p->label))) return false;
        if (p->provider_id[0] == '\0') return false;
        if (p->api_key[0] == '\0') return false;
    }
    return true;
}

bool we_cfg_add_wifi(we_cfg_t *cfg, const char *ssid, const char *pass, const char *host)
{
    if (!cfg || !ssid || !pass) return false;
    size_t sl = strlen(ssid), pl = strlen(pass), hl = host ? strlen(host) : 0;
    if (sl == 0 || sl >= sizeof(cfg->wifi[0].ssid)) return false;
    if (pl >= sizeof(cfg->wifi[0].pass)) return false;
    if (hl >= sizeof(cfg->wifi[0].host)) return false;

    uint8_t idx = cfg->wifi_count;
    for (uint8_t i = 0; i < cfg->wifi_count; i++) {
        if (strcmp(cfg->wifi[i].ssid, ssid) == 0) { idx = i; break; }
    }
    if (idx >= WE_CFG_MAX_WIFI) return false;
    if (idx == cfg->wifi_count) cfg->wifi_count++;

    memset(&cfg->wifi[idx], 0, sizeof(cfg->wifi[idx]));
    memcpy(cfg->wifi[idx].ssid, ssid, sl + 1);
    memcpy(cfg->wifi[idx].pass, pass, pl + 1);
    if (host) memcpy(cfg->wifi[idx].host, host, hl + 1);
    return true;
}

bool we_cfg_add_prov(we_cfg_t *cfg, const char *provider_id, const char *api_key,
                     const char *label)
{
    if (!cfg || !provider_id || !api_key) return false;
    size_t idl = strlen(provider_id), kl = strlen(api_key), ll = label ? strlen(label) : 0;
    if (idl == 0 || idl >= sizeof(cfg->prov[0].provider_id)) return false;
    if (kl == 0 || kl >= sizeof(cfg->prov[0].api_key)) return false;
    if (ll >= sizeof(cfg->prov[0].label)) return false;

    uint8_t idx = cfg->prov_count;
    for (uint8_t i = 0; i < cfg->prov_count; i++) {
        if (strcmp(cfg->prov[i].provider_id, provider_id) == 0) { idx = i; break; }
    }
    if (idx >= WE_CFG_MAX_PROV) return false;
    if (idx == cfg->prov_count) cfg->prov_count++;

    memset(&cfg->prov[idx], 0, sizeof(cfg->prov[idx]));
    memcpy(cfg->prov[idx].provider_id, provider_id, idl + 1);
    memcpy(cfg->prov[idx].api_key, api_key, kl + 1);
    if (label) memcpy(cfg->prov[idx].label, label, ll + 1);
    return true;
}

void we_cfg_clear(we_cfg_t *cfg)
{
    if (!cfg) return;
    uint32_t magic = cfg->magic;
    uint16_t version = cfg->version;
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic = magic;
    cfg->version = version;
}
