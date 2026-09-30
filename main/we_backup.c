// main/we_backup.c — 备份/恢复:we_cfg 配置 + balhist 历史 ⇄ 单个 JSON 文档。
//
// 三个必须守住的点:
//   1. 配置与历史一起写。hist_check_rows() 一旦发现平台顺序与 row_id 不符就会
//      清空全部历史 —— 只恢复 Key 等于白丢热力图。
//   2. 恢复后由门户重启设备。app_balance 已在内存里缓存了 s_cfg / s_hist,
//      不重启会出现"NVS 已换、界面还是旧的"。
//   3. 备份文件含明文 API Key,页面与日志都要提醒用户妥善保管。
//
// 边界:WiFi/平台字段超长一律拒绝而不是静默截断 —— 截断后的 SSID 是连不上的,
// 与其"看起来恢复了",不如直接报错。摘要类字段(nickname/label)允许截断。
#include "we_backup.h"

#include "we_cfg.h"
#include "we_hist.h"
#include "we_portal.h"     // we_cfg_load / we_cfg_save(NVS 存取)

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "we_backup";

// 余额是钱,留 4 位小数足够;顺手把 float→double 的二进制尾巴抹掉 ——
// 否则 cJSON 会按 %1.15g 原样输出 12.3400001525879 这种没法看的数字。
static double money(float v)
{
    double d = (double)v;
    if (!(d > -1e9 && d < 1e9)) return 0.0;   // NaN/Inf/离谱值一律归零
    double scaled = d * 10000.0;
    double rounded = (scaled >= 0.0) ? (scaled + 0.5) : (scaled - 0.5);
    return (double)(long long)rounded / 10000.0;
}

static void set_msg(char *buf, size_t len, const char *text)
{
    if (!buf || len == 0) return;
    strlcpy(buf, text ? text : "", len);
}

// ---------------------------------------------------------------- 导出
static void export_cfg(cJSON *root, we_cfg_t *out)
{
    we_cfg_t cfg;
    if (!we_cfg_load(&cfg)) we_cfg_init(&cfg);   // 尚未配置时导出空壳,便于人工填写
    *out = cfg;

    cJSON *jcfg = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "cfg", jcfg);
    cJSON_AddStringToObject(jcfg, "nickname", cfg.nickname);

    cJSON *jwifi = cJSON_CreateArray();
    cJSON_AddItemToObject(jcfg, "wifi", jwifi);
    for (uint8_t i = 0; i < cfg.wifi_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", cfg.wifi[i].ssid);
        cJSON_AddStringToObject(o, "pass", cfg.wifi[i].pass);
        cJSON_AddStringToObject(o, "host", cfg.wifi[i].host);
        cJSON_AddItemToArray(jwifi, o);
    }

    cJSON *jprov = cJSON_CreateArray();
    cJSON_AddItemToObject(jcfg, "providers", jprov);
    for (uint8_t i = 0; i < cfg.prov_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", cfg.prov[i].provider_id);
        cJSON_AddStringToObject(o, "key", cfg.prov[i].api_key);
        cJSON_AddStringToObject(o, "label", cfg.prov[i].label);
        cJSON_AddItemToArray(jprov, o);
    }
}

static int export_hist(cJSON *root)
{
    hist_t h;
    memset(&h, 0, sizeof(h));
    app_hist_nvs_read(&h);       // 读失败即视为空历史,导出空数组而不是报错

    cJSON *jhist = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "hist", jhist);

    cJSON *rows = cJSON_CreateArray();
    cJSON_AddItemToObject(jhist, "rows", rows);
    for (int i = 0; i < HIST_ROWS; i++)
        cJSON_AddItemToArray(rows, cJSON_CreateString(h.row_id[i]));

    cJSON *days = cJSON_CreateArray();
    cJSON_AddItemToObject(jhist, "days", days);

    // NVS 是环形槽,顺序无意义;按日期升序导出,方便人直接看这份 JSON。
    int order[HIST_DAYS];
    int used = 0;
    for (int i = 0; i < HIST_DAYS; i++) {
        if (h.day[i] == 0) continue;             // 空槽不导出
        int k = used;
        while (k > 0 && h.day[order[k - 1]] > h.day[i]) { order[k] = order[k - 1]; k--; }
        order[k] = i;
        used++;
    }
    for (int i = 0; i < used; i++) {
        int idx = order[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "day", (double)h.day[idx]);
        cJSON *bal = cJSON_CreateArray();
        cJSON_AddItemToObject(o, "bal", bal);
        for (int r = 0; r < HIST_ROWS; r++)
            cJSON_AddItemToArray(bal, cJSON_CreateNumber(money(h.bal[idx][r])));
        cJSON_AddItemToArray(days, o);
    }
    return used;
}

char *we_backup_export(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    we_cfg_t cfg;                                // export_cfg 填好,后面写 counts 用
    cJSON_AddStringToObject(root, "format", WE_BACKUP_FORMAT);
    cJSON_AddNumberToObject(root, "version", (double)WE_BACKUP_VERSION);

    // 未校时就不写时间戳,免得留下误导性的 1970 时间
    // 缓冲给到 96:gcc 的 -Werror=format-truncation 会按 %d 的最坏宽度(11 位)算,
    // 6 个字段 6*11+5=71,32 字节会被判成可能截断;实际输出恒为 19 字节。
    char when[96] = "";
    time_t now = time(NULL);
    if (now >= 1700000000) {
        time_t t8 = now + 8 * 3600;              // 与设备记账天界一致,用 UTC+8
        struct tm tm;
        gmtime_r(&t8, &tm);
        int year = tm.tm_year + 1900;
        if (year < 1970 || year > 9999) year = 1970;   // 异常年份垫掉,别写出 6 位年份
        snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d",
                 year, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    cJSON_AddStringToObject(root, "created_utc8", when);

    char macs[18] = "";
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "device_mac", macs);

    export_cfg(root, &cfg);
    int hist_days = export_hist(root);
    // 摘要:方便人一眼看清这份备份里到底有什么,恢复时不读这一节。
    cJSON *sum = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "counts", sum);
    cJSON_AddNumberToObject(sum, "wifi", (double)cfg.wifi_count);
    cJSON_AddNumberToObject(sum, "providers", (double)cfg.prov_count);
    cJSON_AddNumberToObject(sum, "hist_days", (double)hist_days);

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out) ESP_LOGE(TAG, "JSON 序列化失败");
    return out;
}

// ---------------------------------------------------------------- 恢复
esp_err_t we_backup_restore(const char *json, char *err_msg, size_t err_msg_len)
{
    cJSON *root = NULL;
    we_cfg_t cfg;
    hist_t hist;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    int i = 0, n = 0, hist_days = 0;
    bool rows_match = true;

    if (!json) {
        set_msg(err_msg, err_msg_len, "请求体为空");
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(json);
    if (!root) {
        set_msg(err_msg, err_msg_len, "不是合法 JSON");
        return ESP_ERR_INVALID_ARG;
    }

    // ---- 文件头 ----
    cJSON *jobj = cJSON_GetObjectItem(root, "format");
    if (!cJSON_IsString(jobj) || strcmp(jobj->valuestring, WE_BACKUP_FORMAT) != 0) {
        set_msg(err_msg, err_msg_len, "不是本设备的备份文件(format 不符)");
        goto out;
    }
    jobj = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsNumber(jobj) || jobj->valueint != WE_BACKUP_VERSION) {
        set_msg(err_msg, err_msg_len, "备份版本不兼容");
        goto out;
    }

    // ---- 配置 ----
    jobj = cJSON_GetObjectItem(root, "cfg");
    if (!cJSON_IsObject(jobj)) {
        set_msg(err_msg, err_msg_len, "备份缺少 cfg 段");
        goto out;
    }

    we_cfg_init(&cfg);
    cJSON *item = cJSON_GetObjectItem(jobj, "nickname");
    if (cJSON_IsString(item))
        strlcpy(cfg.nickname, item->valuestring, sizeof(cfg.nickname));

    cJSON *jarr = cJSON_GetObjectItem(jobj, "wifi");
    n = cJSON_GetArraySize(jarr);                 // 非数组时为 0
    for (i = 0; i < n && cfg.wifi_count < WE_CFG_MAX_WIFI; i++) {
        cJSON *o  = cJSON_GetArrayItem(jarr, i);
        cJSON *s  = cJSON_GetObjectItem(o, "ssid");
        cJSON *pw = cJSON_GetObjectItem(o, "pass");
        cJSON *ho = cJSON_GetObjectItem(o, "host");
        if (!cJSON_IsString(s) || !s->valuestring[0]) continue;
        if (strlen(s->valuestring) >= sizeof(cfg.wifi[0].ssid) ||
            (cJSON_IsString(pw) && strlen(pw->valuestring) >= sizeof(cfg.wifi[0].pass)) ||
            (cJSON_IsString(ho) && strlen(ho->valuestring) >= sizeof(cfg.wifi[0].host))) {
            set_msg(err_msg, err_msg_len, "WiFi 字段超长(SSID 32 / 密码 64 / host 47 字节上限)");
            goto out;
        }
        if (!we_cfg_add_wifi(&cfg, s->valuestring,
                             cJSON_IsString(pw) ? pw->valuestring : "",
                             cJSON_IsString(ho) ? ho->valuestring : "")) {
            set_msg(err_msg, err_msg_len, "WiFi 档案写入失败");
            goto out;
        }
    }

    jarr = cJSON_GetObjectItem(jobj, "providers");
    n = cJSON_GetArraySize(jarr);
    for (i = 0; i < n && cfg.prov_count < WE_CFG_MAX_PROV; i++) {
        cJSON *o  = cJSON_GetArrayItem(jarr, i);
        cJSON *id = cJSON_GetObjectItem(o, "id");
        cJSON *ky = cJSON_GetObjectItem(o, "key");
        cJSON *lb = cJSON_GetObjectItem(o, "label");
        if (!cJSON_IsString(id) || !cJSON_IsString(ky)) continue;
        if (!id->valuestring[0] || !ky->valuestring[0]) continue;
        if (strlen(id->valuestring) >= sizeof(cfg.prov[0].provider_id) ||
            strlen(ky->valuestring) >= sizeof(cfg.prov[0].api_key) ||
            (cJSON_IsString(lb) && strlen(lb->valuestring) >= sizeof(cfg.prov[0].label))) {
            set_msg(err_msg, err_msg_len, "平台字段超长(id 15 / key 95 / label 23 字节上限)");
            goto out;
        }
        we_cfg_add_prov(&cfg, id->valuestring, ky->valuestring,
                        cJSON_IsString(lb) ? lb->valuestring : "");
    }

    if (cfg.wifi_count == 0 && cfg.prov_count == 0) {
        set_msg(err_msg, err_msg_len, "备份里既没有 WiFi 也没有平台 Key");
        goto out;
    }

    // ---- 历史 ----
    memset(&hist, 0, sizeof(hist));
    hist.magic = HIST_MAGIC;
    jobj = cJSON_GetObjectItem(root, "hist");
    if (cJSON_IsObject(jobj)) {
        jarr = cJSON_GetObjectItem(jobj, "rows");
        n = cJSON_GetArraySize(jarr);
        for (i = 0; i < n && i < HIST_ROWS; i++) {
            item = cJSON_GetArrayItem(jarr, i);
            if (cJSON_IsString(item))
                strlcpy(hist.row_id[i], item->valuestring, sizeof(hist.row_id[i]));
        }

        jarr = cJSON_GetObjectItem(jobj, "days");
        n = cJSON_GetArraySize(jarr);
        for (i = 0; i < n; i++) {
            cJSON *o  = cJSON_GetArrayItem(jarr, i);
            cJSON *jd = cJSON_GetObjectItem(o, "day");
            cJSON *jb = cJSON_GetObjectItem(o, "bal");
            if (!cJSON_IsNumber(jd)) continue;
            int day = jd->valueint;
            if (day <= 0 || day > 65535) continue;
            int idx = day % HIST_DAYS;
            hist.day[idx] = (uint16_t)day;
            int bn = cJSON_GetArraySize(jb);
            for (int r = 0; r < bn && r < HIST_ROWS; r++) {
                cJSON *b = cJSON_GetArrayItem(jb, r);
                if (cJSON_IsNumber(b)) hist.bal[idx][r] = (float)b->valuedouble;
            }
            hist_days++;
        }
    }

    // 平台顺序与历史行语义不一致时,下次刷新 hist_check_rows() 会清空历史。
    // 仍然允许恢复(用户可能就是换平台),但在结果页把后果说清楚。
    for (i = 0; i < HIST_ROWS; i++) {
        const char *want = (i < (int)cfg.prov_count) ? cfg.prov[i].provider_id : "";
        if (strncmp(hist.row_id[i], want, sizeof(hist.row_id[i])) != 0) {
            rows_match = false;
            break;
        }
    }

    // ---- 落盘:两者必须一起写 ----
    if (!we_cfg_save(&cfg)) {
        set_msg(err_msg, err_msg_len, "配置写入 NVS 失败");
        goto out;
    }
    if (!app_hist_nvs_write(&hist)) {
        set_msg(err_msg, err_msg_len, "配置已写入,但历史写入 NVS 失败");
        goto out;
    }

    ESP_LOGI(TAG, "备份已恢复:wifi=%u prov=%u hist_days=%d rows_match=%d",
             (unsigned)cfg.wifi_count, (unsigned)cfg.prov_count, hist_days, (int)rows_match);

    if (err_msg && err_msg_len) {
        if (rows_match)
            snprintf(err_msg, err_msg_len, "已恢复 %u 组 WiFi、%u 个平台 Key、%d 天记录",
                     (unsigned)cfg.wifi_count, (unsigned)cfg.prov_count, hist_days);
        else
            snprintf(err_msg, err_msg_len,
                     "已恢复 %u 组 WiFi、%u 个平台 Key、%d 天记录;"
                     "但平台数量或顺序与备份不同,下次刷新会按新配置重记历史(旧记录将被清空)",
                     (unsigned)cfg.wifi_count, (unsigned)cfg.prov_count, hist_days);
    }
    err = ESP_OK;

out:
    cJSON_Delete(root);
    return err;
}
