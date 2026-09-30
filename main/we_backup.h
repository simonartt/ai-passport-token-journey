// main/we_backup.h — 配置与历史记录的 JSON 备份/恢复(供局域网门户调用)。
//
// 备份文件同时含 we_cfg 配置(nickname / WiFi 档案 / 平台 API Key)与本地逐日
// 记账历史(balhist),因此换设备或重刷固件后可以整份刷回,记录不丢。
#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_BACKUP_FORMAT  "token-journey-backup"
#define WE_BACKUP_VERSION 1

// 导出:把配置与历史编码成 JSON 文本。
// 成功返回 malloc 的字符串(调用方负责 free),失败返回 NULL。
char *we_backup_export(void);

// 恢复:解析 JSON 并把配置 + 历史一起写进 NVS。
// err_msg 会写入人类可读的结果/失败原因(可为 NULL)。
// 成功后调用方必须重启设备:app_balance 已在内存里缓存了配置与历史。
esp_err_t we_backup_restore(const char *json, char *err_msg, size_t err_msg_len);

#ifdef __cplusplus
}
#endif
