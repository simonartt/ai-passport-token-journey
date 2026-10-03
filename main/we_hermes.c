// main/we_hermes.c — Hermes 响应解析实现(纯逻辑)
//
// host 兼容约定:不用 strlcpy(Windows CRT 没有)、不用 malloc(解析路径零堆分配)。
#include "we_hermes.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cpy_n(char *dst, size_t cap, const char *src, size_t n)
{
    size_t o = 0;
    while (o + 1 < cap && o < n && src[o]) { dst[o] = src[o]; o++; }
    dst[o] = '\0';
}

// ---------------------------------------------------------------- 提取原语
// "key":"value" → 拷贝 value。找到返回 true。
static bool json_str(const char *body, const char *key, char *out, size_t cap)
{
    char pat[64];
    if (snprintf(pat, sizeof(pat), "\"%s\":\"", key) >= (int)sizeof(pat)) return false;
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) p++;                 // 跳转义首字符
        out[o++] = *p++;
    }
    out[o] = '\0';
    return true;
}

// 从 from 起累加所有 "key":N(限 len 窗口内)
static uint64_t sum_key(const char *from, size_t len, const char *key)
{
    char pat[64];
    if (snprintf(pat, sizeof(pat), "\"%s\":", key) >= (int)sizeof(pat)) return 0;
    uint64_t total = 0;
    const char *p = from, *lim = from + len;
    while (p < lim) {
        const char *f = strstr(p, pat);
        if (!f || f >= lim) break;
        f += strlen(pat);
        while (*f && (unsigned char)*f <= ' ') f++;
        if (isdigit((unsigned char)*f)) {
            char *end = NULL;
            total += (uint64_t)strtoull(f, &end, 10);
            p = end;
        } else p = f;
    }
    return total;
}

static bool json_u64_at(const char *seg, size_t len, const char *key, uint64_t *out)
{
    char pat[64];
    if (snprintf(pat, sizeof(pat), "\"%s\":", key) >= (int)sizeof(pat)) return false;
    const char *f = strstr(seg, pat);
    if (!f || f >= seg + len) return false;
    f += strlen(pat);
    while (*f && (unsigned char)*f <= ' ') f++;
    if (!isdigit((unsigned char)*f)) return false;
    char *end = NULL;
    *out = (uint64_t)strtoull(f, &end, 10);
    return end != f;
}

// 段内找 "key":"val"(段不保证 NUL 结尾,手工扫)
static bool json_str_seg(const char *seg, size_t len, const char *key, char *out, size_t cap)
{
    char pat[64];
    if (snprintf(pat, sizeof(pat), "\"%s\":\"", key) >= (int)sizeof(pat)) return false;
    size_t pl = strlen(pat);
    for (size_t i = 0; i + pl <= len; i++) {
        if (memcmp(seg + i, pat, pl) != 0) continue;
        size_t j = i + pl, o = 0;
        while (j < len && seg[j] != '"' && o + 1 < cap) {
            if (seg[j] == '\\' && j + 1 < len) j++;
            out[o++] = seg[j++];
        }
        out[o] = '\0';
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- usage
bool we_hm_parse_usage(const char *body, we_hm_usage_t *out)
{
    if (!body || !out) return false;
    memset(out, 0, sizeof(*out));
    const char *d = strstr(body, "\"daily\"");
    if (!d) return false;
    size_t len = strlen(d);
    out->tokens = sum_key(d, len, "input_tokens")
                + sum_key(d, len, "output_tokens")
                + sum_key(d, len, "reasoning_tokens");
    out->calls = sum_key(d, len, "api_calls");
    // sessions 取最大值而非累加:展示"峰值并发会话"才有意义
    uint64_t peak = 0;
    {
        const char *p = d;
        while ((p = strstr(p, "\"sessions\":")) != NULL) {
            p += 11;
            while (*p && (unsigned char)*p <= ' ') p++;
            if (isdigit((unsigned char)*p)) {
                char *end = NULL;
                uint64_t v = (uint64_t)strtoull(p, &end, 10);
                if (v > peak) peak = v;
                p = end;
            }
        }
    }
    out->sessions_peak = peak;
    {
        const char *p = d;
        while ((p = strstr(p, "\"day\":")) != NULL) { out->days++; p += 6; }
    }
    out->ok = true;
    return true;
}

// ---------------------------------------------------------------- models
// 逐对象切片:每个 "model":"..." 到下一个 "model":"(或串尾)之间找本对象的
// provider/tokens/calls,避免跨对象串键。capabilities 等长尾字段在切片后段,
// 不影响前面的键提取。
bool we_hm_parse_models(const char *body, we_hm_models_t *out)
{
    if (!body || !out) return false;
    memset(out, 0, sizeof(*out));
    const char *m = strstr(body, "\"models\"");
    if (!m) return false;
    static const char pat[] = "\"model\":\"";
    const char *p = m;
    while (out->count < WE_HM_MAX_MODELS) {
        const char *f = strstr(p, pat);
        if (!f) break;
        f += strlen(pat);
        const char *nxt = strstr(f, pat);
        size_t seg = nxt ? (size_t)(nxt - f) : strlen(f);
        we_hm_model_t *it = &out->item[out->count];
        // name 就在 f 处
        {
            size_t o = 0;
            while (o < seg && f[o] != '"' && o + 1 < WE_HM_NAME_MAX) {
                if (f[o] == '\\' && o + 1 < seg) o++;
                it->name[o] = f[o]; o++;
            }
            it->name[o] = '\0';
        }
        char tmp[48];
        if (json_str_seg(f, seg, "provider", tmp, sizeof(tmp)))
            cpy_n(it->provider, sizeof(it->provider), tmp, strlen(tmp));
        it->tokens = sum_key(f, seg, "input_tokens")
                   + sum_key(f, seg, "output_tokens")
                   + sum_key(f, seg, "reasoning_tokens");
        uint64_t v = 0;
        if (json_u64_at(f, seg, "api_calls", &v)) it->calls = v;
        out->count++;
        if (!nxt) break;
        p = nxt;
    }
    out->ok = out->count > 0;
    return out->ok;
}

void we_hm_models_sort(we_hm_models_t *m)
{
    if (!m) return;
    for (int i = 1; i < m->count; i++) {              // 插入排序,≤48 条
        we_hm_model_t key = m->item[i];
        int j = i - 1;
        while (j >= 0 && m->item[j].tokens < key.tokens) { m->item[j + 1] = m->item[j]; j--; }
        m->item[j + 1] = key;
    }
}

// ---------------------------------------------------------------- status
// 平台条目形态: "feishu":{"state":"connected",...}
// 扫描 '"NAME":{"state":"' 组合;NAME 限字母数字下划线(避开 components 等嵌套)。
bool we_hm_parse_status(const char *body, we_hm_status_t *out)
{
    if (!body || !out) return false;
    memset(out, 0, sizeof(*out));
    char tmp[WE_HM_NAME_MAX];
    if (json_str(body, "overall", tmp, sizeof(tmp)))
        cpy_n(out->overall, sizeof(out->overall), tmp, strlen(tmp));
    const char *gp = strstr(body, "\"gateway_platforms\"");
    if (!gp) { out->ok = out->overall[0] != '\0'; return out->ok; }
    const char *p = gp + 19;
    while (out->plat_count < WE_HM_PLAT_MAX) {
        // 找下一个 `":{"state":"`
        const char *q = p;
        for (;;) {
            q = strstr(q, "\":{\"state\":\"");
            if (!q) { q = NULL; break; }
            // 回溯取平台名: 从 q 往前找 '"' 起点
            const char *s = q - 1;
            while (s > gp && *s != '"') s--;
            size_t nl = (size_t)(q - s - 1);
            if (nl > 0 && nl < WE_HM_NAME_MAX) {
                char pname[WE_HM_NAME_MAX];
                memcpy(pname, s + 1, nl); pname[nl] = '\0';
                const char *e0 = strchr(q + 12, '"');
                if (e0) {
                    cpy_n(out->plat[out->plat_count], WE_HM_NAME_MAX, pname, nl);
                    cpy_n(out->state[out->plat_count], WE_HM_STATE_MAX, q + 12, (size_t)(e0 - (q + 12)));
                    out->plat_count++;
                    p = e0;
                    break;
                }
            }
            q = s + 1;
        }
        if (!q) break;
    }
    out->ok = out->overall[0] != '\0' || out->plat_count > 0;
    return out->ok;
}

// ---------------------------------------------------------------- 登录
bool we_hm_parse_login(const char *body)
{
    if (!body) return false;
    const char *p = strstr(body, "\"ok\"");
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p && (unsigned char)*p <= ' ') p++;
    return p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e';
}
