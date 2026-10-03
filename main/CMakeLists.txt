idf_component_register(
    SRCS "main.c"
         "app_balance.c"
         "we_backup.c"
         "we_cfg.c"
         "we_diag.c"
         "we_hermes.c"
         "we_portal.c"
         "we_provider.c"
         "we_set.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
         "demo_portal.c"
         "fap_screenshot.c"
    INCLUDE_DIRS "."
    # esp_app_format 提供 esp_app_desc.h(门户 /diag 要打印固件版本/编译时间),
    # 它不在 IDF 的公共依赖列表里,必须显式声明。
    REQUIRES bsp bt esp_app_format esp_driver_usb_serial_jtag esp_event esp_http_client esp_http_server esp_hw_support esp_netif esp_timer esp-tls esp_wifi json mdns nvs_flash
)
