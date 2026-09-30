// main/fap_screenshot.h — FoloToy 发布用串口截屏协议(FAP_SCREENSHOT_V1)
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动协议服务(读串口行命令;观测专用,不改设备不重启)
esp_err_t fap_screenshot_start(void);

#ifdef __cplusplus
}
#endif
