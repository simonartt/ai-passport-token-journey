// main/demo_portal.c —— 配置后台入口页(菜单里选 Portal 进入)。
// 设备优先接入家庭局域网,手机连同一个 WiFi 就能访问;连不上才回落设备热点。
#include "demo.h"
#include "we_portal.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include <stdio.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_state;    // 模式行
static lv_obj_t *s_url;      // 访问地址
static lv_obj_t *s_hint;     // 账号 / 操作提示
static lv_timer_t *s_timer;
static bool s_closed;        // 门户已停(启动失败或空闲超时)

static void portal_tick(lv_timer_t *t)
{
    (void)t;
    if (s_closed) return;

    we_portal_mode_t mode = we_portal_mode();

    if (mode == WE_PORTAL_OFF) {
        s_closed = true;
        lv_label_set_text(s_state, "PORTAL OFF");
        lv_label_set_text(s_url, "");
        lv_label_set_text(s_hint, "press OK to retry");
        return;
    }
    if (mode == WE_PORTAL_CONNECTING) {
        lv_label_set_text(s_state, "CONNECTING...");
        lv_label_set_text(s_url, "");
        lv_label_set_text(s_hint, "trying saved WiFi");
        return;
    }

    uint32_t idle = we_portal_idle_seconds();
    if (idle >= WE_PORTAL_IDLE_LIMIT_S) {
        we_portal_stop();            // 长时间没人用,关 Wi-Fi 省电(STA 常连约 80-100mA)
        s_closed = true;
        lv_label_set_text(s_state, "IDLE CLOSED");
        lv_label_set_text(s_url, "");
        lv_label_set_text(s_hint, "auto closed to save power\npress OK to reopen");
        return;
    }

    unsigned left = (unsigned)(WE_PORTAL_IDLE_LIMIT_S - idle);

    if (mode == WE_PORTAL_STA) {
        lv_label_set_text(s_state, "LAN MODE");
        lv_label_set_text(s_url, we_portal_url());
        const char *hn = we_portal_hostname();
        if (hn && hn[0])
            lv_label_set_text_fmt(s_hint, "%s.local too\nadmin / %s\ncloses in %us",
                                  hn, we_portal_password(), left);
        else
            lv_label_set_text_fmt(s_hint, "admin / %s\ncloses in %us",
                                  we_portal_password(), left);
    } else {
        lv_label_set_text(s_state, "HOTSPOT MODE");
        lv_label_set_text(s_url, we_portal_url());
        lv_label_set_text_fmt(s_hint, "join %s / %s\nthen open the URL\ncloses in %us",
                              we_portal_ssid(), we_portal_password(), left);
    }
}

void demo_portal_enter(void)
{
    s_closed = false;
    we_portal_start();                 // 立即返回,联网在后台任务里做

    s_scr = ui_pixel_screen_create("PORTAL");

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 58, 216, 176, UI_PAPER);

    s_state = lv_label_create(panel);
    lv_obj_set_width(s_state, 192);
    lv_obj_set_style_text_font(s_state, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_state, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_state, LV_ALIGN_TOP_LEFT, 6, 8);

    s_url = lv_label_create(panel);
    lv_obj_set_width(s_url, 192);
    lv_obj_set_style_text_font(s_url, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_url, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_url, LV_ALIGN_TOP_LEFT, 6, 34);

    s_hint = lv_label_create(panel);
    lv_obj_set_width(s_hint, 192);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_LEFT, 6, 70);

    ui_pixel_mascot_create(s_scr, 101, 246);
    lv_screen_load(s_scr);

    portal_tick(NULL);                 // 先画一帧,避免出现空白面板
    s_timer = lv_timer_create(portal_tick, 1000, NULL);
}

void demo_portal_exit(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    we_portal_stop();
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_state = NULL;
    s_url = NULL;
    s_hint = NULL;
}

void demo_portal_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn != BSP_BTN_OK || ev != BSP_BTN_CLICK) return;
    if (!s_closed) return;             // 运行中不响应,避免误重启
    s_closed = false;
    we_portal_start();
    portal_tick(NULL);
}
