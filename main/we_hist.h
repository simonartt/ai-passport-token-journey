// main/we_hist.h — 本地逐日消耗记账的数据模型与 NVS 存取(纯数据结构,零依赖)。
//
// 单独抽成头文件,是为了让门户侧的备份/恢复模块(we_backup)能直接读写这份
// NVS blob,而不必把 app_balance.c 的界面状态暴露出去。记账逻辑仍在 app_balance.c。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HIST_DAYS   31              // 环形槽数(31 槽差出 30 天消耗)
#define HIST_ROWS   3               // 记账行数(与余额页 ROW_COUNT 同源)
#define HIST_MAGIC  0x48495332u     // 'HIS2'(v2 含 row_id 行语义)
#define HIST_NVS_NS "balhist"
#define HIST_NVS_KEY "log"

// 每天首次成功取数把各平台余额记为“当日基线”;某日消耗 ≈ 当日基线 − 次日基线
// (次日首快照 ≈ 当日末尾余额)。31 槽环形覆盖 30 天差,槽位 = day % HIST_DAYS。
typedef struct {
    uint32_t magic;
    char     row_id[HIST_ROWS][16];          // 记账时的平台行语义(provider_id 顺序)
    uint16_t day[HIST_DAYS];                 // 槽对应 UTC+8 epoch 日;0=空
    float    bal[HIST_DAYS][HIST_ROWS];      // 该日基线(未配置行恒 0)
} hist_t;

// 直接读写 NVS 中的历史 blob(实现见 app_balance.c,与主记账逻辑同源)。
// 读:magic 不符或未写过 → 清零并返回 false。
// 写:要求调用方已把 magic 置为 HIST_MAGIC,否则拒绝。
// 恢复备份后必须重启,以免命中原任务已加载的内存副本。
bool app_hist_nvs_read(hist_t *out);
bool app_hist_nvs_write(const hist_t *in);

#ifdef __cplusplus
}
#endif
