// tests/test_we_set.c — 用户可选项纯逻辑 host 测试
#include <assert.h>
#include <stdio.h>

#include "we_set.h"

static void test_valid(void)
{
    assert(we_set_auto_min_valid(WE_SET_AUTO_OFF));
    assert(we_set_auto_min_valid(WE_SET_AUTO_30));
    assert(we_set_auto_min_valid(WE_SET_AUTO_60));
    assert(we_set_auto_min_valid(WE_SET_AUTO_120));
    assert(we_set_auto_min_valid(WE_SET_AUTO_180));
    // 离散档位之外一律拒绝(防脏 NVS / 表单篡改)
    assert(!we_set_auto_min_valid(1));
    assert(!we_set_auto_min_valid(15));
    assert(!we_set_auto_min_valid(45));
    assert(!we_set_auto_min_valid(240));
    assert(!we_set_auto_min_valid(255));
    printf("ok: auto_min valid\n");
}

static void test_ms(void)
{
    assert(we_set_auto_min_ms(WE_SET_AUTO_OFF) == 0);
    assert(we_set_auto_min_ms(WE_SET_AUTO_30) == 30u * 60u * 1000u);
    assert(we_set_auto_min_ms(WE_SET_AUTO_60) == 60u * 60u * 1000u);
    assert(we_set_auto_min_ms(WE_SET_AUTO_120) == 120u * 60u * 1000u);
    assert(we_set_auto_min_ms(WE_SET_AUTO_180) == 180u * 60u * 1000u);
    // 非法 → 0(等同关闭),绝不返回一个随机的"半个档"周期
    assert(we_set_auto_min_ms(45) == 0);
    assert(we_set_auto_min_ms(255) == 0);
    printf("ok: auto_min ms\n");
}

int main(void)
{
    test_valid();
    test_ms();
    printf("ALL PASS\n");
    return 0;
}
