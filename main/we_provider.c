// main/we_provider.c — 平台适配器实现(纯逻辑,零 ESP-IDF/LVGL 依赖)
#include "we_provider.h"

#include <ctype.h>
#include <stdio.h>
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

// ---------------------------------------------------------------- 适配器表
static const we_provider_t PROVIDERS[] = {
    { "deepseek", "DeepSeek", "https://api.deepseek.com/user/balance" },
    { "kimi", "Kimi", "https://api.moonshot.cn/v1/users/me/balance" },
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
    return false;
}
