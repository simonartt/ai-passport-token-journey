// main/we_set.h — 用户可选项的纯逻辑(零 ESP-IDF/LVGL 依赖,可 host 测试)
//
// 目前只有一项:自动刷新间隔(auto_min,单位分钟)。这里只放"档位→毫秒"的换算
// 与合法性判断;NVS 读写按本仓库惯例留在 app_balance.c / we_portal.c。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 允许的自动刷新档位(分钟)。0 = 关闭定时刷新(仍可手动 OK、离线时自动重试)。
#define WE_SET_AUTO_OFF      0
#define WE_SET_AUTO_30       30
#define WE_SET_AUTO_60       60
#define WE_SET_AUTO_120      120
#define WE_SET_AUTO_180      180

// 档位是否合法(只接受上面几个离散值)。
bool we_set_auto_min_valid(uint8_t m);

// 分钟 → 毫秒。0/非法 → 0(表示关闭)。uint32 上限足够:180min=1.08e7 ms。
uint32_t we_set_auto_min_ms(uint8_t m);

#ifdef __cplusplus
}
#endif
