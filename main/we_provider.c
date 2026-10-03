// main/we_provider.c — 平台适配器实现(纯逻辑,零 ESP-IDF/LVGL 依赖)
#include "we_provider.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- 轻量 JSON 字段提取
// 从 body 中找 "key": 并取出值(字符串或裸数字),写入 out。成功返回 true。
static bool json_field(const char *body, const char *key, char *out, size_t cap)
{
    if (!body || !key || !out || cap == 0) return false;

    size_t kl = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        // 确认 key 前是 '"' 边界
        if (p == body || p[-1] != '"' || p[kl] != '"') { p += kl; continue; }
        const char *q = p + kl;              // 指向 key 后的 '"'
        while (*q && *q != ':') q++;
        if (*q != ':') return false;
        q++;
        while (*q && (unsigned char)*q <= ' ') q++;   // 跳过空白

        bool quoted = (*q == '"');
        if (quoted) q++;

        size_t out_len = 0;
        while (*q && out_len + 1 < cap) {
            char c = *q;
            if (quoted) {
                if (c == '"') break;
                if (c == '\\') { q++; if (!*q) break; }   // 简单转义跳过
                out[out_len++] = c;
            } else {
                if (c == ',' || c == '}' || c == ' ' || c == '\n' || c == '\r' ||
                    c == '\t') break;
                out[out_len++] = c;
            }
            q++;
        }
        if (out_len == 0) return false;
        out[out_len] = '\0';

        // 数值类字段只保留数字/小数点
        for (size_t i = 0; i < out_len; i++) {
            if (!isdigit((unsigned char)out[i]) && out[i] != '.') return false;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- 各平台解析
// DeepSeek: {"balance_infos":[{"currency":"CNY","total_balance":"146.09",...}],...}
static bool parse_deepseek(const char *body, char *amount, size_t cap)
{
    return json_field(body, "total_balance", amount, cap);
}

// Kimi: {"status":true,"data":{"available_balance":"51.83",...}}
static bool parse_kimi(const char *body, char *amount, size_t cap)
{
    return json_field(body, "available_balance", amount, cap);
}

// 金额格式化:两位小数,自拼而不是 snprintf。缓冲由调用方决定,编译期大小未知,
// GCC 的 -Werror=format-truncation 会按 "%.2f" 的最坏宽度(~320 字符)报警。
static void fmt_amount(double v, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (v < 0) v = 0;                       // 透支显示为 0.00,避免负号撑爆窄卡槽
    unsigned long whole = (unsigned long)v;
    unsigned cents = (unsigned)((v - (double)whole) * 100.0 + 0.5);
    if (cents >= 100) { whole += 1; cents -= 100; }

    char w[24];
    int n = 0;
    do { w[n++] = (char)('0' + (int)(whole % 10)); whole /= 10; } while (whole && n < (int)sizeof(w));
    size_t o = 0;
    while (n > 0 && o + 1 < cap) out[o++] = w[--n];
    if (o + 1 < cap) out[o++] = '.';
    if (o + 1 < cap) out[o++] = (char)('0' + (cents / 10) % 10);
    if (o + 1 < cap) out[o++] = (char)('0' + cents % 10);
    out[o] = '\0';
}

// OpenRouter: {"data":{"total_credits":100.5,"total_usage":25.75}}
// 没有现成的"余额"字段,剩余 = 已购 - 已用(单位:美元)。
// 注意:该接口要求 Management Key,普通推理 key(sk-or-v1-...)会得到 403。
static bool parse_openrouter(const char *body, char *amount, size_t cap)
{
    char credits[24], usage[24];
    if (!json_field(body, "total_credits", credits, sizeof(credits))) return false;
    if (!json_field(body, "total_usage", usage, sizeof(usage))) return false;
    double v = strtod(credits, NULL) - strtod(usage, NULL);
    fmt_amount(v, amount, cap);
    return amount[0] != '\0';
}

// ---------------------------------------------------------------- 适配器表
// host 必须真是 url 里的主机名(网络自检单独解析它用)。
// 注:目前余额页/记账结构只有 3 行卡槽,表里最多放 3 个平台;加第 4 个前
// 要先把 HIST_ROWS/WE_DIAG_ROWS/余额页布局一起扩到 4(动 NVS 记账 blob,需迁移)。
static const we_provider_t PROVIDERS[] = {
    { "deepseek",   "DeepSeek",        "https://api.deepseek.com/user/balance",       "api.deepseek.com", "CNY" },
    { "kimi",       "Kimi",            "https://api.moonshot.cn/v1/users/me/balance", "api.moonshot.cn",  "CNY" },
    { "openrouter", "OpenRouter",      "https://openrouter.ai/api/v1/credits",        "openrouter.ai",    "USD" },
};

bool we_provider_lookup(const char *id, const we_provider_t **out)
{
    if (!id || !out) return false;
    for (size_t i = 0; i < sizeof(PROVIDERS) / sizeof(PROVIDERS[0]); i++) {
        if (strcmp(PROVIDERS[i].id, id) == 0) {
            *out = &PROVIDERS[i];
            return true;
        }
    }
    return false;
}

bool we_provider_parse(const char *id, const char *body, char *amount, size_t cap)
{
    if (!id || !body || !amount || cap == 0) return false;
    if (strcmp(id, "deepseek") == 0) return parse_deepseek(body, amount, cap);
    if (strcmp(id, "kimi") == 0) return parse_kimi(body, amount, cap);
    if (strcmp(id, "openrouter") == 0) return parse_openrouter(body, amount, cap);
    return false;
}

// ---------------------------------------------------------------- 汇率解析
// frankfurter.dev: {"amount":1.0,"base":"USD","date":"...","rates":{"CNY":6.7046}}
// er-api.com     : {"result":"success","rates":{"USD":1,...,"CNY":6.7143},...}
// 两家的公共形态都是 "rates" 对象里的 "CNY":数字,直接复用 json_field。
// 数值合理区间 4.0~12.0:明显出界的(比如误抓到 time_last_update 的秒级时间戳)
// 一律拒绝,宁可回退缓存/兜底值,也不要把 6 位数当汇率乘上去。
bool we_rate_parse(const char *body, double *out)
{
    if (!body || !out) return false;
    char num[24];
    if (!json_field(body, "CNY", num, sizeof(num))) return false;
    char *end = NULL;
    double v = strtod(num, &end);
    if (end == num || *end != '\0') return false;      // 整段必须是纯数字
    if (v < 4.0 || v > 12.0) return false;
    *out = v;
    return true;
}

double we_provider_to_cny(const char *currency, double amount, double usd_cny_rate)
{
    if (!currency) return amount;
    if (strcmp(currency, "USD") == 0) {
        if (usd_cny_rate > 0.0) return amount * usd_cny_rate;
        return amount;            // 没拿到汇率:宁可标价不折,也不瞎乘
    }
    return amount;                // CNY 与未知币种一律原样
}
