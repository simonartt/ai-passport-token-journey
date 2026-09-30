// main/fap_screenshot.c — FAP_SCREENSHOT_V1 串口截屏服务(收割版)。
//
// 上一版用 lv_snapshot_take 在独立 task 渲染整帧(240x320 RGB565 ≈ 150KB),
// 本板无 PSRAM、内部 RAM 总共约 150KB,反复 Stack protection / 内存不足崩溃,已回退。
//
// 本版不再自行渲染:钩在 esp_lvgl_port 的 flush 回调(swap 之前,像素为原生小端
// RGB565),把 LVGL 真实渲染出来的像素带逐带转发到 USB Serial/JTAG。
//   · 收到 "FAP_SCREENSHOT_V1\n" → 武装 + 全屏 invalidate;
//   · LVGL 刷新从 (0,0) 起逐带调用 flush → 首带前发协议 header,之后逐带转发;
//   · 累计满屏高即完成。零额外大缓冲(带宽即 LVGL draw buffer,~9.6KB)。
// 观测专用:不重启、不写设备、不改配置。
//
// 注意:协议 payload 之后主机按字节数读取,期间任何 ESP_LOG 输出都会污染
// payload → 捕获期间把全局日志级别压到 NONE,收完恢复。

#include "fap_screenshot.h"

#include "bsp_display.h"
#include "driver/usb_serial_jtag.h"
#include "esp_lvgl_port_disp.h"
#include "esp_log.h"
#include "hal/usb_serial_jtag_ll.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "fap_shot";

typedef enum {
    CAP_IDLE = 0,   // 空闲
    CAP_ARMED,      // 已收到命令,等待整屏刷新从 (0,0) 开始的第一个 flush
    CAP_STREAM,     // 正在逐带转发像素
} cap_state_t;

static volatile cap_state_t s_state = CAP_IDLE;
static volatile int          s_rows = 0;   // 已转发行数

// 回到空闲并恢复全局日志级别。任何退出路径都必须走这里。
static void cap_finish(void)
{
    s_state = CAP_IDLE;
    s_rows = 0;
    esp_log_level_set("*", ESP_LOG_INFO);
}

// 阻塞写串口(TX 寄存器轮询直写,console 同款路径;不依赖驱动 TX 状态机)。
// 全部写完返回 true;超时/异常返回 false。
static bool write_all(const uint8_t *data, size_t len)
{
    size_t off = 0;
    TickType_t start = xTaskGetTickCount();
    while (off < len) {
        if (usb_serial_jtag_ll_txfifo_writable()) {
            size_t n = (len - off > 64) ? 64 : (len - off);
            int w = usb_serial_jtag_ll_write_txfifo(data + off, n);
            if (w > 0) {
                off += (size_t)w;
                usb_serial_jtag_ll_txfifo_flush();
                continue;
            }
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(20000)) {
            ESP_LOGE(TAG, "usb tx timeout off=%u/%u", (unsigned)off, (unsigned)len);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

// 由 LVGL flush 回调调用(LVGL 渲染任务上下文,color_map 为 swap 前的原生小端 RGB565)。
static void cap_flush_hook(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map,
                           void *user_ctx)
{
    (void)user_ctx;
    if (s_state == CAP_IDLE) return;

    if (s_state == CAP_ARMED) {
        // 丢弃非"整屏起点"的增量刷新;全屏 invalidate 后首个 flush 必从 (0,0) 起。
        if (area->x1 != 0 || area->y1 != 0) return;
        if (lv_area_get_width(area) != lv_display_get_horizontal_resolution(drv)) return;

        int w = lv_display_get_horizontal_resolution(drv);
        int h = lv_display_get_vertical_resolution(drv);
        char hdr[96];
        int hl = snprintf(hdr, sizeof(hdr), "FAP_SCREENSHOT_V1 %d %d RGB565LE %d\n",
                          w, h, w * h * 2);
        if (hl <= 0 || !write_all((const uint8_t *)hdr, (size_t)hl)) {
            cap_finish();
            return;
        }
        s_state = CAP_STREAM;
        s_rows = 0;
    }

    // CAP_STREAM:转发本带像素(小端 RGB565,与协议 RGB565LE 一致)。
    if (!write_all(color_map, (size_t)lv_area_get_size(area) * 2)) {
        cap_finish();   // 发送中断(如 USB 拔出),放弃本次捕获
        return;
    }
    s_rows += lv_area_get_height(area);
    if (s_rows >= lv_display_get_vertical_resolution(drv)) {
        int rows = (int)s_rows;
        cap_finish();   // 满屏收齐,完成
        ESP_LOGI(TAG, "capture done (%d rows)", rows);
    }
}

static void trigger_capture(void)
{
    if (s_state != CAP_IDLE) return;          // 上一次捕获尚未结束
    ESP_LOGI(TAG, "capture requested");       // payload 前的日志,主机 readline 可容忍
    if (!bsp_lvgl_lock(2000)) return;
    // payload 期间主机按字节数读取,任何日志都会污染 → 先全局静默
    esp_log_level_set("*", ESP_LOG_NONE);
    s_state = CAP_ARMED;
    lv_obj_invalidate(lv_screen_active());    // 强制下一轮整屏重绘
    bsp_lvgl_unlock();
}

static void reader_task(void *arg)
{
    (void)arg;
    // 直接读 USB Serial/JTAG 驱动(不经 VFS stdin;行拼接匹配协议命令)。
    uint8_t raw[64];
    char line[96];
    size_t llen = 0;
    for (;;) {
        int n = usb_serial_jtag_read_bytes(raw, sizeof(raw), pdMS_TO_TICKS(200));
        if (n <= 0) continue;
        for (int i = 0; i < n; i++) {
            char c = (char)raw[i];
            if (c == '\n' || c == '\r') {
                if (llen > 0) {
                    line[llen] = '\0';
                    if (strstr(line, "FAP_SCREENSHOT_V1")) {
                        trigger_capture();
                    }
                    llen = 0;
                }
            } else if (llen < sizeof(line) - 1) {
                line[llen++] = c;
            }
        }
    }
}

esp_err_t fap_screenshot_start(void)
{
    lv_display_t *disp = bsp_lvgl_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "LVGL display 未就绪,无法注册截屏钩子");
        return ESP_FAIL;
    }
    lvgl_port_display_set_snapshot_cb(disp, cap_flush_hook, NULL);

    // console 默认走"寄存器模式"VFS(不 install 驱动),直接调驱动读会 NULL 崩溃;
    // 此处确保 USB Serial/JTAG 驱动已 install(install 后 RX 由驱动 ISR 收进 ringbuffer)。
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "USB Serial/JTAG 驱动安装失败,截屏服务不可用");
            return ESP_FAIL;
        }
    }

    BaseType_t ok = xTaskCreate(reader_task, "fap_shot", 6144, NULL, 4, NULL);
    if (ok != pdPASS) return ESP_FAIL;
    ESP_LOGI(TAG, "FAP_SCREENSHOT service ready");
    return ESP_OK;
}
