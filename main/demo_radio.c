#include "demo_radio.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "demo_radio";

static bool s_nvs_ready;
static bool s_netif_ready;
static bool s_event_loop_ready;

esp_err_t demo_radio_nvs_prepare(void)
{
    if (s_nvs_ready) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // 示例不能为了启动无线功能而擦除未来应用可能已经保存的数据。
        ESP_LOGE(TAG, "NVS 初始化失败: %s;未自动擦除分区", esp_err_to_name(err));
        return err;
    }
    s_nvs_ready = true;
    return ESP_OK;
}

esp_err_t demo_radio_network_prepare(void)
{
    if (!s_netif_ready) {
        esp_err_t err = esp_netif_init();
        // 门户等其它模块可能已先初始化过网络栈;INVALID_STATE 说明已就绪,按成功处理,
        // 否则本函数会永久失败并连带让余额刷新取不到数。
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_netif_ready = true;
    }
    if (!s_event_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_event_loop_ready = true;
    }
    return ESP_OK;
}
