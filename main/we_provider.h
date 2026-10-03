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
    const char *host;          // url 里的主机名(取数失败时网络探测要用它单独解析)
    const char *currency;      // "CNY" / "USD"(USD 平台取回金额会先折成人民币再记账)
} we_provider_t;

// 查找支持表(不支持返回 false)
bool we_provider_lookup(const char *id, const we_provider_t **out);

// 从响应体解析余额金额到 amount(纯字符串/轻量 JSON 提取,零依赖)
bool we_provider_parse(const char *id, const char *body, char *amount, size_t cap);

// 从汇率接口响应体提取人民币牌价。兼容两种常见格式(frankfurter.dev 与
// open.er-api.com 都是 "rates":{"CNY":6.7046,...} 这一段)。
// 解析成功且数值落在合理区间(4.0~12.0,防抓错字段)返回 true。
bool we_rate_parse(const char *body, double *out);

// 把一个平台的余额折成人民币。currency="CNY" 原样返回;"USD" 乘 usd_cny_rate;
// 其它/空 currency 保守按原值返回(绝不误乘)。rate<=0 时也原样返回(没汇率就不折)。
// 纯函数,可 host 测。
double we_provider_to_cny(const char *currency, double amount, double usd_cny_rate);

#ifdef __cplusplus
}
#endif
