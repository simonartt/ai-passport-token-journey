// main/we_portal.c — 无云依赖的本地配置门户。
// 联网策略:优先用 we_cfg 里的 WiFi 档案接入家庭局域网(手机同网段即可访问),
// 全部档案失败或尚未配网时回落 SoftAP 热点兜底,保证任何状态下都进得去后台。
// 数据流仍是:表单 → we_cfg blob(NVS)。
#include "we_portal.h"

#include "demo_radio.h"   // demo_radio_nvs_prepare / demo_radio_network_prepare(幂等初始化)
#include "we_backup.h"    // 配置 + 历史整份导出/恢复(GET /backup、POST /restore)
#include "we_cfg.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "we_portal";

#define PORTAL_SSID_PREFIX  "BAL-"
#define PORTAL_PASSWORD     "12345678"       // 同时用作 AP 热点密码与后台访问密码
#define PORTAL_USER         "admin"          // HTTP Basic Auth 用户名
#define PORTAL_HOSTNAME     "token-journey"  // mDNS 短名 → http://token-journey.local/
#define PORTAL_STA_WAIT_MS  20000            // 单组 WiFi 档案的连接等待上限
#define PORTAL_HTTP_PORT    80
#define FORM_BODY_MAX       4096
#define RESTORE_BODY_MAX    16384            // 备份 JSON 上限(实测约 2~3KB,留足余量)
#define CFG_NVS_NS          "cfg"
#define CFG_NVS_KEY         "main"

// STA 连接结果位
#define BIT_IP   BIT0
#define BIT_FAIL BIT1

static httpd_handle_t s_server;
static esp_netif_t *s_netif;          // 当前生效的 netif(STA 或 AP)
static bool s_netif_is_ap;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_mdns_ready;
static char s_ssid[33] = "";          // 仅 AP 模式有值
static char s_ip[20] = "";            // 点分十进制,不含协议
static char s_url[40] = "";           // "http://x.x.x.x/"
static char s_auth_b64[64] = "";      // base64("admin:12345678"),启动时算一次
static volatile we_portal_mode_t s_mode = WE_PORTAL_OFF;
static volatile bool s_stop_req;
static TaskHandle_t s_task;
static EventGroupHandle_t s_ev;
static esp_event_handler_instance_t s_h_wifi, s_h_ip;
static bool s_hw_reg, s_hip_reg;
static volatile TickType_t s_last_tick;   // 最近一次 HTTP 请求时刻(空闲超时用)

// ---------------------------------------------------------------- 工具
static void text_copy(char *dst, size_t len, const char *src)
{
    if (!dst || len == 0) return;
    src = src ? src : "";
    size_t out = 0;
    while (*src && out + 1 < len) {
        unsigned char first = (unsigned char)*src;
        size_t bytes = first < 0x80 ? 1 :
                       (first & 0xE0) == 0xC0 ? 2 :
                       (first & 0xF0) == 0xE0 ? 3 :
                       (first & 0xF8) == 0xF0 ? 4 : 1;
        if (out + bytes >= len) break;
        bool valid = true;
        for (size_t i = 1; i < bytes; i++) {
            if (!src[i] || ((unsigned char)src[i] & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }
        if (!valid) { src++; continue; }
        memcpy(dst + out, src, bytes);
        out += bytes;
        src += bytes;
    }
    dst[out] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t out = 0;
    for (size_t i = 0; i < src_len && out + 1 < dst_len; i++) {
        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && i + 2 < src_len) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi >= 0 && lo >= 0) { dst[out++] = (char)((hi << 4) | lo); i += 2; }
            else dst[out++] = src[i];
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    if (!body || !key || !out || out_len == 0) return false;
    size_t key_len = strlen(key);
    const char *cursor = body;
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        if (!end) end = cursor + strlen(cursor);
        const char *eq = memchr(cursor, '=', (size_t)(end - cursor));
        if (eq && (size_t)(eq - cursor) == key_len &&
            memcmp(cursor, key, key_len) == 0) {
            url_decode(out, out_len, eq + 1, (size_t)(end - eq - 1));
            return true;
        }
        cursor = *end ? end + 1 : end;
    }
    out[0] = '\0';
    return false;
}

static void send_html_escaped(httpd_req_t *req, const char *text)
{
    const char *cursor = text ? text : "";
    while (*cursor) {
        const char *escaped = NULL;
        switch (*cursor) {
        case '&': escaped = "&amp;"; break;
        case '<': escaped = "&lt;"; break;
        case '>': escaped = "&gt;"; break;
        case '"': escaped = "&quot;"; break;
        default: break;
        }
        if (escaped) {
            httpd_resp_sendstr_chunk(req, escaped);
        } else {
            char one[2] = { *cursor, '\0' };
            httpd_resp_sendstr_chunk(req, one);
        }
        cursor++;
    }
}

// ---------------------------------------------------------------- NVS 存取
bool we_cfg_save(const we_cfg_t *cfg)
{
    if (!we_cfg_validate(cfg)) return false;
    if (demo_radio_nvs_prepare() != ESP_OK) return false;

    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, CFG_NVS_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool we_cfg_load(we_cfg_t *cfg)
{
    if (!cfg) return false;
    we_cfg_init(cfg);
    if (demo_radio_nvs_prepare() != ESP_OK) return false;

    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(*cfg);
    esp_err_t err = nvs_get_blob(h, CFG_NVS_KEY, cfg, &len);
    nvs_close(h);
    return err == ESP_OK && we_cfg_validate(cfg);
}

// ---------------------------------------------------------------- 访问认证
// 局域网模式下门户暴露给整个家庭网络(含访客设备),而它能改配置、日后还能导出明文
// API Key,因此统一加 HTTP Basic Auth。base64 手写,免得为一行编码再拉依赖。
static void b64_encode(const char *in, char *out, size_t out_len)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), o = 0;
    for (size_t i = 0; i < n && o + 5 < out_len; i += 3) {
        unsigned v = (unsigned)(unsigned char)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)(unsigned char)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)(unsigned char)in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? T[v & 63] : '=';
    }
    out[o] = '\0';
}

static bool portal_auth_ok(httpd_req_t *req)
{
    s_last_tick = xTaskGetTickCount();       // 任何请求都算"有人在用",刷新空闲计时
    if (!s_auth_b64[0]) return true;         // 未初始化则不拦(正常流程不会走到)
    char hdr[96];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    if (strncmp(hdr, "Basic ", 6) != 0) return false;
    return strcmp(hdr + 6, s_auth_b64) == 0;
}

static esp_err_t portal_ask_auth(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Token Journey\"");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>需要密码</title></head><body style=\"background:#0b1020;color:#e6ecff;"
        "font:15px sans-serif;text-align:center;padding-top:80px\">"
        "<p style=\"font-size:19px\">需要访问密码</p>"
        "<p style=\"color:#8fa1af\">用户名 admin,密码见设备屏幕</p></body></html>");
    return ESP_OK;
}

// ---------------------------------------------------------------- 页面
static esp_err_t root_get(httpd_req_t *req)
{
    if (!portal_auth_ok(req)) return portal_ask_auth(req);

    we_cfg_t cfg;
    if (!we_cfg_load(&cfg)) we_cfg_init(&cfg);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Token 余额看板 · 配置</title><style>"
        "*{box-sizing:border-box}body{margin:0;background:#0b1020;color:#e6ecff;"
        "font:15px/1.55 -apple-system,BlinkMacSystemFont,'PingFang SC',sans-serif}"
        "header{border-top:4px solid #a855f7;border-bottom:1px solid #24345c;padding:18px 16px}"
        "main{max-width:680px;margin:auto;padding:0 16px 40px}"
        "h1{font-size:21px;margin:0}h2{font-size:16px;color:#c084fc;margin:22px 0 6px}"
        ".sub{color:#8fa1af}.row{display:grid;gap:6px;padding:8px 0;border-bottom:1px solid #1d2b4d}"
        "label{color:#aeb9d8;font-size:13px}input{width:100%;background:#131c38;color:#e6ecff;"
        "border:1px solid #2e3f6b;border-radius:6px;padding:10px;font-size:15px}"
        ".hint{color:#7c8ab5;font-size:12px}.btn{display:block;width:100%;background:#a855f7;"
        "color:#fff;border:0;border-radius:8px;padding:13px;font-size:16px;font-weight:700;"
        "margin-top:22px;text-align:center;text-decoration:none}"
        ".btn2{background:#24345c;color:#c7d2fe;margin-top:10px}"
        ".ok{color:#34d399;font-weight:700}</style></head><body>"
        "<header><h1>Token 余额看板</h1><div class=\"sub\">本地配置 · 数据仅设备直连各平台</div></header><main>"
        "<form method=\"post\" action=\"/save\" id=\"cfg\">"
        "<h2>WiFi(设备取数用)</h2>"
        "<div class=\"row\"><label>WiFi 名称(SSID)</label><input name=\"ssid\" maxlength=\"32\" value=\"");
    for (uint8_t i = 0; i < cfg.wifi_count; i++) {
        send_html_escaped(req, cfg.wifi[i].ssid);
        break;   // 表单先回填第一组
    }
    httpd_resp_sendstr_chunk(req,
        "\"></div><div class=\"row\"><label>WiFi 密码</label>"
        "<input type=\"password\" name=\"pass\" maxlength=\"64\"></div>"
        "<h2>平台 API Key(留空=不使用该平台)</h2>"
        "<div class=\"row\"><label>DeepSeek Key</label><input name=\"key_deepseek\" maxlength=\"95\" placeholder=\"sk-...\"></div>"
        "<div class=\"row\"><label>Kimi(Moonshot)Key</label><input name=\"key_kimi\" maxlength=\"95\" placeholder=\"sk-...\"></div>"
        "<h2>锁屏签名(可选,空=不显示)</h2>"
        "<div class=\"row\"><label>昵称</label><input name=\"nickname\" maxlength=\"23\" value=\"");
    send_html_escaped(req, cfg.nickname);
    httpd_resp_sendstr_chunk(req,
        "\"></div>"
        "<button class=\"btn\" type=\"submit\">保存并重启</button>"
        "</form><p class=\"hint\">保存后设备会重启;密钥只存在设备 NVS,丢失设备即泄露 Key,建议用只读/低配额 Key。</p>"
        "<h2>备份与恢复</h2>"
        "<p class=\"hint\">备份含明文 API Key 与 30 天消耗记录,请只存放在自己的设备上,不要外传。</p>"
        "<a class=\"btn btn2\" href=\"/backup\" download>下载备份文件(.json)</a>"
        "<div class=\"row\"><label>从备份文件恢复</label>"
        "<input type=\"file\" id=\"bkfile\" accept=\".json,application/json\"></div>"
        "<button class=\"btn btn2\" type=\"button\" id=\"bkgo\">上传并恢复(会重启)</button>"
        "<p class=\"hint\" id=\"bkmsg\"></p>"
        "<script>"
        "var bkf=document.getElementById('bkfile'),bkm=document.getElementById('bkmsg');"
        "document.getElementById('bkgo').onclick=function(){"
        "if(!bkf.files||!bkf.files[0]){bkm.textContent='请先选择备份文件';return;}"
        "bkm.textContent='正在上传…';"
        "var rd=new FileReader();"
        "rd.onload=function(){"
        "fetch('/restore',{method:'POST',headers:{'Content-Type':'application/json'},body:rd.result})"
        ".then(function(x){return x.text();})"
        ".then(function(t){document.open();document.write(t);document.close();})"
        ".catch(function(e){bkm.textContent='上传失败:'+e;});"
        "};"
        "rd.readAsText(bkf.files[0]);"
        "};"
        "</script>"
        "</main></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (!portal_auth_ok(req)) return portal_ask_auth(req);

    if (req->content_len <= 0 || req->content_len >= FORM_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_FAIL;
    }
    char *body = malloc((size_t)req->content_len + 1);
    if (!body) return httpd_resp_send_500(req);
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) { free(body); return ESP_FAIL; }
        received += n;
    }
    body[received] = '\0';

    char v[160];
    we_cfg_t cfg;
    we_cfg_init(&cfg);

    if (form_get(body, "ssid", v, sizeof(v)) && v[0]) {
        char pass[96] = "";
        form_get(body, "pass", pass, sizeof(pass));
        we_cfg_add_wifi(&cfg, v, pass, "");
    }
    if (form_get(body, "key_deepseek", v, sizeof(v)) && v[0])
        we_cfg_add_prov(&cfg, "deepseek", v, "");
    if (form_get(body, "key_kimi", v, sizeof(v)) && v[0])
        we_cfg_add_prov(&cfg, "kimi", v, "");
    if (form_get(body, "nickname", v, sizeof(v)))
        text_copy(cfg.nickname, sizeof(cfg.nickname), v);

    free(body);

    if (!we_cfg_validate(&cfg)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "配置不完整:至少一组 WiFi 或一个平台 Key");
        return ESP_FAIL;
    }

    if (!we_cfg_save(&cfg)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "配置已保存:wifi=%d prov=%d nickname=%s,准备重启",
             cfg.wifi_count, cfg.prov_count, cfg.nickname[0] ? cfg.nickname : "(空)");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width\"><body style=\"background:#0b1020;color:#e6ecff;"
        "font:15px sans-serif;text-align:center;padding-top:80px\">"
        "<p class=\"ok\" style=\"color:#34d399;font-size:20px\">保存成功</p>"
        "<p>设备即将重启…</p></body></html>");

    // 400ms 后重启,让响应先发完
    xTaskCreate(restart_task, "we_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ---------------------------------------------------------------- 备份 / 恢复
// 导出:配置(nickname/WiFi/平台 Key)+ 本地逐日历史,整份 JSON 一次下载。
// 内容含明文 Key,所以和 / 一样必须过认证;页面侧也给了"勿外传"提示。
static esp_err_t backup_get(httpd_req_t *req)
{
    if (!portal_auth_ok(req)) return portal_ask_auth(req);

    char *json = we_backup_export();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "导出失败");
        return ESP_FAIL;
    }
    size_t len = strlen(json);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"token-journey-backup.json\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, (ssize_t)len);
    free(json);
    ESP_LOGI(TAG, "已导出备份 %u 字节", (unsigned)len);
    return err;
}

// 导入:整份覆盖 NVS 里的配置与历史,然后重启(app_balance 内存里还留着旧值)。
static esp_err_t restore_post(httpd_req_t *req)
{
    if (!portal_auth_ok(req)) return portal_ask_auth(req);

    if (req->content_len <= 0 || req->content_len >= RESTORE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "备份文件为空或过大");
        return ESP_FAIL;
    }
    char *body = malloc((size_t)req->content_len + 1);
    if (!body) return httpd_resp_send_500(req);
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "接收中断");
            return ESP_FAIL;
        }
        received += n;
    }
    body[received] = '\0';

    char msg[192] = "";
    esp_err_t err = we_backup_restore(body, msg, sizeof(msg));
    free(body);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>恢复备份</title></head><body style=\"background:#0b1020;color:#e6ecff;"
        "font:15px/1.6 sans-serif;max-width:620px;margin:auto;padding:32px 16px\">");
    if (err == ESP_OK) {
        httpd_resp_sendstr_chunk(req,
            "<p style=\"color:#34d399;font-size:20px;font-weight:700\">恢复成功</p><p>");
        send_html_escaped(req, msg);
        httpd_resp_sendstr_chunk(req,
            "</p><p>设备即将重启…</p><p style=\"color:#8fa1af;font-size:13px\">"
            "重启后回到 Portal 即是恢复后的配置。</p>");
    } else {
        httpd_resp_sendstr_chunk(req,
            "<p style=\"color:#f87171;font-size:20px;font-weight:700\">恢复失败</p><p>");
        send_html_escaped(req, msg);
        httpd_resp_sendstr_chunk(req,
            "</p><p><a href=\"/\" style=\"color:#c084fc\">返回配置页</a></p>");
    }
    httpd_resp_sendstr_chunk(req, "</body></html>");
    esp_err_t sent = httpd_resp_sendstr_chunk(req, NULL);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "备份恢复完成,准备重启");
        xTaskCreate(restart_task, "we_restart", 2048, NULL, 5, NULL);
    }
    return sent;
}

static esp_err_t register_handlers(void)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get },
        { .uri = "/save", .method = HTTP_POST, .handler = save_post },
        { .uri = "/backup", .method = HTTP_GET, .handler = backup_get },
        { .uri = "/restore", .method = HTTP_POST, .handler = restore_post },
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(s_server, &handlers[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- 网络层
static void portal_fill_ip(void)
{
    esp_netif_ip_info_t info = { 0 };
    s_ip[0] = '\0';
    s_url[0] = '\0';
    if (s_netif && esp_netif_get_ip_info(s_netif, &info) == ESP_OK && info.ip.addr != 0)
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&info.ip));
    else if (s_netif_is_ap)
        snprintf(s_ip, sizeof(s_ip), "192.168.4.1");
    if (s_ip[0]) snprintf(s_url, sizeof(s_url), "http://%s/", s_ip);
}

// 仅在真正接入局域网时启用 mDNS;AP 模式没有意义。
static void portal_mdns_start(void)
{
    if (s_mdns_ready || s_netif_is_ap) return;
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS 初始化失败,只能用 IP 访问");
        return;
    }
    mdns_hostname_set(PORTAL_HOSTNAME);
    mdns_instance_name_set("Token Journey Portal");
    mdns_service_add(NULL, "_http", "_tcp", PORTAL_HTTP_PORT, NULL, 0);
    s_mdns_ready = true;
}

// 回收本轮网络资源;可重复调用(各字段用完即置空)。
static void portal_net_down(void)
{
    if (s_mdns_ready) { mdns_free(); s_mdns_ready = false; }
    if (s_ev) { vEventGroupDelete(s_ev); s_ev = NULL; }
    if (s_hip_reg) { esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip); s_hip_reg = false; }
    if (s_hw_reg)  { esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi); s_hw_reg = false; }
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
    if (s_wifi_started) { esp_wifi_stop(); s_wifi_started = false; }
    if (s_wifi_initialized) { esp_wifi_deinit(); s_wifi_initialized = false; }
    if (s_netif) { esp_netif_destroy_default_wifi(s_netif); s_netif = NULL; }
    s_netif_is_ap = false;
    s_ip[0] = '\0';
    s_url[0] = '\0';
    s_ssid[0] = '\0';
}

static void portal_wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_ev) xEventGroupSetBits(s_ev, BIT_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_ev) xEventGroupSetBits(s_ev, BIT_IP);
    }
}

// 用一组档案接入局域网;成功返回 ESP_OK 且 Wi-Fi 保持开启,失败时内部已回收资源。
static esp_err_t portal_try_sta(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (demo_radio_nvs_prepare() != ESP_OK) return ESP_FAIL;
    if (demo_radio_network_prepare() != ESP_OK) return ESP_FAIL;

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_ERR_NO_MEM;
    s_netif_is_ap = false;
    esp_netif_set_hostname(s_netif, PORTAL_HOSTNAME);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) { portal_net_down(); return err; }
    s_wifi_initialized = true;

    s_ev = xEventGroupCreate();
    if (!s_ev) { portal_net_down(); return ESP_ERR_NO_MEM; }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        portal_wifi_event_cb, NULL, &s_h_wifi);
    s_hw_reg = true;
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        portal_wifi_event_cb, NULL, &s_h_ip);
    s_hip_reg = true;

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) { portal_net_down(); return err; }

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   // 按 AP 实际认证方式协商
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) { portal_net_down(); return err; }

    err = esp_wifi_start();
    if (err != ESP_OK) { portal_net_down(); return err; }
    s_wifi_started = true;

    // 分片等待,便于 we_portal_stop() 及时打断(否则最长要卡住调用方 20 秒)
    EventBits_t bits = 0;
    for (int waited = 0; waited < PORTAL_STA_WAIT_MS && !s_stop_req; waited += 250) {
        bits = xEventGroupWaitBits(s_ev, BIT_IP | BIT_FAIL, pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(250));
        if (bits & (BIT_IP | BIT_FAIL)) break;
    }
    if (!(bits & BIT_IP)) {
        portal_net_down();
        return s_stop_req ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
    }

    portal_fill_ip();
    ESP_LOGI(TAG, "已接入局域网:%s → %s", ssid, s_url);
    return ESP_OK;
}

// 回落:开设备热点 BAL-XXXX,手机连上后访问 192.168.4.1。
static esp_err_t portal_start_ap(void)
{
    if (demo_radio_nvs_prepare() != ESP_OK) return ESP_FAIL;
    if (demo_radio_network_prepare() != ESP_OK) return ESP_FAIL;

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK)
        snprintf(s_ssid, sizeof(s_ssid), "%s%02X%02X", PORTAL_SSID_PREFIX, mac[4], mac[5]);
    else
        snprintf(s_ssid, sizeof(s_ssid), "%s0000", PORTAL_SSID_PREFIX);

    s_netif = esp_netif_create_default_wifi_ap();
    if (!s_netif) return ESP_ERR_NO_MEM;
    s_netif_is_ap = true;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) { portal_net_down(); return err; }
    s_wifi_initialized = true;

    wifi_config_t config = { 0 };
    text_copy((char *)config.ap.ssid, sizeof(config.ap.ssid), s_ssid);
    text_copy((char *)config.ap.password, sizeof(config.ap.password), PORTAL_PASSWORD);
    config.ap.ssid_len = strlen(s_ssid);
    config.ap.channel = 6;
    config.ap.max_connection = 2;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.pmf_cfg.required = false;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) { portal_net_down(); return err; }
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) { portal_net_down(); return err; }
    err = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (err != ESP_OK) { portal_net_down(); return err; }
    err = esp_wifi_start();
    if (err != ESP_OK) { portal_net_down(); return err; }
    s_wifi_started = true;

    portal_fill_ip();
    ESP_LOGI(TAG, "回落热点已启动:%s 密码:%s http://192.168.4.1", s_ssid, PORTAL_PASSWORD);
    return ESP_OK;
}

static esp_err_t portal_http_start(void)
{
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    // we_backup_restore() 在栈上放 we_cfg_t(1.3KB)+ hist_t(0.5KB),
    // 原来的 6KB 余量偏紧,抬到 8KB。
    server_config.stack_size = 8192;
    server_config.max_uri_handlers = 8;
    server_config.server_port = PORTAL_HTTP_PORT;
    esp_err_t err = httpd_start(&s_server, &server_config);
    if (err != ESP_OK) { s_server = NULL; return err; }
    return register_handlers();
}

// 后台协程:先试局域网 → 失败回落热点 → 起 HTTP 服务 → 开 mDNS。
// 之所以放独立任务,是因为 STA 关联最长要等 20 秒,不能卡住 LVGL 任务。
static void portal_task(void *arg)
{
    (void)arg;

    // 让上一个页面的收尾(可能正在 esp_wifi_deinit)先跑完,避免 WIFI_INIT_STATE 冲突
    vTaskDelay(pdMS_TO_TICKS(600));

    esp_err_t err = ESP_FAIL;
    we_cfg_t cfg;
    bool have_profile = false;

    if (!s_stop_req) {
        have_profile = we_cfg_load(&cfg) && cfg.wifi_count > 0;
        for (uint8_t i = 0; have_profile && i < cfg.wifi_count && !s_stop_req; i++) {
            err = portal_try_sta(cfg.wifi[i].ssid, cfg.wifi[i].pass);
            if (err == ESP_OK) break;
            ESP_LOGW(TAG, "档案 %u(%s)连接失败:%s",
                     (unsigned)i, cfg.wifi[i].ssid, esp_err_to_name(err));
        }
        if (s_stop_req) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "未接入局域网,回落热点");
                err = portal_start_ap();
            }
            if (err == ESP_OK) err = portal_http_start();
        }
    }

    if (err == ESP_OK) {
        portal_mdns_start();
        s_last_tick = xTaskGetTickCount();
        s_mode = s_netif_is_ap ? WE_PORTAL_AP : WE_PORTAL_STA;
        ESP_LOGI(TAG, "门户就绪:%s(用户名 %s 密码 %s)", s_url, PORTAL_USER, PORTAL_PASSWORD);
    } else {
        if (!s_stop_req) ESP_LOGE(TAG, "门户启动失败:%s", esp_err_to_name(err));
        portal_net_down();
        s_mode = WE_PORTAL_OFF;
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------- 启停
esp_err_t we_portal_start(void)
{
    if (s_mode != WE_PORTAL_OFF) return ESP_OK;   // 已在运行或正在连接

    char raw[48];
    snprintf(raw, sizeof(raw), "%s:%s", PORTAL_USER, PORTAL_PASSWORD);
    b64_encode(raw, s_auth_b64, sizeof(s_auth_b64));

    s_stop_req = false;
    s_mode = WE_PORTAL_CONNECTING;
    s_last_tick = xTaskGetTickCount();
    if (xTaskCreate(portal_task, "we_portal", 6144, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        s_mode = WE_PORTAL_OFF;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void we_portal_stop(void)
{
    if (s_task) {
        s_stop_req = true;
        // 后台任务每 250ms 检查一次停止标志,正常 1 秒内即可退出
        for (int i = 0; i < 60 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
        s_stop_req = false;
        s_task = NULL;
    }
    portal_net_down();
    s_mode = WE_PORTAL_OFF;
}

bool we_portal_running(void)
{
    return s_mode == WE_PORTAL_STA || s_mode == WE_PORTAL_AP;
}

we_portal_mode_t we_portal_mode(void) { return s_mode; }
const char *we_portal_url(void) { return s_url; }
const char *we_portal_ip(void) { return s_ip; }
const char *we_portal_hostname(void) { return s_mdns_ready ? PORTAL_HOSTNAME : ""; }
const char *we_portal_ssid(void) { return s_netif_is_ap ? s_ssid : ""; }
const char *we_portal_password(void) { return PORTAL_PASSWORD; }

uint32_t we_portal_idle_seconds(void)
{
    if (!we_portal_running() || s_last_tick == 0) return 0;
    TickType_t d = xTaskGetTickCount() - s_last_tick;
    return (uint32_t)((d * portTICK_PERIOD_MS) / 1000);
}
