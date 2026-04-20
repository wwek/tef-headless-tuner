#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    WIFI_MGR_MODE_AP,
    WIFI_MGR_MODE_STA,
    WIFI_MGR_MODE_APSTA,
} wifi_mgr_mode_t;

typedef void (*wifi_mgr_event_cb_t)(wifi_mgr_mode_t mode, bool connected, void *ctx);

esp_err_t wifi_manager_init(void);
const char *wifi_manager_get_ip(void);
wifi_mgr_mode_t wifi_manager_get_mode(void);
esp_err_t wifi_manager_set_sta_config(const char *ssid, const char *password);
esp_err_t wifi_manager_clear_sta_config(void);
void wifi_manager_register_event_cb(wifi_mgr_event_cb_t cb, void *ctx);
void wifi_manager_get_ap_ssid(char *buf, size_t buf_len);
bool wifi_manager_is_connected(void);
