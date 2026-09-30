// main/we_portal.h — 本地配置门户(优先连家庭 WiFi 走局域网,失败回落 SoftAP)。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 空闲多久(秒)后建议关闭门户省电。STA 常连约 80-100mA,不适合长时间挂着。
#define WE_PORTAL_IDLE_LIMIT_S 300

// 门户当前所处模式。
typedef enum {
    WE_PORTAL_OFF = 0,     // 未启动
    WE_PORTAL_CONNECTING,  // 后台正在连家庭 WiFi
    WE_PORTAL_STA,         // 已接入局域网,用 we_portal_url() 访问
    WE_PORTAL_AP,          // 回落为设备热点
} we_portal_mode_t;

// 启动/停止门户。start 立即返回,STA 连接在后台进行,用 we_portal_mode() 轮询结果。
esp_err_t we_portal_start(void);
void we_portal_stop(void);
bool we_portal_running(void);

we_portal_mode_t we_portal_mode(void);
// 访问地址:STA 为 "http://<局域网IP>/",AP 为 "http://192.168.4.1/";未就绪时空串。
const char *we_portal_url(void);
// 局域网 IP(不含协议),未就绪时空串。
const char *we_portal_ip(void);
// mDNS 短名(如 "token-journey"),未启用空串;STA 下可用 http://<hostname>.local/ 访问。
const char *we_portal_hostname(void);
// AP 模式的热点名;非 AP 模式为空串。
const char *we_portal_ssid(void);
// 访问密码(HTTP Basic Auth,用户名 admin)。AP 热点密码同值。
const char *we_portal_password(void);
// 距最近一次 HTTP 请求的秒数;门户未运行时返回 0。
uint32_t we_portal_idle_seconds(void);

// we_cfg 的 NVS 存取(设备侧胶水;读/写 cfg/main blob,含 magic 校验)
#include "we_cfg.h"
bool we_cfg_save(const we_cfg_t *cfg);
bool we_cfg_load(we_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
