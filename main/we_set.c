// main/we_set.c — 用户可选项纯逻辑实现
#include "we_set.h"

bool we_set_auto_min_valid(uint8_t m)
{
    switch (m) {
    case WE_SET_AUTO_OFF:
    case WE_SET_AUTO_30:
    case WE_SET_AUTO_60:
    case WE_SET_AUTO_120:
    case WE_SET_AUTO_180:
        return true;
    default:
        return false;
    }
}

uint32_t we_set_auto_min_ms(uint8_t m)
{
    if (!we_set_auto_min_valid(m)) return 0;
    return (uint32_t)m * 60u * 1000u;
}
