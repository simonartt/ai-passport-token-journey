// main/we_hermes.h — Hermes 网关响应解析(纯逻辑,零 ESP-IDF 依赖,可 host 测试)
//
// 数据面(2026-10-03 在用户网关 v0.21.5 实测):
//   POST /auth/password-login {"provider":"basic","username","password"}
//        → Set-Cookie: hermes_session_at=...(12h);api/* 只认这个 Cookie,不认 Basic 头
//   GET  /api/status                    (免认证)  overall + 平台连接状态
//   GET  /api/analytics/usage?days=30   (需Cookie) daily[]{tokens,api_calls,sessions,...}
//   GET  /api/analytics/models?days=30  (需Cookie) models[]{model,provider,tokens,calls}
//
// usage/models 响应实测 15~18KB,远超余额用的 4KB 缓冲。这里按"整读进大缓冲 +
// strstr 提取"设计:Hermes 走局域网明文 HTTP,没有 TLS 握手内存峰值,20KB static
// 缓冲在 C3 上安全(失败时调用方跳过本轮即可)。
//
// 注意口径:这是"网关侧 agent 用量"(含本地 lmstudio 模型,cost 常为 0),
// 与三家 API 余额不是一个币种口径,UI 上单独成页,绝不并入 CNY 合计。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_HM_NAME_MAX    40    // 模型名(超长截断)
#define WE_HM_PROVIDER_MAX 16
#define WE_HM_MAX_MODELS  48    // 实测 42 个,留余量
#define WE_HM_PLAT_MAX    6     // feishu/telegram/weixin/discord/...
#define WE_HM_STATE_MAX   16

// ---------------------------------------------------------------- usage
typedef struct {
    bool     ok;
    uint64_t tokens;          // Σ input+output+reasoning(不含 cache_read)
    uint64_t calls;           // Σ api_calls
    uint64_t sessions_peak;   // max(sessions)(当日峰值,非累计)
    int      days;            // daily 条目数
} we_hm_usage_t;

// 解析 {"daily":[{...,"input_tokens":N,"output_tokens":N,"reasoning_tokens":N,
//                "sessions":N,"api_calls":N,...},...]}
// 对每个键的所有出现分别累加;数值必须是 JSON 整数。找不到 daily 返回 ok=false。
bool we_hm_parse_usage(const char *body, we_hm_usage_t *out);

// ---------------------------------------------------------------- models
typedef struct {
    char     name[WE_HM_NAME_MAX];
    char     provider[WE_HM_PROVIDER_MAX];
    uint64_t tokens;          // input+output+reasoning
    uint64_t calls;
} we_hm_model_t;

typedef struct {
    bool          ok;
    int           count;      // 解析出的条目数(≤WE_HM_MAX_MODELS)
    we_hm_model_t item[WE_HM_MAX_MODELS];
} we_hm_models_t;

// 解析 {"models":[{...},...]}。超出上限的条目丢弃(仍计入 total)。
bool we_hm_parse_models(const char *body, we_hm_models_t *out);

// 按 tokens 降序冒排(条目 ≤48,插入排序足够),供 TOP6 展示
void we_hm_models_sort(we_hm_models_t *m);

// ---------------------------------------------------------------- status
typedef struct {
    bool ok;
    char overall[WE_HM_STATE_MAX];                       // ok / degraded / ...
    char plat[WE_HM_PLAT_MAX][WE_HM_NAME_MAX];           // 平台名(响应出现序)
    char state[WE_HM_PLAT_MAX][WE_HM_STATE_MAX];         // connected / retrying / ...
    int  plat_count;
} we_hm_status_t;

// 解析 {"overall":"...","gateway_platforms":{"feishu":{"state":"connected",...},...}}
bool we_hm_parse_status(const char *body, we_hm_status_t *out);

// ---------------------------------------------------------------- 登录响应
// {"ok":true,"next":"/"} → true。解析失败/{"ok":false} → false。
bool we_hm_parse_login(const char *body);

#ifdef __cplusplus
}
#endif
