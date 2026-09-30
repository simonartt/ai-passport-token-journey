// main/app_balance.c —— AI Passport Token 看板主界面玩法(深色 AI 风 UI)。
//
// 双页结构(DOWN 键水平滑动切换):
//   第 1 页 TOKEN JOURNEY —— 最近 30 天消耗热力图(紫/青色阶)
//   第 2 页 TOKEN BALANCE —— 各平台余额 + 今日花费
//
// 数据源:设备 HTTPS 直连各平台官方余额接口(we_provider 适配层),
//   API Key 由用户在配网门户中填写,仅存设备 NVS(we_cfg)。
//
// 交互:
//   开机       直接进入本看板(由 main.c 引导),默认第 1 页 TOKEN JOURNEY
//   DOWN 短按  切换 主页 ↔ 余额页
//   OK 短按    立即刷新(联网取数,约 2~8s,取完关 Wi-Fi;两页同源一次取回)
//   UP 短按    锁屏;锁屏态仅 UP 可解锁(DOWN/OK 锁屏时忽略)
//   OK 长按    返回 demo 菜单(main.c 统一拦截)
//
// 电源策略:Wi-Fi 仅在刷新期间打开,取完即停即释放;屏幕静态画面不重建。
// 内存:C3 无 PSRAM;全部小缓冲;LVGL 操作只在本任务/持锁上下文进行。
//
// 视觉(2026-09-04 v2 重构):深空底色 + 大号总余额 + 三张平台卡右侧余额 +
// 底部正中弱化提示。标题/金额无 ¥ 字形(montserrat 内置无 U+00A5),单位用 CNY 标注。

#include "demo.h"
#include "demo_radio.h"
#include "bsp_battery.h"
#include "bsp_display.h"   // bsp_lvgl_lock / bsp_lvgl_unlock / backlight
#include "ui_pixel.h"      // 仅复用主题常量与菜单;本页不用其屏幕
#include "lock_hello.h"    // 锁屏薄荷绿点阵位图(自动生成)
#include "we_cfg.h"        // 社区版配置模型(WiFi 档案 + 平台 Key + 昵称)
#include "we_hist.h"       // 逐日记账数据模型(NVS 存取;门户备份模块共用)
#include "we_provider.h"   // 平台适配:直连官方余额接口
#include "we_portal.h"     // we_cfg_load / we_cfg_save(NVS 存取)

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "balance";

#define ROW_COUNT HIST_ROWS    // 余额页最多同时显示的平台卡槽(与 hist_t 行数同源)
#define ROW_X     14
#define ROW_W     212
#define ROW_H     52
#define ROW_YS    { 118, 180, 242 }
#define ROW_RIGHT (ROW_X + ROW_W)     // 226
#define ROW_RPAD  16                   // 金额距卡片右缘(与左侧状态灯留白对称)

#define DOT_SIZE 12

// ---- 双页:TOKEN JOURNEY(主页)/ TOKEN BALANCE(余额页)----
#define PAGE_JOURNEY 0
#define PAGE_BALANCE 1
#define PAGE_SW_MS   220            // 切页滑动动画时长

// 热力图:最近 30 天 = 6 列 × 5 行(旧→新,左上→右下;今天=右下角)
#define HEAT_N     30
#define HEAT_COLS  6
#define HEAT_ROWS  5
#define HEAT_X0    15              // 格区左缘(与上方摘要卡近似对齐,左右对称)
#define HEAT_Y0    96              // 格区上缘(处于 30 DAYS 卡与图例行正中偏下)
#define HEAT_S     30              // 格边长(正方形)
#define HEAT_G     6               // 格间距(横竖相等)
// 同一紫系:低消耗=浅,高消耗=深(浅→深);今天格用粉红线框高亮
#define HEAT_LV0   0x1A1330        // 空/无消耗:暗紫槽
#define HEAT_LV1   0xC4B5FD        // 低:淡紫(浅)
#define HEAT_LV2   0x8B5CF6        // 中低:亮紫
#define HEAT_LV3   0x5B21B6        // 中高:紫
#define HEAT_LV4   0x2D1B69        // 高:深紫(深)

// ---- 页内控件坐标(供可视化布局编辑器 layout-editor.html 往返回写)----
// 电量胶囊:两页各自定位,J=主页 TOKEN JOURNEY / B=余额页 TOKEN BALANCE
#define BAT_PILL_J_X 174
#define BAT_PILL_J_Y 11
#define BAT_PILL_J_W 50
#define BAT_PILL_J_H 19
#define BAT_PILL_B_X 184
#define BAT_PILL_B_Y 8
#define BAT_PILL_B_W 50
#define BAT_PILL_B_H 22
// 主页图例:4 个色块等距居中,LOW/HIGH 两端文字各自定位
#define LEGEND_SQ_X0   66          // 首块左缘
#define LEGEND_SQ_Y    283
#define LEGEND_SQ_S    12          // 块边长
#define LEGEND_SQ_GAP  32          // 块间距(横向)
#define LEGEND_LO_X    194         // "LOW"字左缘
#define LEGEND_LO_Y    283
#define LEGEND_HI_X    15          // "HIGH"字左缘
#define LEGEND_HI_Y    283

// ---- 深色 AI 风配色 ----
#define C_BG0      0x141B33   // 顶部稍亮的深空蓝
#define C_BG1      0x10162A
#define C_BG2      0x0D1224
#define C_BG3      0x0A0F1E
#define C_BG4      0x080C18
#define C_BG5      0x060910
#define C_HAIR     0x22304F   // 细分隔线
#define C_CARD     0x111B33   // 卡片底
#define C_CARD_BD  0x24345C   // 卡片描边
#define C_TXT_HI   0xF5F8FF   // 主数字/金额
#define C_TXT      0xD3DCF5   // 平台名
#define C_TXT_DIM  0x6E7CA6   // 弱文字
#define C_TXT_FAINT 0x41507A  // 最弱(OK=REFRESH)
#define C_ACCENT   0x22D3EE   // AI 青
#define C_PURPLE   0xA855F7   // 赛博紫(文字/线框用)
#define C_PINK     0xFF69B4   // 赛博粉红(今日花费负值用,深底上清晰)
#define C_DOT_GRAY 0x3D4A63
#define C_DOT_GREEN 0x34D399
#define C_DOT_YELLOW 0xFBBF24
#define C_DOT_RED   0xF87171

#define WIFI_WAIT_MS   pdMS_TO_TICKS(20000)
#define HTTP_TIMEOUT_MS 12000
#define RESP_MAX      4096          // usage[30] 加入后响应 ~2KB,留余量
#define REFRESH_STACK 6144

static int row_y_by(int idx, int n);              // 行 y:上对齐,行少下方留白
#define ROW_YS_STEP(i) row_y_by((i), s_nrow)

static lv_obj_t     *s_scr;          // 根屏(背景 + 双页容器 + 翻页点 + 锁屏遮罩)
static lv_obj_t     *s_jcont;        // 第 1 页容器:TOKEN JOURNEY(主页)
static lv_obj_t     *s_bcont;        // 第 2 页容器:TOKEN BALANCE(余额页)
static int           s_page = PAGE_JOURNEY;   // 当前页

// 余额页元素(全部挂在 s_bcont 内,相对坐标同旧版整屏布局)
static lv_obj_t     *s_cards[ROW_COUNT];    // 平台卡(社区按配置数显隐)
static lv_obj_t     *s_names[ROW_COUNT];    // 平台名(社区可用自定义 label)
static lv_obj_t     *s_dots[ROW_COUNT];
static lv_obj_t     *s_amounts[ROW_COUNT];
static lv_obj_t     *s_spents[ROW_COUNT]; // 今日花费(-x.xx,玫红)
static int           s_row_y[ROW_COUNT];   // 各行当前 y(行数不同分布不同)
static lv_obj_t     *s_total;      // 大号总余额
static lv_obj_t     *s_spent_total; // 今日总消费(总余额右侧,粉红负号)
static lv_obj_t     *s_state;      // 余额页状态/更新时间(居中,弱)

// 主页元素(挂在 s_jcont 内)
static lv_obj_t     *s_cells[HEAT_N];   // 30 天热力格
static lv_obj_t     *s_heat_sum;        // 摘要:使用天数(右对齐大字)
static lv_obj_t     *s_legend[5];       // 图例色块(低→高)
static lv_obj_t     *s_bats[2];         // 每页右上电量胶囊 label

// 热力图数据(本机逐日记账推导,升序;旧→新)
static double        s_usage[HEAT_N];
static double        s_usage_max;
static int           s_usage_days;      // 30 天中实际有消耗的天数
static bool          s_usage_ok;

// 社区配置缓存(creds_load 时读入;UI/worker/记账共用)
static we_cfg_t      s_cfg;
static bool          s_cfg_ok;
static char          s_nick[24];        // 锁屏昵称(社区配置,空=不显示)
static int           s_nrow;            // 当前实际显示的平台行数(1..3)

static lv_obj_t     *s_pgdot[2];    // 底部翻页点(全局,不随页动)
static lv_timer_t   *s_timer;      // 电量刷新
static volatile bool s_busy;
static volatile bool s_cancel;
static volatile int  s_gen;

// ---- 锁屏 + 分级调光(短按 UP 锁屏;无操作 3 分钟自动锁)----
#define LOCK_BL_ENTER 8            // 锁屏起始亮度
#define LOCK_BL_FINAL 3            // 锁屏稳定亮度
#define LOCK_FADE_MS  15000        // 8% → 3% 渐变时长
#define BL_MAIN       80           // 解锁/进入主界面亮度
#define BL_STEP1     45           // 1 分钟后
#define BL_STEP2     10           // 2 分钟后
#define IDLE_T1_MS   60000
#define IDLE_T2_MS   120000
#define IDLE_T3_MS   180000       // 3 分钟整锁屏
static bool          s_locked;
static lv_obj_t     *s_lock_ov;   // 全黑遮罩 + 点阵
static lv_image_dsc_t s_hello_dsc;
static lv_timer_t   *s_idle_timer;   // 250ms 调光/锁屏节拍
static int           s_bl;           // 当前背光
static uint32_t      s_last_act;     // 最近一次操作时刻
static uint32_t      s_lock_at;      // 锁屏起始时刻(亮度渐变用)
static uint32_t      s_unlock_at;    // 解锁时刻(防同一次按键的后续事件误触发)

// ---- 多 WiFi 档案 + 自动找网 ----
typedef struct {
    char ssid[33];
    char pass[65];
    char host[48];       // 该网络下取数服务所在机器的 IP
} bal_prof_t;
static bal_prof_t s_prof[4];
static int   s_nprof;
static bool  s_creds_ok;
static bool  s_data_ok;       // 最近一次刷新是否成功
static uint32_t s_last_try;   // 最近一次刷新尝试时刻
#define RETRY_OFFLINE_MS 90000   // 离线:90s 自动重试
#define RETRY_FRESH_MS   600000  // 在线:10 分钟自动刷新
static lv_timer_t *s_retry_timer;

static void refresh_start(void);
static bool cfg_load_local(void);          // 定义见 NVS 凭据节
static void hist_load(void);               // 定义见本地逐日记账节

// ---------------------------------------------------------------- 基础构件
static lv_obj_t *obj_new(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

// 深色统一背景(不分段,避免色带边界在 LCD 上显线)
static void bg_build(lv_obj_t *scr)
{
    obj_new(scr, 0, 0, 240, 320, C_BG2);
    obj_new(scr, 8, 39, 224, 1, C_HAIR);           // 标题下细分隔线
}

static void dot_set(int idx, uint32_t color)
{
    if (idx < 0 || idx >= ROW_COUNT || !s_dots[idx]) return;
    lv_obj_set_style_bg_color(s_dots[idx], lv_color_hex(color), 0);
}

// 行 y 分布:上对齐,一律从顶部 118 起按行距 62 顺序排(行少时下方留白)
static int row_y_by(int idx, int n)
{
    static const int ys[ROW_COUNT] = ROW_YS;   // { 118, 180, 242 }
    (void)n;
    if (idx < 0) idx = 0;
    if (idx >= ROW_COUNT) idx = ROW_COUNT - 1;
    return ys[idx];
}

static void rows_apply_community(void);   // 定义见 balance_ui_build 之后

static void amount_set(int idx, const char *text)
{
    if (idx < 0 || idx >= ROW_COUNT || !s_amounts[idx]) return;
    lv_label_set_text(s_amounts[idx], text);
    lv_obj_set_style_text_font(s_amounts[idx], &lv_font_montserrat_16, 0);
    // LVGL 尺寸异步:先同步布局再量宽,否则拿到旧宽度会画到框外被裁
    lv_obj_update_layout(s_amounts[idx]);
    int w = lv_obj_get_width(s_amounts[idx]);
    // 余额与平台名同行;行1 偏上,给下方今日花费留出均匀空间(卡高 52)
    lv_obj_set_pos(s_amounts[idx], ROW_RIGHT - ROW_RPAD - w, s_row_y[idx] + 9);
}

// 今日花费:余额下方小字。有花费=-x.xx(粉红);无花费=0.00(弱灰)。
static void spent_set(int idx, const char *text)
{
    if (idx < 0 || idx >= ROW_COUNT || !s_spents[idx]) return;
    lv_obj_set_style_text_font(s_spents[idx], &lv_font_montserrat_14, 0);
    if (!text || !text[0]) {
        lv_obj_add_flag(s_spents[idx], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_spents[idx], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(s_spents[idx],
        text[0] == '-' ? lv_color_hex(C_PINK) : lv_color_hex(C_TXT_DIM), 0);
    lv_label_set_text(s_spents[idx], text);
    lv_obj_update_layout(s_spents[idx]);
    int w = lv_obj_get_width(s_spents[idx]);
    // 行2 上移:使数字上边距(行1)与下边距(行2)相对卡框对称
    lv_obj_set_pos(s_spents[idx], ROW_RIGHT - ROW_RPAD - w, s_row_y[idx] + 28);
}

static void set_state(const char *text)
{
    if (s_state) lv_label_set_text(s_state, text);
}

// 总余额 + 今日总消费作为一组,整体左右居中;总余额大号、总消费小号下对齐。
#define TOTAL_GAP 12   // 两组数字间距

static void layout_total_group(void)
{
    if (!s_total) return;
    lv_obj_update_layout(s_total);
    int w1 = lv_obj_get_width(s_total);
    bool sp_hidden = s_spent_total && lv_obj_has_flag(s_spent_total, LV_OBJ_FLAG_HIDDEN);
    int w2 = 0;
    if (s_spent_total && !sp_hidden) {
        lv_obj_update_layout(s_spent_total);
        w2 = lv_obj_get_width(s_spent_total);
    }
    int group_w = w1 + (w2 > 0 ? TOTAL_GAP + w2 : 0);
    int x0 = (240 - group_w) / 2;
    lv_obj_set_pos(s_total, x0, 60);
    if (s_spent_total && !sp_hidden) {
        // 总消费固定 y=71(常规小字),x 随组居中
        lv_obj_set_pos(s_spent_total, x0 + w1 + TOTAL_GAP, 71);
    }
}

// 总余额:大字(font32)
static void set_total(const char *text)
{
    if (!s_total) return;
    lv_label_set_text(s_total, text);
    layout_total_group();
}

// 今日总消费:总余额右侧,常规小字。花费=-x.xx(粉红);0=0.00(弱灰)。
static void spent_total_set(const char *text)
{
    if (!s_spent_total) return;
    lv_obj_set_style_text_font(s_spent_total, &lv_font_montserrat_20, 0);
    if (!text || !text[0]) {
        lv_obj_add_flag(s_spent_total, LV_OBJ_FLAG_HIDDEN);
        layout_total_group();
        return;
    }
    lv_obj_remove_flag(s_spent_total, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(s_spent_total,
        text[0] == '-' ? lv_color_hex(C_PINK) : lv_color_hex(C_TXT_DIM), 0);
    lv_label_set_text(s_spent_total, text);
    layout_total_group();
}

// ---------------------------------------------------------------- UI 构建
// 每页右上电量胶囊(标题同排,紫色描边),返回内部 label 指针
// 位置尺寸由调用方给出 —— 两页各自定位(见 BAT_PILL_J_* / BAT_PILL_B_*)
static lv_obj_t *bat_pill_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *box = obj_new(parent, x, y, w, h, C_CARD);
    lv_obj_set_style_radius(box, h / 2, 0);   // 全圆角胶囊
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(C_PURPLE), 0);
    lv_obj_t *lb = lv_label_create(box);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(C_TXT_HI), 0);
    lv_obj_center(lb);
    lv_label_set_text(lb, "--%");
    return lb;
}

// 页容器:全屏 240×320,透明底(深空底色由根屏提供),x 用于横向滑动
static lv_obj_t *page_cont_create(lv_obj_t *scr, int x)
{
    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(c, x, 0);
    lv_obj_set_size(c, 240, 320);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    return c;
}

// 第 1 页 TOKEN JOURNEY:标题 + 30 天累计卡 + 热力图 + 图例
static void journey_ui_build(void)
{
    // 标题行:TOKEN JOURNEY(左对齐,无装饰点)+ 右上电量
    lv_obj_t *title = lv_label_create(s_jcont);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TXT), 0);
    lv_obj_set_pos(title, 14, 11);
    lv_label_set_text(title, "TOKEN JOURNEY");
    s_bats[PAGE_JOURNEY] = bat_pill_create(s_jcont, BAT_PILL_J_X, BAT_PILL_J_Y,
                                           BAT_PILL_J_W, BAT_PILL_J_H);

    // 摘要卡:左「30 DAYS」+ 右侧使用天数大字(同字号,卡加高下边框下移)
    lv_obj_t *sum = lv_obj_create(s_jcont);
    lv_obj_remove_flag(sum, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(sum, 14, 48);
    lv_obj_set_size(sum, 212, 36);
    lv_obj_set_style_radius(sum, 10, 0);
    lv_obj_set_style_pad_all(sum, 0, 0);
    lv_obj_set_style_border_width(sum, 1, 0);
    lv_obj_set_style_border_color(sum, lv_color_hex(C_PURPLE), 0);
    lv_obj_set_style_bg_color(sum, lv_color_hex(C_CARD), 0);
    lv_obj_t *sumcap = lv_label_create(sum);
    lv_obj_set_style_text_font(sumcap, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(sumcap, lv_color_hex(C_PURPLE), 0);
    lv_obj_set_pos(sumcap, 16, 9);
    lv_label_set_text(sumcap, "30 DAYS");
    s_heat_sum = lv_label_create(sum);
    lv_obj_set_style_text_font(s_heat_sum, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_heat_sum, lv_color_hex(C_TXT_HI), 0);
    lv_label_set_text(s_heat_sum, "--");
    lv_obj_update_layout(s_heat_sum);
    lv_obj_set_pos(s_heat_sum, 212 - 14 - lv_obj_get_width(s_heat_sum), 9);

    // 热力图 30 格(6 列 × 5 行,旧→新,左上→右下):先全画暗槽
    for (int i = 0; i < HEAT_N; i++) {
        int col = i % HEAT_COLS;
        int row = i / HEAT_COLS;
        lv_obj_t *g = lv_obj_create(s_jcont);
        lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(g, HEAT_X0 + col * (HEAT_S + HEAT_G),
                          HEAT_Y0 + row * (HEAT_S + HEAT_G));
        lv_obj_set_size(g, HEAT_S, HEAT_S);
        lv_obj_set_style_radius(g, 5, 0);
        lv_obj_set_style_pad_all(g, 0, 0);
        lv_obj_set_style_border_width(g, 1, 0);
        lv_obj_set_style_border_color(g, lv_color_hex(HEAT_LV0), 0);
        lv_obj_set_style_bg_color(g, lv_color_hex(HEAT_LV0), 0);
        s_cells[i] = g;
    }

    // 图例:4 个正方形色块等距居中,两端文字位置由 LEGEND_LO_* / LEGEND_HI_* 决定
    // 垂直:处于热力格底(264)与翻页点(304)正中
    static const uint32_t lg[4] = { HEAT_LV1, HEAT_LV2, HEAT_LV3, HEAT_LV4 };
    lv_obj_t *hi = lv_label_create(s_jcont);
    lv_obj_set_style_text_font(hi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hi, lv_color_hex(C_TXT_FAINT), 0);
    lv_label_set_text(hi, "HIGH");
    lv_obj_set_pos(hi, LEGEND_HI_X, LEGEND_HI_Y);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = obj_new(s_jcont, LEGEND_SQ_X0 + i * LEGEND_SQ_GAP, LEGEND_SQ_Y,
                              LEGEND_SQ_S, LEGEND_SQ_S, lg[i]);
        lv_obj_set_style_radius(b, 2, 0);
        s_legend[i] = b;
    }
    lv_obj_t *lo = lv_label_create(s_jcont);
    lv_obj_set_style_text_font(lo, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lo, lv_color_hex(C_TXT_FAINT), 0);
    lv_label_set_text(lo, "LOW");
    lv_obj_set_pos(lo, LEGEND_LO_X, LEGEND_LO_Y);
}

// 第 2 页 TOKEN BALANCE(原整屏布局,迁入 s_bcont 容器)
static void balance_ui_build(void)
{
    // 标题行:TOKEN BALANCE(左对齐,无装饰点)+ 右上电量
    lv_obj_t *title = lv_label_create(s_bcont);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TXT), 0);
    lv_obj_set_pos(title, 14, 11);
    lv_label_set_text(title, "TOKEN BALANCE");
    s_bats[PAGE_BALANCE] = bat_pill_create(s_bcont, BAT_PILL_B_X, BAT_PILL_B_Y,
                                           BAT_PILL_B_W, BAT_PILL_B_H);

    // 大号总余额(居中,一目了然)
    lv_obj_t *cap = lv_label_create(s_bcont);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(C_PURPLE), 0);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 44);
    lv_label_set_text(cap, "TOTAL  CNY");

    s_total = lv_label_create(s_bcont);
    lv_obj_set_style_text_font(s_total, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_total, lv_color_hex(C_TXT_HI), 0);
    set_total("--");

    // 今日总消费:总余额右侧(数据到达前隐藏)
    s_spent_total = lv_label_create(s_bcont);
    lv_obj_set_style_text_font(s_spent_total, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_spent_total, lv_color_hex(C_TXT_DIM), 0);
    lv_obj_add_flag(s_spent_total, LV_OBJ_FLAG_HIDDEN);

    // 更新时间/状态:居中于总余额下方;两侧配青色短分割线(装饰,省纵向空间)
    s_state = lv_label_create(s_bcont);
    lv_obj_set_style_text_font(s_state, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_state, lv_color_hex(C_TXT_DIM), 0);
    lv_obj_align(s_state, LV_ALIGN_TOP_MID, 0, 96);
    lv_label_set_text(s_state, "");

    lv_obj_t *line_l = obj_new(s_bcont, ROW_X + 2, 101, 38, 4, C_ACCENT);
    lv_obj_t *line_r = obj_new(s_bcont, ROW_X + ROW_W - 40, 101, 38, 4, C_ACCENT);
    (void)line_l;
    (void)line_r;

    // 平台卡槽(最多 3):左状态灯 + 平台名 + 右侧余额;社区按配置数显隐重排
    for (int i = 0; i < ROW_COUNT; i++) {
        s_row_y[i] = ROW_YS_STEP(i);
        lv_obj_t *card = lv_obj_create(s_bcont);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(card, ROW_X, s_row_y[i]);
        lv_obj_set_size(card, ROW_W, ROW_H);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(C_PURPLE), 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
        s_cards[i] = card;

        // 状态灯整卡纵向居中;dot 12px 中心对齐
        lv_obj_t *dot = obj_new(s_bcont, ROW_X + 16, s_row_y[i] + (ROW_H - DOT_SIZE) / 2,
                                DOT_SIZE, DOT_SIZE, C_DOT_GRAY);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        s_dots[i] = dot;

        lv_obj_t *name = lv_label_create(s_bcont);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_TXT), 0);
        // 平台名整卡纵向居中(右侧余额/花费两行均布,左右互不干扰)
        lv_obj_set_pos(name, ROW_X + 42, s_row_y[i] + (ROW_H - 19) / 2);
        lv_label_set_text(name, "--");
        s_names[i] = name;

        lv_obj_t *amount = lv_label_create(s_bcont);
        lv_obj_set_style_text_color(amount, lv_color_hex(C_TXT_HI), 0);
        s_amounts[i] = amount;
        amount_set(i, "--");

        // 今日花费(默认隐藏;有花费时 -x.xx 玫红,余额下方右对齐)
        lv_obj_t *spent = lv_label_create(s_bcont);
        lv_obj_set_style_text_color(spent, lv_color_hex(C_PINK), 0);
        s_spents[i] = spent;
        lv_obj_add_flag(spent, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);   // 先用,rows_apply 按配置放开
    }

    rows_apply_community();
}

// 社区行布局:按已配置平台数(1..3)重排行 y、显隐卡槽、写平台名(label 或默认名)
static void rows_apply_community(void)
{
    int n = s_cfg_ok ? s_cfg.prov_count : 0;
    if (n < 1) n = 1;
    if (n > ROW_COUNT) n = ROW_COUNT;
    s_nrow = n;

    for (int i = 0; i < ROW_COUNT; i++) {
        bool on = (i < n);
        if (!s_cards[i]) continue;
        if (!on) {
            lv_obj_add_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
            if (s_dots[i]) lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
            if (s_names[i]) lv_obj_add_flag(s_names[i], LV_OBJ_FLAG_HIDDEN);
            if (s_amounts[i]) lv_obj_add_flag(s_amounts[i], LV_OBJ_FLAG_HIDDEN);
            if (s_spents[i]) lv_obj_add_flag(s_spents[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        s_row_y[i] = ROW_YS_STEP(i);
        lv_obj_remove_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
        if (s_dots[i]) lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
        if (s_names[i]) lv_obj_remove_flag(s_names[i], LV_OBJ_FLAG_HIDDEN);
        if (s_amounts[i]) lv_obj_remove_flag(s_amounts[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_cards[i], ROW_X, s_row_y[i]);
        if (s_dots[i]) lv_obj_set_pos(s_dots[i], ROW_X + 16,
                                      s_row_y[i] + (ROW_H - DOT_SIZE) / 2);
        if (s_names[i]) {
            const char *nm = NULL;
            if (s_cfg_ok && i < s_cfg.prov_count && s_cfg.prov[i].label[0]) {
                nm = s_cfg.prov[i].label;
            } else if (s_cfg_ok && i < s_cfg.prov_count) {
                const we_provider_t *p = NULL;
                if (we_provider_lookup(s_cfg.prov[i].provider_id, &p)) nm = p->display_name;
            }
            if (!nm) nm = "--";
            lv_label_set_text(s_names[i], nm);
            lv_obj_set_pos(s_names[i], ROW_X + 42,
                           s_row_y[i] + (ROW_H - 19) / 2);
        }
        amount_set(i, "--");
        spent_set(i, "");
    }
}

static void page_dots_refresh(void)
{
    for (int i = 0; i < 2; i++) {
        if (!s_pgdot[i]) continue;
        lv_obj_set_style_bg_color(s_pgdot[i],
            lv_color_hex(i == s_page ? C_PURPLE : 0x3A2E6E), 0);
    }
}

static void ui_build(void)
{
    // 根屏:深空底色;子对象 = 两个页面容器 + 底部翻页点 + (锁屏遮罩后建)
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG2), 0);

    s_page = PAGE_JOURNEY;

    // 第 1 页在屏内,第 2 页在屏右(离屏隐藏,待切换)
    s_jcont = page_cont_create(s_scr, 0);
    s_bcont = page_cont_create(s_scr, 240);
    bg_build(s_jcont);
    bg_build(s_bcont);
    journey_ui_build();
    balance_ui_build();
    lv_obj_add_flag(s_bcont, LV_OBJ_FLAG_HIDDEN);

    // 底部翻页点(全局,不随页动):2 点近距居中,当前页紫、另一页暗紫
    for (int i = 0; i < 2; i++) {
        lv_obj_t *d = obj_new(s_scr, 114 + i * 12, 304, 6, 6, 0x3A2E6E);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        s_pgdot[i] = d;
    }
    page_dots_refresh();
}

static void battery_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_scr) return;
    int soc = bsp_battery_soc();
    int mv = bsp_battery_mv();
    // CW2017 上电后 SOC 从 0 爬升至真实值需约 40-60s(库仑计初始化);
    // 电压充足却报 0% → 视为计算中,显示占位 "--",避免误导成没电
    if (soc == 0 && mv > 3600) soc = -1;
    for (int i = 0; i < 2; i++) {
        if (!s_bats[i]) continue;
        if (soc < 0) {
            lv_label_set_text(s_bats[i], "--%");
            lv_obj_set_style_text_color(s_bats[i], lv_color_hex(C_TXT_HI), 0);
        } else {
            lv_label_set_text_fmt(s_bats[i], "%d%%", soc);
            lv_obj_set_style_text_color(s_bats[i],
                soc < 20 ? lv_color_hex(C_DOT_RED) : lv_color_hex(C_TXT_HI), 0);
        }
    }
}

// UI 指针集体遗忘(屏对象删除后置空,防悬垂;enter/exit/worker 三处复用)
static void ui_forget(void)
{
    s_scr = NULL;
    s_jcont = s_bcont = NULL;
    s_state = s_total = NULL;
    s_spent_total = NULL;
    s_heat_sum = NULL;
    for (int i = 0; i < ROW_COUNT; i++) {
        s_cards[i] = NULL; s_names[i] = NULL;
        s_dots[i] = NULL; s_amounts[i] = NULL; s_spents[i] = NULL;
    }
    for (int i = 0; i < HEAT_N; i++) s_cells[i] = NULL;
    for (int i = 0; i < 2; i++) { s_bats[i] = NULL; s_pgdot[i] = NULL; }
}

// 摘要右对齐文本(卡片右侧大字)
static void heat_sum_set(const char *txt)
{
    if (!s_heat_sum) return;
    lv_label_set_text(s_heat_sum, txt);
    lv_obj_update_layout(s_heat_sum);
    lv_obj_set_pos(s_heat_sum, 212 - 14 - lv_obj_get_width(s_heat_sum), 9);
}

// 用 usage[30] 重绘主页:使用天数 + 30 格分档着色(紫系深浅),今天格粉框
static void journey_refresh(void)
{
    if (!s_usage_ok) return;

    char buf[16];
    if (s_usage_days == 1)
        snprintf(buf, sizeof(buf), "%d DAY", s_usage_days);
    else
        snprintf(buf, sizeof(buf), "%d DAYS", s_usage_days);
    heat_sum_set(buf);

    for (int i = 0; i < HEAT_N; i++) {
        uint32_t col = HEAT_LV0;
        if (s_usage[i] > 0.004) {
            double r = (s_usage_max > 0.0) ? (s_usage[i] / s_usage_max) : 1.0;
            int lv = (r < 0.25) ? 1 : (r < 0.5) ? 2 : (r < 0.75) ? 3 : 4;
            col = (lv == 1) ? HEAT_LV1 : (lv == 2) ? HEAT_LV2 :
                  (lv == 3) ? HEAT_LV3 : HEAT_LV4;
        }
        if (s_cells[i]) {
            lv_obj_set_style_bg_color(s_cells[i], lv_color_hex(col), 0);
        }
    }
    // 今天格(右下)粉红线框高亮
    lv_obj_t *today = s_cells[HEAT_N - 1];
    if (today) {
        lv_obj_set_style_border_color(today, lv_color_hex(C_PINK), 0);
    }
}

// 直接切换页面(无动画):journey x∈{0,-240},balance 恒在其右 240px
static void page_goto(int to)
{
    if (to == s_page || !s_scr || !s_jcont || !s_bcont) return;
    s_page = to;
    int x = (to == PAGE_JOURNEY) ? 0 : -240;
    lv_obj_set_x(s_jcont, x);
    lv_obj_set_x(s_bcont, x + 240);
    lv_obj_t *off = (to == PAGE_JOURNEY) ? s_bcont : s_jcont;
    lv_obj_t *on  = (to == PAGE_JOURNEY) ? s_jcont : s_bcont;
    lv_obj_add_flag(off, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(on, LV_OBJ_FLAG_HIDDEN);
    page_dots_refresh();
}

// ---------------------------------------------------------------- 锁屏
static void lock_ui_init(void)
{
    s_hello_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_hello_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_hello_dsc.header.flags = 0;
    s_hello_dsc.header.w = HELLO_IMG_W;
    s_hello_dsc.header.h = HELLO_IMG_H;
    s_hello_dsc.header.stride = HELLO_IMG_W * 2;
    s_hello_dsc.header.reserved_2 = 0;
    s_hello_dsc.data_size = HELLO_IMG_W * HELLO_IMG_H * 2;
    s_hello_dsc.data = (const uint8_t *)hello_map;
}

static void lock_show(void)
{
    if (!s_scr || s_locked) return;
    s_locked = true;
    if (!s_lock_ov) {
        s_lock_ov = lv_obj_create(s_scr);
        lv_obj_remove_flag(s_lock_ov, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_lock_ov, 0, 0);
        lv_obj_set_size(s_lock_ov, 240, 320);
        lv_obj_set_style_radius(s_lock_ov, 0, 0);
        lv_obj_set_style_border_width(s_lock_ov, 0, 0);
        lv_obj_set_style_pad_all(s_lock_ov, 0, 0);
        lv_obj_set_style_bg_color(s_lock_ov, lv_color_hex(0x000000), 0);
        lv_obj_t *img = lv_image_create(s_lock_ov);
        lv_image_set_src(img, &s_hello_dsc);
        lv_obj_set_pos(img, 0, (320 - HELLO_IMG_H) / 2);

        // 底部签名:取配置昵称;空则不显示(社区版默认无个人标识)
        if (s_nick[0]) {
            lv_obj_t *foot = lv_label_create(s_lock_ov);
            lv_obj_set_style_text_font(foot, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(foot, lv_color_hex(C_TXT_DIM), 0);
            lv_obj_align(foot, LV_ALIGN_TOP_MID, 0, 292);
            lv_label_set_text(foot, s_nick);
        }
    }
    s_lock_at = lv_tick_get();          // 开始 8%→3% 渐变计时
    s_bl = LOCK_BL_ENTER;
    bsp_display_backlight(s_bl);
}

static void unlock_show(void)
{
    if (!s_locked) return;
    s_locked = false;
    s_unlock_at = lv_tick_get();       // 解锁防抖起点
    s_last_act  = lv_tick_get();       // 重新开始分级调光计时
    s_bl = BL_MAIN;
    if (s_lock_ov) { lv_obj_delete(s_lock_ov); s_lock_ov = NULL; }
    bsp_display_backlight(s_bl);
}

static void lock_reset_state(void)
{
    if (s_idle_timer)  { lv_timer_delete(s_idle_timer);  s_idle_timer  = NULL; }
    if (s_retry_timer) { lv_timer_delete(s_retry_timer); s_retry_timer = NULL; }
    if (s_lock_ov)     { lv_obj_delete(s_lock_ov); s_lock_ov = NULL; }
    s_locked = false;
    s_bl = 100;
    bsp_display_backlight(100);
}

// 250ms 节拍:主界面无操作分级调光(80→45→10),3 分钟整锁屏;
// 锁屏态 8%→3% 于 15s 内渐变
static void dim_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_scr) return;

    if (s_locked) {
        uint32_t el = lv_tick_get() - s_lock_at;
        int t = LOCK_BL_FINAL;
        if (el < LOCK_FADE_MS) {
            t = LOCK_BL_ENTER -
                ((LOCK_BL_ENTER - LOCK_BL_FINAL) * (int)el) / (int)LOCK_FADE_MS;
        }
        if (t < LOCK_BL_FINAL) t = LOCK_BL_FINAL;
        if (s_bl != t) { s_bl = t; bsp_display_backlight(s_bl); }
        return;
    }

    uint32_t el = lv_tick_get() - s_last_act;
    int target;
    if (el < IDLE_T1_MS)      target = BL_MAIN;
    else if (el < IDLE_T2_MS) target = BL_STEP1;
    else if (el < IDLE_T3_MS) target = BL_STEP2;
    else { lock_show(); return; }

    if (s_bl == target) return;
    int d = (target > s_bl) ? 6 : -6;      // 每 250ms 平滑 6 档(~2 秒完成一档)
    s_bl += d;
    if ((d > 0 && s_bl > target) || (d < 0 && s_bl < target)) s_bl = target;
    if (s_bl < 1) s_bl = 1;
    if (s_bl > 100) s_bl = 100;
    bsp_display_backlight(s_bl);
}

// 60s 心跳:离线 90s 自动重试 / 在线 10 分钟自动刷新(锁屏或忙碌时跳过)
static void retry_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_scr || s_locked || s_busy) return;
    uint32_t ago = lv_tick_get() - s_last_try;
    if (s_data_ok) {
        if (ago >= RETRY_FRESH_MS) refresh_start();
    } else {
        if (ago >= RETRY_OFFLINE_MS) refresh_start();
    }
}

void demo_balance_enter(void)
{
    s_gen++;
    s_cancel = false;
    s_busy = false;
    lock_ui_init();
    lock_reset_state();                  // 清掉上次遗留遮罩/定时器,恢复亮度

    // 先载入社区配置(NVS 幂等初始化):UI 行数/昵称/锁屏都依赖它
    demo_radio_nvs_prepare();
    cfg_load_local();
    hist_load();

    if (s_scr) {
        if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
        lv_obj_delete(s_scr);
        ui_forget();
    }

    ui_build();
    battery_tick(NULL);
    s_timer = lv_timer_create(battery_tick, 30000, NULL);
    lv_screen_load(s_scr);

    // 分级调光/自动锁节拍:进入即 80%,无操作计时归零
    s_last_act = lv_tick_get();
    s_bl = BL_MAIN;
    bsp_display_backlight(s_bl);
    s_idle_timer = lv_timer_create(dim_tick, 250, NULL);

    // 自动找网心跳(离线 90s 重试 / 在线 10min 刷新)
    s_data_ok = false;
    s_last_try = 0;
    s_retry_timer = lv_timer_create(retry_cb, 60000, NULL);

    refresh_start();
}

void demo_balance_exit(void)
{
    s_cancel = true;
    lock_reset_state();                  // 退出页面:取消自动锁、删遮罩、恢复亮度
    if (!s_busy) {
        if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
        if (s_scr)  { lv_obj_delete(s_scr); }
        ui_forget();
    }
}

void demo_balance_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    s_last_act = lv_tick_get();               // 有操作即重新开始调光计时

    // 锁屏态:仅 UP 可解锁(锁屏/解锁都只认 UP);DOWN/OK 一律忽略,防误触
    if (s_locked) {
        if (btn == BSP_BTN_UP) unlock_show();
        return;
    }

    if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
        if (lv_tick_get() - s_unlock_at < 500) return;  // 刚解锁:忽略同一次按键的收尾事件
        lock_show();
        return;
    }

    // DOWN:水平滑动切换 主页 ↔ 余额页(动画中忽略;解锁 500ms 内忽略收尾事件)
    if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
        if (lv_tick_get() - s_unlock_at < 500) return;
        page_goto(s_page == PAGE_JOURNEY ? PAGE_BALANCE : PAGE_JOURNEY);
        return;
    }

    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK && !s_busy) {
        refresh_start();
    }
}

// ---------------------------------------------------------------- NVS 凭据
// 社区版配置主路径:we_cfg 读 WiFi 档案 → s_prof;昵称/平台缓存到 s_cfg
static bool cfg_load_local(void)
{
    s_cfg_ok = false;
    s_nprof = 0;
    s_creds_ok = false;
    s_nick[0] = '\0';

    we_cfg_t cfg;
    if (!we_cfg_load(&cfg) || !we_cfg_validate(&cfg)) {
        ESP_LOGW(TAG, "社区配置缺失或损坏(先到 Portal 配网)");
        return false;
    }
    s_cfg = cfg;
    s_cfg_ok = true;
    strlcpy(s_nick, cfg.nickname, sizeof(s_nick));

    for (uint8_t i = 0; i < cfg.wifi_count && i < 4 && s_nprof < 4; i++) {
        if (!cfg.wifi[i].ssid[0]) continue;
        strlcpy(s_prof[s_nprof].ssid, cfg.wifi[i].ssid, sizeof(s_prof[s_nprof].ssid));
        strlcpy(s_prof[s_nprof].pass, cfg.wifi[i].pass, sizeof(s_prof[s_nprof].pass));
        s_prof[s_nprof].host[0] = '\0';
        ESP_LOGI(TAG, "档案[%d] %s", s_nprof, s_prof[s_nprof].ssid);
        s_nprof++;
    }
    if (s_nprof <= 0) {
        ESP_LOGW(TAG, "社区配置无 WiFi 档案");
        return false;
    }
    s_creds_ok = true;
    return true;
}

// ---------------------------------------------------------------- Wi-Fi + HTTP
static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta;
static esp_event_handler_instance_t s_h_wifi, s_h_ip;
static bool s_wifi_inited, s_wifi_started, s_hw_reg, s_hip_reg;
#define BIT_IP   BIT0
#define BIT_FAIL BIT1

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "Wi-Fi 断开 reason=%d", d ? d->reason : -1);
        xEventGroupSetBits(s_ev, BIT_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "已获取 IP");
        xEventGroupSetBits(s_ev, BIT_IP);
    }
}

// 彻底回收 Wi-Fi 资源(连上取数完成后调用;失败路径也复用)
static void wifi_teardown(void)
{
    if (s_wifi_started) { esp_wifi_stop(); s_wifi_started = false; }
    if (s_hip_reg)  { esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_h_ip);  s_hip_reg = false; }
    if (s_hw_reg)   { esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi); s_hw_reg = false; }
    if (s_wifi_inited) { esp_wifi_deinit(); s_wifi_inited = false; }
    if (s_sta) { esp_netif_destroy_default_wifi(s_sta); s_sta = NULL; }
    if (s_ev) { vEventGroupDelete(s_ev); s_ev = NULL; }
}

// 返回 0=已连上(成功后 Wi-Fi 保持开启,由调用方在取数后 wifi_teardown);
// 否则负错误码(内部已回收)。
static int wifi_connect_once(const bal_prof_t *p)
{
    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) return -1;
    err = demo_radio_network_prepare();
    if (err != ESP_OK) return -2;

    s_sta = esp_netif_create_default_wifi_sta();
    if (!s_sta) return -3;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) { s_sta = NULL; return -4; }
    s_wifi_inited = true;

    s_ev = xEventGroupCreate();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL, &s_h_wifi);
    s_hw_reg = true;
    esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL, &s_h_ip);
    s_hip_reg = true;

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, p->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, p->pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;    // 不挑认证方式,按 AP 实际协商
    esp_wifi_set_config(WIFI_IF_STA, &wc);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        wifi_teardown();
        return -5;
    }
    s_wifi_started = true;

    EventBits_t bits = xEventGroupWaitBits(s_ev, BIT_IP | BIT_FAIL, pdFALSE, pdFALSE, WIFI_WAIT_MS);
    if (bits & BIT_IP) {
        return 0;                                   // 成功:Wi-Fi 保持开,取数完再回收
    }
    wifi_teardown();
    return -6;
}

// ---------------------------------------------------------------- 平台直连(HTTPS 官方余额)
static int https_fetch_provider(const we_provider_t *prov, const char *key,
                                char *out, size_t cap)
{
    esp_http_client_config_t hcfg = {
        .url = prov->url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&hcfg);
    if (!cl) return -1;

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    esp_http_client_set_header(cl, "Authorization", auth);
    esp_http_client_set_header(cl, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(cl, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTPS open %s 失败: %s", prov->url, esp_err_to_name(err));
        esp_http_client_cleanup(cl);
        return -2;
    }
    esp_http_client_fetch_headers(cl);
    ESP_LOGI(TAG, "HTTPS %s 状态=%d", prov->id, esp_http_client_get_status_code(cl));

    size_t used = 0;
    int r;
    while (used + 1 < cap &&
           (r = esp_http_client_read(cl, out + used, cap - used - 1)) > 0) {
        used += (size_t)r;
    }
    out[used] = '\0';
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    return (int)used;
}

// ---------------------------------------------------------------- SNTP 时间(UTC+8)
#define SNTP_WAIT_MS 3500   // 取数前最多等这么久校时(首次约 1~3s;等不到也继续)
static bool s_sntp_done;

static void ensure_sntp(void)
{
    if (s_sntp_done) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    s_sntp_done = true;
}

static bool time_synced(void)
{
    return time(NULL) >= 1700000000;   // 2023-11 之后才算校时完成
}

static void wait_sntp(int wait_ms)
{
    int waited = 0;
    while (waited < wait_ms && !time_synced()) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
}

// UTC+8 的 epoch 日序号(记账天界;与北京时间一致)
static uint16_t epoch_day_utc8(void)
{
    time_t t = time(NULL) + 8 * 3600;
    return (uint16_t)(t / 86400);
}

static void utc8_tm(struct tm *out)
{
    time_t t = time(NULL) + 8 * 3600;
    gmtime_r(&t, out);
}

// UPDATED 行文本(HH:MM;未校时返回 false)
static bool updated_hhmm(char *buf, size_t cap)
{
    if (!time_synced()) return false;
    struct tm tm;
    utc8_tm(&tm);
    snprintf(buf, cap, "UPDATED %02d:%02d", tm.tm_hour, tm.tm_min);
    return true;
}

// ---------------------------------------------------------------- 本地逐日消耗记账
// 每天首次成功取数把各平台余额记为“当日基线”;某日消耗 ≈ 当日基线 − 次日基线
// (次日首快照≈当日末尾余额)。31 槽环形覆盖 30 天差。
// 数据结构与 NVS 存取见 we_hist.h(门户备份模块复用同一份 blob)。
static hist_t s_hist;
static bool   s_hist_loaded;

// NVS ns=balhist key=log;magic 不符或从未写过 → 清零返回 false
bool app_hist_nvs_read(hist_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(HIST_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, HIST_NVS_KEY, out, &len);
    nvs_close(h);
    if (err != ESP_OK || out->magic != HIST_MAGIC) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

// 调用方须先把 magic 置为 HIST_MAGIC(本函数不替调用方补,避免静默接受脏数据)
bool app_hist_nvs_write(const hist_t *in)
{
    if (!in || in->magic != HIST_MAGIC) return false;
    nvs_handle_t h;
    if (nvs_open(HIST_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, HIST_NVS_KEY, in, sizeof(*in));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static void hist_load(void)
{
    if (s_hist_loaded) return;
    s_hist_loaded = true;
    if (app_hist_nvs_read(&s_hist)) ESP_LOGI(TAG, "hist 载入");
}

static void hist_save(void)
{
    s_hist.magic = HIST_MAGIC;
    app_hist_nvs_write(&s_hist);
}

// 平台行语义校验:当前配置平台顺序与记账时不一致(中途增删/调序)
// → 旧快照行含义错位,一律清空历史从今天重记,防止伪差值。
static void hist_check_rows(int n)
{
    bool changed = false;
    for (int i = 0; i < ROW_COUNT; i++) {
        const char *want = (i < n) ? s_cfg.prov[i].provider_id : "";
        if (strncmp(s_hist.row_id[i], want, sizeof(s_hist.row_id[i])) != 0) {
            changed = true;
            break;
        }
    }
    if (changed) {
        ESP_LOGW(TAG, "hist 平台行变化(%d 平台):清空旧历史重记", n);
        memset(&s_hist, 0, sizeof(s_hist));   // magic 由 hist_save 补
    }
    for (int i = 0; i < ROW_COUNT; i++) {
        const char *want = (i < n) ? s_cfg.prov[i].provider_id : "";
        strlcpy(s_hist.row_id[i], want, sizeof(s_hist.row_id[i]));
    }
}

// 记录今日基线(今日已记则不覆盖,保首快照)
static void hist_set_day(uint16_t day, const float *cur, const bool *ok, int n)
{
    int idx = day % HIST_DAYS;
    if (s_hist.day[idx] == day) return;
    s_hist.day[idx] = day;
    for (int i = 0; i < n; i++) s_hist.bal[idx][i] = ok[i] ? cur[i] : 0.0f;
    hist_save();
}

// 取某日基线;无记录返回 false
static bool hist_base(uint16_t day, int n, float *out)
{
    int idx = day % HIST_DAYS;
    if (s_hist.day[idx] != day) return false;
    for (int i = 0; i < n; i++) out[i] = s_hist.bal[idx][i];
    return true;
}

// 生成最近 30 天每日总消耗 → s_usage[](升序,旧→新;今天=末位)
static void hist_build_usage(uint16_t today, const float *cur, const bool *ok, int n)
{
    for (int i = 0; i < HEAT_N; i++) s_usage[i] = 0.0;

    // 今天:基线 − 当前实时(全平台合计)
    float tb[ROW_COUNT] = { 0 };
    if (hist_base(today, n, tb)) {
        double s = 0;
        for (int i = 0; i < n; i++) {
            if (ok[i] && tb[i] > cur[i]) s += (double)(tb[i] - cur[i]);
        }
        s_usage[HEAT_N - 1] = s;
    }

    // 历史日 i=1..29(昨天往前):消耗 = base(d) − base(d+1)
    for (int i = 1; i < HEAT_N; i++) {
        uint16_t d  = today - (uint16_t)i;
        uint16_t dn = d + 1;
        float bd[ROW_COUNT], bn[ROW_COUNT];
        if (!hist_base(d, n, bd)) continue;
        if (!hist_base(dn, n, bn)) continue;
        double s = 0;
        for (int r = 0; r < n; r++) {
            if (bd[r] > 0.01f && bd[r] > bn[r]) s += (double)(bd[r] - bn[r]);
        }
        s_usage[HEAT_N - 1 - i] = s;
    }

    s_usage_max = 0;
    s_usage_days = 0;
    for (int i = 0; i < HEAT_N; i++) {
        if (s_usage[i] > s_usage_max) s_usage_max = s_usage[i];
        if (s_usage[i] > 0.004) s_usage_days++;
    }
    s_usage_ok = true;
}

static void balance_worker(void *arg)
{
    (void)arg;
    int gen = s_gen;

    // 1. 状态提示
    if (!s_cancel) {
        bsp_lvgl_lock(1000);
        set_state("REFRESHING...");
        bsp_lvgl_unlock();
    }

    // 2. NVS + 社区配置(必须先初始化 NVS,示例封装幂等可重复调)
    if (demo_radio_nvs_prepare() != ESP_OK) {
        if (!s_cancel) {
            bsp_lvgl_lock(1000);
            set_state("NVS FAIL");
            bsp_lvgl_unlock();
        }
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    if (!cfg_load_local()) {
        if (!s_cancel) {
            bsp_lvgl_lock(1000);
            set_state("NO WIFI");
            bsp_lvgl_unlock();
        }
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    hist_load();

    // 一个平台 Key 都没有 → 联网也没有可查的对象。直接点明原因,别让下面的
    // 取数循环空跑一轮再报 DIRECT FAIL(那个提示会被误读成网络问题)。
    if (s_cfg.prov_count == 0) {
        ESP_LOGW(TAG, "没有平台 Key,请到 Portal 填 API Key");
        if (!s_cancel) {
            bsp_lvgl_lock(1000);
            set_state("NO API KEY");
            s_data_ok = false;
            bsp_lvgl_unlock();
        }
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    // 3. 联网:按档案顺序尝试
    int rc = -1;
    for (int i = 0; i < s_nprof; i++) {
        if (s_cancel) break;
        rc = wifi_connect_once(&s_prof[i]);
        if (rc == 0) {
            ESP_LOGI(TAG, "已连 %s", s_prof[i].ssid);
            break;
        }
        ESP_LOGW(TAG, "档案[%d] %s 失败 rc=%d", i, s_prof[i].ssid, rc);
    }
    if (rc != 0) {
        if (!s_cancel) {
            bsp_lvgl_lock(1000);
            set_state("WIFI FAIL");
            s_data_ok = false;
            bsp_lvgl_unlock();
        }
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    if (s_cancel) { wifi_teardown(); s_busy = false; vTaskDelete(NULL); return; }

    // 4. 直连官方余额(只查已配置平台),同时收实时余额做本地记账
    // 先校时再发请求:HTTPS 走证书包校验,会比对证书有效期,冷启动时 RTC 还停在
    // 1970,证书会被判成"尚未生效"而握手失败 —— 所有平台一起失败,屏幕只剩
    // DIRECT FAIL。原来的等待放在取数之后,等于第一次刷新必然拿不到数。
    ensure_sntp();                       // 直连需要本地时间(更新时间/记账天界)
    wait_sntp(SNTP_WAIT_MS);
    int n = s_cfg.prov_count;
    if (n > ROW_COUNT) n = ROW_COUNT;
    float cur[ROW_COUNT] = { 0 };
    bool  okr[ROW_COUNT] = { false };
    int   n_ok = 0;
    double total = 0;

    if (!s_cancel) {
        bsp_lvgl_lock(1000);
        for (int i = 0; i < n; i++) { dot_set(i, C_DOT_GRAY); amount_set(i, "--"); }
        bsp_lvgl_unlock();
    }

    for (int k = 0; k < n && !s_cancel; k++) {
        const we_provider_t *prov = NULL;
        if (!we_provider_lookup(s_cfg.prov[k].provider_id, &prov)) continue;
        static char dresp[RESP_MAX];
        int gotd = https_fetch_provider(prov, s_cfg.prov[k].api_key, dresp, sizeof(dresp));
        char amt[24];
        bool ok = gotd > 0 && we_provider_parse(prov->id, dresp, amt, sizeof(amt));
        double v = ok ? strtod(amt, NULL) : 0;
        cur[k] = (float)v;
        okr[k] = ok;
        if (ok) { total += v; n_ok++; }
        if (!s_cancel) {
            bsp_lvgl_lock(1000);
            if (ok) {
                char dsp[24];
                snprintf(dsp, sizeof(dsp), "%.2f", v);
                dot_set(k, C_DOT_GREEN);
                amount_set(k, dsp);
            } else {
                dot_set(k, C_DOT_RED);
                amount_set(k, "---");
            }
            bsp_lvgl_unlock();
        }
        ESP_LOGI(TAG, "direct %s got=%d ok=%d", prov->id, gotd, ok);
    }

    if (!s_cancel) {
        bsp_lvgl_lock(1000);
        if (n_ok > 0) {
            char t[16];
            snprintf(t, sizeof(t), "%.2f", total);
            set_total(t);
            // 本地逐日记账 → 今日每行花费 + 主页 30 天热力
            if (time_synced()) {
                hist_check_rows(n);            // 平台行变化检测(清历史重记)
                uint16_t day = epoch_day_utc8();
                hist_set_day(day, cur, okr, n);
                float tb[ROW_COUNT] = { 0 };
                bool hb = hist_base(day, n, tb);
                double spent_all = 0;
                for (int i = 0; i < n; i++) {
                    char spent[20] = "0.00";
                    if (okr[i] && hb && tb[i] > cur[i] + 0.001f) {
                        snprintf(spent, sizeof(spent), "-%.2f", (double)(tb[i] - cur[i]));
                        spent_all += (double)(tb[i] - cur[i]);
                    }
                    spent_set(i, spent);
                }
                char st[20] = "0.00";
                if (spent_all > 0.005) snprintf(st, sizeof(st), "-%.2f", spent_all);
                spent_total_set(st);
                hist_build_usage(day, cur, okr, n);
                journey_refresh();
            }
            char up[24];
            set_state(updated_hhmm(up, sizeof(up)) ? up : "");
            s_data_ok = true;
        } else {
            set_state("DIRECT FAIL");
            s_data_ok = false;
        }
        bsp_lvgl_unlock();
    }

    wifi_teardown();                 // 取数结束,关 Wi-Fi 省电并释放资源

    // 5. 离开页面的迟到清理(仅当仍是本代且 exit 把清理留给了我们)
    if (s_cancel && gen == s_gen && s_scr) {
        bsp_lvgl_lock(1000);
        if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
        if (s_scr) { lv_obj_delete(s_scr); }
        ui_forget();
        bsp_lvgl_unlock();
    }

    s_busy = false;
    ESP_LOGI(TAG, "刷新完成(gen=%d)", gen);
    vTaskDelete(NULL);
}

static void refresh_start(void)
{
    if (s_busy) return;
    if (we_portal_running()) {          // 配置后台正占着 Wi-Fi,等它关闭后再刷
        ESP_LOGW(TAG, "配置后台占用 Wi-Fi,跳过本次刷新");
        return;
    }
    s_busy = true;
    s_last_try = lv_tick_get();
    if (xTaskCreate(balance_worker, "balance", REFRESH_STACK, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "刷新任务创建失败");
        s_busy = false;
        return;
    }
}
