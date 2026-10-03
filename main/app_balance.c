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
#include "we_diag.h"       // 每次刷新的诊断快照(门户 /diag 页面读取)
#include "we_hist.h"       // 逐日记账数据模型(NVS 存取;门户备份模块共用)
#include "we_provider.h"   // 平台适配:直连官方余额接口
#include "we_portal.h"     // we_cfg_load / we_cfg_save(NVS 存取)
#include "we_set.h"        // 用户可选项(自动刷新档位 → 毫秒换算)

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_tls.h"       // 网络自检:is_plain_tcp 探测 + 错误句柄取分类码/errno
#include "esp_timer.h"
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
#define BAT_PILL_B_X 174
#define BAT_PILL_B_Y 11
#define BAT_PILL_B_W 50
#define BAT_PILL_B_H 19
// 主页(TOKEN JOURNEY)标题/发丝线/摘要卡
#define TITLE_J_X    14
#define TITLE_J_Y    11
#define TITLE_J_W    130
#define TITLE_J_H    19
#define HAIR_J_X     8
#define HAIR_J_Y     39
#define HAIR_J_W     224
#define HAIR_J_H     1
#define SUM_CARD_X   14
#define SUM_CARD_Y   48
#define SUM_CARD_W   212
#define SUM_CARD_H   36
#define SUM_CAP_X    30           // "30 DAYS" 标题(绝对坐标;卡内相对 = 减 SUM_CARD_XY)
#define SUM_CAP_Y    57
#define SUM_CAP_W    70
#define SUM_CAP_H    19
#define HEAT_SUM_X   142          // 使用天数大字
#define HEAT_SUM_Y   57
#define HEAT_SUM_W   70
#define HEAT_SUM_H   19
// 热力图:单格 + 整区(格区由 HEAT_CELL 按 6×5 + 间距 6 铺满 HEAT_GRID)
#define HEAT_CELL_X  15
#define HEAT_CELL_Y  96
#define HEAT_CELL_W  30
#define HEAT_CELL_H  30
#define HEAT_GRID_X  15
#define HEAT_GRID_Y  96
#define HEAT_GRID_W  210
#define HEAT_GRID_H  174
#define HEAT_TODAY_X 195          // 今天 = 右下角格(第 6 列第 5 行)
#define HEAT_TODAY_Y 240
#define HEAT_TODAY_W 30
#define HEAT_TODAY_H 30
// 图例:LOW/HIGH 两端文字 + 4 个色块(低→高:淡紫→深紫)
#define LEGEND_LO_X  194
#define LEGEND_LO_Y  283
#define LEGEND_LO_W  30
#define LEGEND_LO_H  17
#define LEGEND_SQ1_X 66
#define LEGEND_SQ1_Y 286
#define LEGEND_SQ1_W 12
#define LEGEND_SQ1_H 12
#define LEGEND_SQ2_X 98
#define LEGEND_SQ2_Y 286
#define LEGEND_SQ2_W 12
#define LEGEND_SQ2_H 12
#define LEGEND_SQ3_X 130
#define LEGEND_SQ3_Y 286
#define LEGEND_SQ3_W 12
#define LEGEND_SQ3_H 12
#define LEGEND_SQ4_X 162
#define LEGEND_SQ4_Y 286
#define LEGEND_SQ4_W 12
#define LEGEND_SQ4_H 12
#define LEGEND_HI_X  15
#define LEGEND_HI_Y  283
#define LEGEND_HI_W  30
#define LEGEND_HI_H  17
// 底部翻页点
#define PAGE_DOT1_X  114
#define PAGE_DOT1_Y  304
#define PAGE_DOT1_W  6
#define PAGE_DOT1_H  6
#define PAGE_DOT2_X  126
#define PAGE_DOT2_Y  304
#define PAGE_DOT2_W  6
#define PAGE_DOT2_H  6

// 宏表与派生几何必须自洽(布局编辑器回写时只动宏,公式不动;对不上就编译期炸)
_Static_assert(HEAT_CELL_X == HEAT_X0 && HEAT_CELL_Y == HEAT_Y0 &&
               HEAT_CELL_W == HEAT_S && HEAT_CELL_H == HEAT_S, "heat cell mismatch");
_Static_assert(HEAT_GRID_X == HEAT_X0 && HEAT_GRID_Y == HEAT_Y0 &&
               HEAT_GRID_W == HEAT_COLS * HEAT_S + (HEAT_COLS - 1) * HEAT_G &&
               HEAT_GRID_H == HEAT_ROWS * HEAT_S + (HEAT_ROWS - 1) * HEAT_G, "heat grid mismatch");
_Static_assert(HEAT_TODAY_X == HEAT_X0 + (HEAT_COLS - 1) * (HEAT_S + HEAT_G) &&
               HEAT_TODAY_Y == HEAT_Y0 + (HEAT_ROWS - 1) * (HEAT_S + HEAT_G), "heat today mismatch");
_Static_assert(LEGEND_SQ1_W == 12 && LEGEND_SQ1_H == 12 &&
               LEGEND_SQ2_X - LEGEND_SQ1_X == 32 &&
               LEGEND_SQ3_X - LEGEND_SQ2_X == 32 &&
               LEGEND_SQ4_X - LEGEND_SQ3_X == 32, "legend squares spacing");
_Static_assert(LEGEND_SQ1_Y == LEGEND_SQ2_Y && LEGEND_SQ2_Y == LEGEND_SQ3_Y &&
               LEGEND_SQ3_Y == LEGEND_SQ4_Y && LEGEND_LO_Y == LEGEND_HI_Y &&
               LEGEND_SQ1_Y == 286, "legend rows aligned");
_Static_assert(PAGE_DOT2_X - PAGE_DOT1_X == 12 && PAGE_DOT1_Y == 304, "page dots");

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
// mbedTLS 握手本身就要吃掉几 KB 栈,6KB 在 C3(无 PSRAM)上贴边;抬高一点更稳。
#define REFRESH_STACK 8192
#define FETCH_TRIES   2             // 全部失败时补一轮重试(DNS/TCP 抖动很常见)
#define FETCH_RETRY_MS 1200
#define PROBE_TCP_TIMEOUT_MS 5000   // 网络自检里"只建 TCP"那一步的超时(别等默认 10s+)

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
static lv_timer_t *s_retry_timer;
// 自动刷新间隔(ms);0 = 用户关闭定时刷新(离线重试与手动 OK 刷新不受影响)。
// 值来自 NVS(ns=cfg key=refmin,存分钟),进页面时 auto_refresh_load();没设过 = 60 分钟。
static uint32_t s_auto_ms;

// 本文件写 NVS 的命名空间(与门户配置 we_cfg 共用 "cfg",key 各不相同:
// usdcny=汇率缓存、refmin=自动刷新档位)。放在两个使用点之前。
#define CFG_NS "cfg"

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
    obj_new(scr, HAIR_J_X, HAIR_J_Y, HAIR_J_W, HAIR_J_H, C_HAIR);   // 标题下细分隔线
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
    lv_obj_set_pos(title, TITLE_J_X, TITLE_J_Y);
    lv_label_set_text(title, "TOKEN JOURNEY");
    s_bats[PAGE_JOURNEY] = bat_pill_create(s_jcont, BAT_PILL_J_X, BAT_PILL_J_Y,
                                           BAT_PILL_J_W, BAT_PILL_J_H);

    // 摘要卡:左「30 DAYS」+ 右侧使用天数大字(同字号,卡加高下边框下移)
    lv_obj_t *sum = lv_obj_create(s_jcont);
    lv_obj_remove_flag(sum, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(sum, SUM_CARD_X, SUM_CARD_Y);
    lv_obj_set_size(sum, SUM_CARD_W, SUM_CARD_H);
    lv_obj_set_style_radius(sum, 10, 0);
    lv_obj_set_style_pad_all(sum, 0, 0);
    lv_obj_set_style_border_width(sum, 1, 0);
    lv_obj_set_style_border_color(sum, lv_color_hex(C_PURPLE), 0);
    lv_obj_set_style_bg_color(sum, lv_color_hex(C_CARD), 0);
    lv_obj_t *sumcap = lv_label_create(sum);
    lv_obj_set_style_text_font(sumcap, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(sumcap, lv_color_hex(C_PURPLE), 0);
    lv_obj_set_pos(sumcap, SUM_CAP_X - SUM_CARD_X, SUM_CAP_Y - SUM_CARD_Y);
    lv_label_set_text(sumcap, "30 DAYS");
    s_heat_sum = lv_label_create(sum);
    lv_obj_set_style_text_font(s_heat_sum, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_heat_sum, lv_color_hex(C_TXT_HI), 0);
    lv_label_set_text(s_heat_sum, "--");
    lv_obj_update_layout(s_heat_sum);
    // 右对齐:让字右缘贴绝对坐标 HEAT_SUM_X+HEAT_SUM_W(=卡右缘),换算到卡内相对 x
    lv_obj_set_pos(s_heat_sum, (HEAT_SUM_X + HEAT_SUM_W - SUM_CARD_X) - lv_obj_get_width(s_heat_sum),
                   SUM_CAP_Y - SUM_CARD_Y);

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
    // 垂直:色块(12px)在 286,与 283 起 17px 高的 LOW/HIGH 文字视觉居中
    static const uint32_t lg[4] = { HEAT_LV1, HEAT_LV2, HEAT_LV3, HEAT_LV4 };
    static const int sq_x[4] = { LEGEND_SQ1_X, LEGEND_SQ2_X, LEGEND_SQ3_X, LEGEND_SQ4_X };
    static const int sq_y[4] = { LEGEND_SQ1_Y, LEGEND_SQ2_Y, LEGEND_SQ3_Y, LEGEND_SQ4_Y };
    static const int sq_w[4] = { LEGEND_SQ1_W, LEGEND_SQ2_W, LEGEND_SQ3_W, LEGEND_SQ4_W };
    static const int sq_h[4] = { LEGEND_SQ1_H, LEGEND_SQ2_H, LEGEND_SQ3_H, LEGEND_SQ4_H };
    lv_obj_t *hi = lv_label_create(s_jcont);
    lv_obj_set_style_text_font(hi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hi, lv_color_hex(C_TXT_FAINT), 0);
    lv_label_set_text(hi, "HIGH");
    lv_obj_set_pos(hi, LEGEND_HI_X, LEGEND_HI_Y);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = obj_new(s_jcont, sq_x[i], sq_y[i], sq_w[i], sq_h[i], lg[i]);
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
        lv_obj_t *d = obj_new(s_scr, i == 0 ? PAGE_DOT1_X : PAGE_DOT2_X,
                              i == 0 ? PAGE_DOT1_Y : PAGE_DOT2_Y,
                              i == 0 ? PAGE_DOT1_W : PAGE_DOT2_W,
                              i == 0 ? PAGE_DOT1_H : PAGE_DOT2_H, 0x3A2E6E);
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
    // 右缘贴卡内 (HEAT_SUM_X+HEAT_SUM_W-SUM_CARD_X) 处(=卡右缘,与建卡时的初始定位同源)
    lv_obj_set_pos(s_heat_sum, (HEAT_SUM_X + HEAT_SUM_W - SUM_CARD_X) - lv_obj_get_width(s_heat_sum),
                   SUM_CAP_Y - SUM_CARD_Y);
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

// 自动刷新档位读入:NVS ns=cfg key=refmin(u8 分钟)。没写过/非法 → 默认 60 分钟。
// (0 是合法值 = 用户主动关闭,和"没写过"区分开:没写过时 nvs_get_u8 返回 NOT_FOUND。)
// 与汇率缓存共用 cfg 命名空间(CFG_NS),两个 key 各存各的,互不影响。
#define AUTO_NVS_KEY "refmin"
static void auto_refresh_load(void)
{
    uint32_t ms = we_set_auto_min_ms(WE_SET_AUTO_60);   // 出厂默认
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t m = 0;
        if (nvs_get_u8(h, AUTO_NVS_KEY, &m) == ESP_OK && we_set_auto_min_valid(m))
            ms = we_set_auto_min_ms(m);
        nvs_close(h);
    }
    s_auto_ms = ms;
    ESP_LOGI(TAG, "自动刷新:%s", ms ? "" : "关");
    if (ms) ESP_LOGI(TAG, "  每 %lu 分钟", (unsigned long)(ms / 60000));
}

// 60s 心跳:离线 90s 自动重试 / 在线按用户档位(默认 10min)自动刷新(锁屏或忙碌时跳过)
static void retry_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_scr || s_locked || s_busy) return;
    uint32_t ago = lv_tick_get() - s_last_try;
    if (s_data_ok) {
        // s_auto_ms=0 表示用户关了定时刷新:在线态不再自动刷,只剩离线重试
        if (s_auto_ms != 0 && ago >= s_auto_ms) refresh_start();
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

    // 自动找网心跳(离线 90s 重试 / 在线按档位定时刷新)
    s_data_ok = false;
    s_last_try = 0;
    auto_refresh_load();                 // 档位从 NVS 读(没设过 = 默认 60 分钟)
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
// 把响应体前若干字节收进诊断快照:丢掉控制字符(换行/制表折成空格),
// 其余原样保留(>0x7F 的字节是 UTF-8 中文,不能当垃圾滤掉)。
static void diag_copy_body(we_diag_row_t *dg, const char *body, size_t len)
{
    if (!dg || !body) return;
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < sizeof(dg->body); i++) {
        unsigned char c = (unsigned char)body[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c < 0x20 || c == 0x7F) continue;
        dg->body[o++] = (char)c;
    }
    dg->body[o] = '\0';
}

// esp_ip_addr_t → IPv4 点分字符串(非 IPv4 或空值产出空串)
static void ip4_str(char *dst, size_t cap, const esp_ip_addr_t *a)
{
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!a || a->type != ESP_IPADDR_TYPE_V4) return;
    we_diag_ip4_str(a->u_addr.ip4.addr, dst, cap);
}

// ---------------------------------------------------------------- 网络自检
// 整轮取数全失败后跑一次,目的是把"到底是哪一层断的"钉死。
//
// 为什么不能只看 esp_http_client_open 的返回值:连接阶段失败时它只回一个笼统的
// ESP_ERR_HTTP_CONNECT(0x7002) —— 它的实现(esp_http_client.c 里)是
//     if (esp_transport_connect(...) < 0) return ESP_ERR_HTTP_CONNECT;
// "域名没解析出来" / "TCP 连不上" / "TCP 超时" 全被折叠成同一个值。
//
// 这里换一个方式重跑同一条连接路径:自己持有一个 esp_tls 错误句柄,并设
// is_plain_tcp = true —— 只做 DNS + TCP,不做 TLS 握手。好处有两点:
//   1) 拿到的 0x800x 分类码直接说明断在哪一层(0x8001 DNS / 0x8004 CONN / 0x8006 TOUT);
//   2) 能再从 ESP_TLS_ERR_TYPE_SYSTEM 里单独取到 socket errno(连接被拒/无路由/超时)。
// 全程不碰证书、不握手、不申请大缓冲,所以也是一次内存低压探测,
// 不会把"内存不够"和"TLS 被拒"两件事混在一起。
static void net_probe(we_diag_net_t *np, const char *host)
{
    memset(np, 0, sizeof(*np));
    np->probed = true;
    if (host) snprintf(np->host, sizeof(np->host), "%s", host);

    // 1) 接口状态:拿到 IP 却没拿到 DNS,是"能连上 WiFi 但什么都解析不出来"的典型病因
    esp_netif_t *nf = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nf) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(nf, &ip) == ESP_OK) {
            snprintf(np->ip, sizeof(np->ip), IPSTR, IP2STR(&ip.ip));
            snprintf(np->gw, sizeof(np->gw), IPSTR, IP2STR(&ip.gw));
        }
        esp_netif_dns_info_t d;
        if (esp_netif_get_dns_info(nf, ESP_NETIF_DNS_MAIN, &d) == ESP_OK)
            ip4_str(np->dns1, sizeof(np->dns1), &d.ip);
        if (esp_netif_get_dns_info(nf, ESP_NETIF_DNS_BACKUP, &d) == ESP_OK)
            ip4_str(np->dns2, sizeof(np->dns2), &d.ip);
    }

    if (!np->host[0]) return;                      // 没有可探测的域名

    // 2) 只建 TCP 的连接探测
    esp_tls_t *tls = esp_tls_init();
    if (!tls) { np->rc = 0x8002; return; }         // 连探测对象都建不起来 = 已无内存

    esp_tls_cfg_t cfg = { 0 };
    cfg.timeout_ms   = PROBE_TCP_TIMEOUT_MS;
    cfg.is_plain_tcp = true;
    cfg.addr_family  = ESP_TLS_AF_INET;            // 固定 IPv4,免得卡在 IPv6 解析上

    int64_t t0 = esp_timer_get_time();
    int r = esp_tls_conn_new_sync(np->host, (int)strlen(np->host), 443, &cfg, tls);
    np->ms = (int)((esp_timer_get_time() - t0) / 1000);

    esp_tls_error_handle_t eh = NULL;
    if (esp_tls_get_error_handle(tls, &eh) == ESP_OK && eh) {
        int code = 0, flags = 0;
        // 同一个坑:分类码是返回值,不是出参;而且读完即清,只能取一次。
        np->rc = (int)esp_tls_get_and_clear_last_error(eh, &code, &flags);
        // socket errno 存在 last_error 结构体之外(memset 清不到),所以可以后取。
        esp_tls_get_and_clear_error_type(eh, ESP_TLS_ERR_TYPE_SYSTEM, &np->sock_errno);
    }
    esp_tls_conn_destroy(tls);
    if (r > 0) np->rc = 0;                         // 连上了:清掉任何残留码
}

// ---------------------------------------------------------------- 美元→人民币折算
// OpenRouter 等美元平台的余额,取回后统一折成人民币再进显示/记账,
// 这样总余额、今日花费、30 天热力都是同一币种,可直接相加比较。
//
// 汇率来源三级回退,取到即写 NVS 缓存(ns=cfg key=usdcny,u32 = 汇率×1000):
//   1) 在线:frankfurter.dev(响应 70B)→ open.er-api.com(响应 ~3KB)
//   2) NVS 缓存(上次成功取到的值)
//   3) 兜底常量 6.800(汇率多年在 6.3~7.3 区间,量级不会错)
// 折算只影响"美元平台"的数值;记账基线也存折算后的人民币,所以平台余额不变、
// 只有汇率波动时,当日"花费"会出现 余额×汇率变化 量级的微小伪差(美分级别),
// 这是单币种记账模型的固有代价,换取三平台可直接合计,值得。
#define RATE_KEY       "usdcny"          // u32:USD/CNY × 1000(命名空间用 CFG_NS)
#define RATE_DEFAULT   6.800             // 最后兜底
#define RATE_LIVE_MAX  2                 // 两个在线源

static const char *RATE_URLS[RATE_LIVE_MAX] = {
    "https://api.frankfurter.dev/v1/latest?base=USD&symbols=CNY",
    "https://open.er-api.com/v6/latest/USD",
};

// 已配置平台里是否有美元计价平台(有才值得花一次请求取汇率)
static bool any_usd_provider(const we_cfg_t *cfg)
{
    for (uint8_t i = 0; i < cfg->prov_count; i++) {
        const we_provider_t *p = NULL;
        if (we_provider_lookup(cfg->prov[i].provider_id, &p) && p &&
            p->currency && strcmp(p->currency, "USD") == 0) return true;
    }
    return false;
}

static double rate_cache_read(void)     // 无缓存返回 -1
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    uint32_t milli = 0;
    esp_err_t err = nvs_get_u32(h, RATE_KEY, &milli);
    nvs_close(h);
    if (err != ESP_OK || milli == 0) return -1;
    return (double)milli / 1000.0;
}

static void rate_cache_write(double rate)
{
    if (rate <= 0) return;
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t milli = (uint32_t)(rate * 1000.0 + 0.5);
    if (nvs_set_u32(h, RATE_KEY, milli) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

// 必须在 Wi-Fi 已连接时调用。buf 复用取数响应缓冲(两个响应都 <4KB)。
// 成功返回汇率并写缓存;两个源都失败返回 -1。
static double fetch_usd_rate(char *buf, size_t cap)
{
    for (int i = 0; i < RATE_LIVE_MAX; i++) {
        esp_http_client_config_t hcfg = {
            .url = RATE_URLS[i],
            .method = HTTP_METHOD_GET,
            .timeout_ms = HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = false,
        };
        esp_http_client_handle_t cl = esp_http_client_init(&hcfg);
        if (!cl) continue;
        double rate = -1;
        if (esp_http_client_open(cl, 0) == ESP_OK) {
            esp_http_client_fetch_headers(cl);
            if (esp_http_client_get_status_code(cl) == 200) {
                size_t used = 0;
                int r;
                while (used + 1 < cap &&
                       (r = esp_http_client_read(cl, buf + used, cap - used - 1)) > 0) {
                    used += (size_t)r;
                }
                buf[used] = '\0';
                if (!we_rate_parse(buf, &rate)) rate = -1;
            }
            esp_http_client_close(cl);
        } else {
            // 别让这个辅助请求的错误污染下一张平台卡的诊断快照:读完即清
            int c = 0, f = 0;
            (void)esp_http_client_get_and_clear_last_tls_error(cl, &c, &f);
        }
        esp_http_client_cleanup(cl);
        if (rate > 0) {
            rate_cache_write(rate);
            ESP_LOGI(TAG, "汇率 %f ← %s", rate, RATE_URLS[i]);
            return rate;
        }
        ESP_LOGW(TAG, "汇率源[%d] 失败", i);
    }
    return -1;
}

// dg 可为 NULL。失败时回填:open_err(0x7000 段)、tls_err(0x8000 段)、mbedTLS 码、
// 证书校验标志、HTTP 状态码、响应体预览 —— 屏幕短码由 we_diag_tag_from_pair 决定。
static int https_fetch_provider(const we_provider_t *prov, const char *key,
                                char *out, size_t cap, we_diag_row_t *dg)
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
        int tls_err = 0, tls_code = 0, tls_flags = 0;
        // 这里的返回值不是"成功/失败",它本身就是 esp_tls 的分类码(0x8000 段)。
        // esp_tls_get_and_clear_last_error() 的实现是:
        //     esp_err_t last_err = h->last_error;      // ← 走返回值
        //     *esp_tls_code  = h->esp_tls_error_code;  // ← 出参只是 mbedTLS 码
        //     *esp_tls_flags = h->esp_tls_flags;
        //     memset(h, 0, sizeof(esp_tls_last_error_t));   // 读完即清,只能取一次
        // 也就是说:真出问题时它"非 0",而 0 才代表没有记录。必须原样接住,
        // 不能拿 != ESP_OK 当成失败标志去清零(那正好把唯一有用的信息抹掉)。
        tls_err = (int)esp_http_client_get_and_clear_last_tls_error(cl, &tls_code, &tls_flags);
        if (dg) {
            dg->open_err  = (int)err;
            dg->tls_err   = tls_err;
            dg->tls_code  = tls_code;
            dg->tls_flags = tls_flags;
            // open_err 在连接阶段只会是笼统的 ESP_ERR_HTTP_CONNECT(0x7002),
            // 底层原因在 tls_err 里,由它优先。
            we_diag_tag_from_pair((int)err, tls_err, dg->tag, sizeof(dg->tag));
        }
        ESP_LOGW(TAG, "HTTPS open %s 失败: %s open_err=0x%X tls_err=0x%X mbed=%d flags=0x%X",
                 prov->url, esp_err_to_name(err), (unsigned)err, (unsigned)tls_err,
                 tls_code, (unsigned)tls_flags);
        esp_http_client_cleanup(cl);
        return -2;
    }
    esp_http_client_fetch_headers(cl);
    int status = esp_http_client_get_status_code(cl);
    if (dg) dg->http_status = status;
    ESP_LOGI(TAG, "HTTPS %s 状态=%d", prov->id, status);

    size_t used = 0;
    int r;
    while (used + 1 < cap &&
           (r = esp_http_client_read(cl, out + used, cap - used - 1)) > 0) {
        used += (size_t)r;
    }
    out[used] = '\0';
    if (dg) {
        dg->body_len = (int)used;
        diag_copy_body(dg, out, used);
    }
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
    // 多备两台:某些路由器/园区网会拦 UDP 123,或让某台 NTP 不可达。
    // 台数上限由 CONFIG_LWIP_SNTP_MAX_SERVERS 决定(默认 1,本项目设为 3)。
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    esp_sntp_setservername(1, "cn.pool.ntp.org");
#endif
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 2
    esp_sntp_setservername(2, "time1.cloud.tencent.com");
#endif
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

// 把堆统计和联网结果记进诊断快照(TLS 握手最吃"最大连续块",C3 无 PSRAM 要盯着)
static void diag_env(int wifi_rc, bool clock_ok)
{
    we_diag_set_env(wifi_rc, clock_ok,
                    (uint32_t)esp_get_free_heap_size(),
                    (uint32_t)esp_get_minimum_free_heap_size(),
                    (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
}

static void balance_worker(void *arg)
{
    (void)arg;
    int gen = s_gen;
    we_diag_reset();
    we_diag_set_stage("CONFIG");

    // 1. 状态提示
    if (!s_cancel) {
        bsp_lvgl_lock(1000);
        set_state("REFRESHING...");
        bsp_lvgl_unlock();
    }

    // 2. NVS + 社区配置(必须先初始化 NVS,示例封装幂等可重复调)
    if (demo_radio_nvs_prepare() != ESP_OK) {
        we_diag_set_stage("NVS");
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
        we_diag_set_stage("NO WIFI");
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
        we_diag_set_stage("NO KEY");
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
    we_diag_set_stage("WIFI");
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
        we_diag_set_stage("WIFI FAIL");
        diag_env(rc, false);
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
    if (s_cancel) {
        we_diag_set_stage("CANCEL");
        wifi_teardown(); s_busy = false; vTaskDelete(NULL); return;
    }

    // 4. 直连官方余额(只查已配置平台),同时收实时余额做本地记账
    //
    // 先把本地时间校好再发请求。ESP-IDF 默认不开证书有效期校验
    // (CONFIG_MBEDTLS_HAVE_TIME_DATE=n),所以这一步不是 TLS 的前提;但记账天界
    // 和 UPDATED 行都依赖本地时间,而且校时本身也顺带证明 DNS/UDP 通不通。
    we_diag_set_stage("SNTP");
    ensure_sntp();                       // 直连需要本地时间(更新时间/记账天界)
    wait_sntp(SNTP_WAIT_MS);
    diag_env(0, time_synced());
    we_diag_set_stage("FETCH");
    int n = s_cfg.prov_count;
    if (n > ROW_COUNT) n = ROW_COUNT;
    float cur[ROW_COUNT] = { 0 };
    bool  okr[ROW_COUNT] = { false };
    int   n_ok = 0;
    double total = 0;

    // 美元平台存在 → 先把汇率备好(在线 → NVS 缓存 → 兜底常量)。
    // 之后所有金额(显示/记账/热力)统一按人民币处理,下游不再区分币种。
    double usdcny = 0;
    if (any_usd_provider(&s_cfg)) {
        static char rbuf[RESP_MAX];
        we_diag_set_stage("RATE");
        double live = s_cancel ? -1 : fetch_usd_rate(rbuf, sizeof(rbuf));
        if (live > 0) {
            usdcny = live;
            we_diag_set_rate((int)(live * 1000.0 + 0.5), "live");
        } else {
            double cached = rate_cache_read();
            if (cached > 0) {
                usdcny = cached;
                we_diag_set_rate((int)(cached * 1000.0 + 0.5), "cache");
            } else {
                usdcny = RATE_DEFAULT;
                we_diag_set_rate((int)(RATE_DEFAULT * 1000.0 + 0.5), "default");
            }
            ESP_LOGW(TAG, "汇率走 %s 路径: %f", cached > 0 ? "缓存" : "兜底", usdcny);
        }
        we_diag_set_stage("FETCH");
    }

    if (!s_cancel) {
        bsp_lvgl_lock(1000);
        for (int i = 0; i < n; i++) { dot_set(i, C_DOT_GRAY); amount_set(i, "--"); }
        bsp_lvgl_unlock();
    }

    // 逐平台取数;若整轮一个都没成功(DNS/握手抖动很常见),补跑一轮再判定失败。
    for (int attempt = 0; attempt < FETCH_TRIES && !s_cancel; attempt++) {
        if (attempt > 0) {
            we_diag_set_stage("RETRY");
            ESP_LOGW(TAG, "首轮全灭,%d ms 后重试一轮", FETCH_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(FETCH_RETRY_MS));
        }
        for (int k = 0; k < n && !s_cancel; k++) {
            if (okr[k]) continue;                   // 重试只补失败的行
            const we_provider_t *prov = NULL;
            if (!we_provider_lookup(s_cfg.prov[k].provider_id, &prov)) continue;

            we_diag_row_t dr;
            memset(&dr, 0, sizeof(dr));
            dr.http_status = -1;
            dr.attempts    = attempt + 1;
            snprintf(dr.id, sizeof(dr.id), "%s", prov->id);

            // 老 NVS 里可能存着带空白/换行的 Key(粘贴进来的),发请求前再洗一遍。
            char kbuf[WE_CFG_MAX_KEY];
            if (!we_cfg_sanitize_key(s_cfg.prov[k].api_key, kbuf, sizeof(kbuf)))
                kbuf[0] = '\0';

            static char dresp[RESP_MAX];
            bool   ok = false;
            double v  = 0;
            int    gotd = -3;
            int64_t t0 = esp_timer_get_time();
            if (kbuf[0]) {
                gotd = https_fetch_provider(prov, kbuf, dresp, sizeof(dresp), &dr);
                if (gotd > 0) {
                    char amt[24];
                    ok = we_provider_parse(prov->id, dresp, amt, sizeof(amt));
                    if (ok) {
                        // 折成人民币:美元 × 汇率,其余原样。下游(显示/记账/热力)只见 CNY
                        v = we_provider_to_cny(prov->currency, strtod(amt, NULL), usdcny);
                    }
                }
                // 打开成功才用状态码/解析结果定码;打开失败时上面已填好 DNS/TLS/CONN
                if (dr.open_err == 0)
                    we_diag_tag_from_status(dr.http_status, ok, dr.tag, sizeof(dr.tag));
            } else {
                snprintf(dr.tag, sizeof(dr.tag), "NOKEY");
            }
            dr.millis = (int)((esp_timer_get_time() - t0) / 1000);
            we_diag_set_row(k, &dr);

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
                    // 失败时把病因短码顶到金额位置:DOMAIN/TLS/401/JSON 一眼可辨
                    amount_set(k, dr.tag[0] ? dr.tag : "---");
                }
                bsp_lvgl_unlock();
            }
            ESP_LOGI(TAG, "direct %s got=%d ok=%d tag=%s %dms",
                     prov->id, gotd, ok, dr.tag, dr.millis);
        }
        if (n_ok > 0) break;                        // 有平台成功就不再来一轮
    }

    // 整轮全灭 → 立刻做一次独立探测,把"哪一层断了"钉死。
    // 必须在 wifi_teardown() 之前:接口一拆,探测就没有意义了。
    if (!s_cancel && n_ok == 0 && n > 0) {
        we_diag_set_stage("PROBE");
        const we_provider_t *p0 = NULL;
        const char *phost = NULL;
        if (we_provider_lookup(s_cfg.prov[0].provider_id, &p0) && p0) phost = p0->host;
        static we_diag_net_t np;                    // 不放栈上:本任务栈只有 REFRESH_STACK
        net_probe(&np, phost);
        we_diag_set_net(&np);
        diag_env(0, time_synced());                 // 重取一次堆:顺便看探测本身花了多少

        // 兜底码 0x7002 说不出是 DNS 还是 TCP,自检能。只在没有更权威结论时才细化:
        //   0x8001           → 域名根本没解析出来
        //   0x8004 / 0x8006  → 域名解析了但 TCP 连不上,再用 errno 说清是被拒还是超时
        if (np.rc != 0) {
            char rt[WE_DIAG_TAG_MAX];
            char et[WE_DIAG_TAG_MAX];
            const char *use = rt;
            we_diag_tag_from_err(np.rc, rt, sizeof(rt));
            if ((np.rc == 0x8004 || np.rc == 0x8006) && np.sock_errno != 0) {
                we_diag_tag_from_errno(np.sock_errno, et, sizeof(et));
                use = et;
            }
            for (int i = 0; i < n && i < WE_DIAG_ROWS; i++)
                we_diag_refine_conn_tag(i, use);
        }
        ESP_LOGW(TAG, "网络自检 host=%s ip=%s gw=%s dns=%s/%s rc=0x%X errno=%d %dms",
                 np.host, np.ip, np.gw, np.dns1, np.dns2,
                 (unsigned)np.rc, np.sock_errno, np.ms);
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
            // 屏幕位置太窄,放不下完整原因:这里给出第一个失败平台的短码,
            // 逐平台的完整信息(err/mbedTLS 码/证书标志/HTTP 状态/响应体)
            // 在门户 /diag 页面上。
            const we_diag_t *d = we_diag_get();
            const char *tag = "?";
            for (int i = 0; i < n && i < WE_DIAG_ROWS; i++) {
                if (d->row[i].tag[0]) { tag = d->row[i].tag; break; }
            }
            char msg[26];
            snprintf(msg, sizeof(msg), "DIRECT FAIL %.11s", tag);
            set_state(msg);
            s_data_ok = false;
        }
        bsp_lvgl_unlock();
    }

    we_diag_finish(n_ok, n);
    we_diag_set_stage(s_cancel ? "CANCEL" : (n_ok > 0 ? "DONE" : "FAIL"));

    wifi_teardown();                 // 取数结束,关 Wi-Fi 省电并释放资源
    diag_env(0, time_synced());      // 收尾再采一次堆(能看出 TLS 握手吃掉的峰值)

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
