// main/we_provider.h — 平台适配器(纯逻辑,可 host 测试)
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct we_provider {
    const char *id;            // "deepseek"
    const char *display_name;  // "DeepSeek"
    const char *url;           // 官方余额接口
} we_provider_t;

// 查找支持表(不支持返回 false)
bool we_provider_lookup(const char *id, const we_provider_t **out);

// 从响应体解析余额金额到 amount(纯字符串/轻量 JSON 提取,零依赖)
bool we_provider_parse(const char *id, const char *body, char *amount, size_t cap);

#ifdef __cplusplus
}
#endif
