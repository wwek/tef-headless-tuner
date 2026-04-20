#include "wifi_manager.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_mgr";

#define NVS_NAMESPACE      "wifi_mgr"
#define NVS_KEY_STA_SSID   "sta_ssid"
#define NVS_KEY_STA_PASS   "sta_pass"

#define STA_CONNECTED_BIT  BIT0
#define STA_FAIL_BIT       BIT1

#define MAX_SSID_LEN       32
#define MAX_PASS_LEN       64
#define AP_MAX_CONN        4

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif;
static wifi_mgr_mode_t s_mode = WIFI_MGR_MODE_AP;
static bool s_connected;
static char s_ip_str[16];
static char s_ap_ssid[32];
static int s_sta_retry_count;

static wifi_mgr_event_cb_t s_event_cb;
static void *s_event_cb_ctx;

#ifndef CONFIG_WIFI_AP_SSID_PREFIX
#define CONFIG_WIFI_AP_SSID_PREFIX "TEF6686"
#endif

#ifndef CONFIG_WIFI_AP_PASSWORD
#define CONFIG_WIFI_AP_PASSWORD ""
#endif

#ifndef CONFIG_WIFI_AP_CHANNEL
#define CONFIG_WIFI_AP_CHANNEL 1
#endif

#ifndef CONFIG_WIFI_STA_CONNECT_TIMEOUT_MS
#define CONFIG_WIFI_STA_CONNECT_TIMEOUT_MS 5000
#endif

static void notify_event(wifi_mgr_mode_t mode, bool connected)
{
    if (s_event_cb) {
        s_event_cb(mode, connected, s_event_cb_ctx);
    }
}

static void set_mode(wifi_mgr_mode_t mode, bool connected)
{
    s_mode = mode;
    s_connected = connected;
    if (!connected) {
        s_ip_str[0] = '\0';
    }
    notify_event(mode, connected);
}

// Build AP SSID from prefix + last 2 bytes of MAC
static void build_ap_ssid(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read AP MAC: %s", esp_err_to_name(err));
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-????", CONFIG_WIFI_AP_SSID_PREFIX);
        return;
    }
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X",
             CONFIG_WIFI_AP_SSID_PREFIX, mac[4], mac[5]);
}

static esp_err_t start_ap(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP mode failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = CONFIG_WIFI_AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP start failed: %s", esp_err_to_name(err));
        return err;
    }

    set_mode(WIFI_MGR_MODE_AP, false);
    ESP_LOGI(TAG, "AP started: %s", s_ap_ssid);
    return ESP_OK;
}

static esp_err_t start_apsta_with_sta(const char *ssid, const char *password)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set APSTA mode failed: %s", esp_err_to_name(err));
        return err;
    }

    // Configure AP side
    wifi_config_t ap_cfg = {
        .ap = {
            .channel = CONFIG_WIFI_AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP config in APSTA failed: %s", esp_err_to_name(err));
        return err;
    }

    // Configure STA side
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);

    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sta_retry_count = 0;
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "APSTA start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "APSTA started, connecting STA to %s", ssid);

    // Wait for STA connection or failure
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            STA_CONNECTED_BIT | STA_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(CONFIG_WIFI_STA_CONNECT_TIMEOUT_MS));

    if (bits & STA_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA connected, stopping AP");
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "switch to STA-only failed: %s", esp_err_to_name(err));
            // Remain in APSTA as degraded state
            set_mode(WIFI_MGR_MODE_APSTA, true);
            return ESP_OK;
        }
        set_mode(WIFI_MGR_MODE_STA, true);
        return ESP_OK;
    }

    // STA failed or timed out - fall back to AP only
    ESP_LOGW(TAG, "STA connect failed, falling back to AP-only");
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fallback to AP failed: %s", esp_err_to_name(err));
        return err;
    }
    set_mode(WIFI_MGR_MODE_AP, false);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_sta_retry_count < 1) {
                s_sta_retry_count++;
                ESP_LOGI(TAG, "STA disconnected, retrying (%d/1)", s_sta_retry_count);
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "STA reconnect failed: %s", esp_err_to_name(err));
                    xEventGroupSetBits(s_wifi_event_group, STA_FAIL_BIT);
                }
            } else {
                ESP_LOGW(TAG, "STA connect failed after retry");
                xEventGroupSetBits(s_wifi_event_group, STA_FAIL_BIT);
            }
            s_connected = false;
            s_ip_str[0] = '\0';
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP: %s", s_ip_str);
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, STA_CONNECTED_BIT);
    }
}

// Read STA credentials from NVS. Returns ESP_OK if both ssid and password found.
static esp_err_t read_sta_credentials(char *ssid, size_t ssid_len,
                                       char *password, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(handle, NVS_KEY_STA_SSID, ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = nvs_get_str(handle, NVS_KEY_STA_PASS, password, &pass_len);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_manager_init(void)
{
    // Init NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Init netif
    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create default event loop
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create netif interfaces
    esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    // Init WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create event group for STA signaling
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Register event handlers
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register WiFi event handler failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register IP event handler failed: %s", esp_err_to_name(err));
        return err;
    }

    // Build AP SSID
    build_ap_ssid();

    // Try to load STA credentials from NVS
    char ssid[MAX_SSID_LEN + 1] = {0};
    char password[MAX_PASS_LEN + 1] = {0};

    if (read_sta_credentials(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
        ESP_LOGI(TAG, "found STA credentials for %s", ssid);
        err = start_apsta_with_sta(ssid, password);
    } else {
        ESP_LOGI(TAG, "no STA credentials, starting AP-only");
        err = start_ap();
    }

    return err;
}

const char *wifi_manager_get_ip(void)
{
    if (!s_connected) {
        return NULL;
    }
    return s_ip_str[0] ? s_ip_str : NULL;
}

wifi_mgr_mode_t wifi_manager_get_mode(void)
{
    return s_mode;
}

esp_err_t wifi_manager_set_sta_config(const char *ssid, const char *password)
{
    if (!ssid || !*ssid) {
        ESP_LOGE(TAG, "set_sta_config: SSID is required");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_STA_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write ssid failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_STA_PASS, password ? password : "");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write password failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "STA config saved: %s", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_clear_sta_config(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for clear failed: %s", esp_err_to_name(err));
        return err;
    }

    // Erase both keys; ignore ESP_ERR_NVS_NOT_FOUND
    nvs_erase_key(handle, NVS_KEY_STA_SSID);
    nvs_erase_key(handle, NVS_KEY_STA_PASS);

    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit after clear failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "STA credentials cleared");
    return ESP_OK;
}

void wifi_manager_register_event_cb(wifi_mgr_event_cb_t cb, void *ctx)
{
    s_event_cb = cb;
    s_event_cb_ctx = ctx;
}

void wifi_manager_get_ap_ssid(char *buf, size_t buf_len)
{
    if (buf && buf_len > 0) {
        strncpy(buf, s_ap_ssid, buf_len - 1);
        buf[buf_len - 1] = '\0';
    }
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}
