#include "wifi_manager.h"

#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <stdint.h>
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
#define STA_MAX_RETRY      2
#define TRANSITION_DELAY_MS 750
#define TRANSITION_TASK_STACK 4096

typedef enum {
    WIFI_TRANSITION_NONE,
    WIFI_TRANSITION_STA_ONLY,
    WIFI_TRANSITION_AP_ONLY,
    WIFI_TRANSITION_RESTORE_ACTIVE,
} wifi_transition_action_t;

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static wifi_mgr_mode_t s_mode = WIFI_MGR_MODE_AP;
static bool s_connected;
static char s_ip_str[16];
static char s_ap_ssid[32];
static int s_sta_retry_count;
static bool s_has_active_sta_config;
static char s_active_ssid[MAX_SSID_LEN + 1];
static char s_active_pass[MAX_PASS_LEN + 1];
static bool s_has_restore_sta_config;
static char s_restore_ssid[MAX_SSID_LEN + 1];
static char s_restore_pass[MAX_PASS_LEN + 1];
static TaskHandle_t s_transition_task;
static wifi_transition_action_t s_pending_transition = WIFI_TRANSITION_NONE;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
static bool s_wifi_inited;
static bool s_event_loop_owned;

static wifi_mgr_event_cb_t s_event_cb;
static void *s_event_cb_ctx;

static const char *wifi_mode_name_local(wifi_mgr_mode_t mode)
{
    switch (mode) {
    case WIFI_MGR_MODE_AP:
        return "AP";
    case WIFI_MGR_MODE_STA:
        return "STA";
    case WIFI_MGR_MODE_APSTA:
        return "APSTA";
    default:
        return "UNKNOWN";
    }
}

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

static void clear_active_sta_state(void)
{
    s_has_active_sta_config = false;
    s_active_ssid[0] = '\0';
    s_active_pass[0] = '\0';
}

static void clear_restore_sta_state(void)
{
    s_has_restore_sta_config = false;
    s_restore_ssid[0] = '\0';
    s_restore_pass[0] = '\0';
}

static void set_active_sta_state(const char *ssid, const char *password)
{
    s_has_active_sta_config = true;
    strncpy(s_active_ssid, ssid, sizeof(s_active_ssid) - 1);
    s_active_ssid[sizeof(s_active_ssid) - 1] = '\0';
    strncpy(s_active_pass, password ? password : "", sizeof(s_active_pass) - 1);
    s_active_pass[sizeof(s_active_pass) - 1] = '\0';
}

static void set_restore_sta_state(const char *ssid, const char *password)
{
    s_has_restore_sta_config = true;
    strncpy(s_restore_ssid, ssid, sizeof(s_restore_ssid) - 1);
    s_restore_ssid[sizeof(s_restore_ssid) - 1] = '\0';
    strncpy(s_restore_pass, password ? password : "", sizeof(s_restore_pass) - 1);
    s_restore_pass[sizeof(s_restore_pass) - 1] = '\0';
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

static esp_err_t stop_wifi_if_running(void)
{
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stop Wi-Fi failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static void wifi_manager_cleanup_partial_init(void)
{
    s_pending_transition = WIFI_TRANSITION_NONE;

    if (s_wifi_event_instance) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_instance);
        s_wifi_event_instance = NULL;
    }
    if (s_ip_event_instance) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_event_instance);
        s_ip_event_instance = NULL;
    }
    if (s_transition_task) {
        vTaskDelete(s_transition_task);
        s_transition_task = NULL;
    }
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    if (s_wifi_inited) {
        stop_wifi_if_running();
        esp_wifi_deinit();
        s_wifi_inited = false;
    }
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_event_loop_owned) {
        esp_event_loop_delete_default();
        s_event_loop_owned = false;
    }
}

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

static esp_err_t load_active_sta_credentials(void)
{
    char ssid[MAX_SSID_LEN + 1] = {0};
    char password[MAX_PASS_LEN + 1] = {0};
    esp_err_t err = read_sta_credentials(ssid, sizeof(ssid), password, sizeof(password));
    if (err != ESP_OK) {
        clear_active_sta_state();
        clear_restore_sta_state();
        return err;
    }

    set_active_sta_state(ssid, password);
    return ESP_OK;
}

static esp_err_t write_active_sta_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_STA_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_STA_PASS, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write active credentials failed: %s", esp_err_to_name(err));
        return err;
    }

    set_active_sta_state(ssid, password);
    return ESP_OK;
}

static esp_err_t erase_active_sta_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for clear failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_erase_key(handle, NVS_KEY_STA_SSID);
    nvs_erase_key(handle, NVS_KEY_STA_PASS);
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit after clear failed: %s", esp_err_to_name(err));
        return err;
    }

    clear_active_sta_state();
    clear_restore_sta_state();
    return ESP_OK;
}

static esp_err_t configure_ap_interface(void)
{
    wifi_config_t ap_cfg = {
        .ap = {
            .channel = CONFIG_WIFI_AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
        },
    };
    size_t pass_len = strlen(CONFIG_WIFI_AP_PASSWORD);

    strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);

    if (pass_len == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        if (pass_len < 8 || pass_len > sizeof(ap_cfg.ap.password) - 1) {
            ESP_LOGE(TAG, "AP password must be 8..%u characters when set", (unsigned)(sizeof(ap_cfg.ap.password) - 1));
            return ESP_ERR_INVALID_ARG;
        }
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        memcpy(ap_cfg.ap.password, CONFIG_WIFI_AP_PASSWORD, pass_len);
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP config failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t configure_sta_interface(const char *ssid, const char *password)
{
    wifi_config_t sta_cfg = {0};
    size_t ssid_len = strnlen(ssid, sizeof(sta_cfg.sta.ssid) + 1);
    size_t pass_len = strnlen(password, sizeof(sta_cfg.sta.password) + 1);

    if (ssid_len == 0 || ssid_len > sizeof(sta_cfg.sta.ssid)) {
        ESP_LOGE(TAG, "STA SSID length must be 1..%u bytes", (unsigned)sizeof(sta_cfg.sta.ssid));
        return ESP_ERR_INVALID_ARG;
    }
    if (pass_len > sizeof(sta_cfg.sta.password)) {
        ESP_LOGE(TAG, "STA password length must be <= %u bytes", (unsigned)sizeof(sta_cfg.sta.password));
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(sta_cfg.sta.ssid, ssid, ssid_len);
    memcpy(sta_cfg.sta.password, password, pass_len);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void prepare_sta_connection_wait(void)
{
    xEventGroupClearBits(s_wifi_event_group, STA_CONNECTED_BIT | STA_FAIL_BIT);
    s_sta_retry_count = 0;
    s_connected = false;
    s_ip_str[0] = '\0';
}

static esp_err_t wait_for_sta_connection(void)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           STA_CONNECTED_BIT | STA_FAIL_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(CONFIG_WIFI_STA_CONNECT_TIMEOUT_MS));

    if (bits & STA_CONNECTED_BIT) {
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t switch_to_sta_only(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "switch to STA-only failed: %s", esp_err_to_name(err));
        return err;
    }

    set_mode(WIFI_MGR_MODE_STA, s_connected);
    ESP_LOGI(TAG, "Wi-Fi switched to STA-only");
    return ESP_OK;
}

static esp_err_t start_ap(void)
{
    esp_err_t err = stop_wifi_if_running();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = configure_ap_interface();
    if (err != ESP_OK) {
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

static esp_err_t start_sta_with_credentials(const char *ssid, const char *password)
{
    esp_err_t err = stop_wifi_if_running();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = configure_sta_interface(ssid, password);
    if (err != ESP_OK) {
        return err;
    }

    prepare_sta_connection_wait();
    set_mode(WIFI_MGR_MODE_STA, false);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "STA started, connecting to %s", ssid);
    err = wait_for_sta_connection();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA connection failed for %s", ssid);
        return err;
    }

    set_mode(WIFI_MGR_MODE_STA, true);
    ESP_LOGI(TAG, "STA connected to %s", ssid);
    return ESP_OK;
}

static esp_err_t start_apsta_validation_mode(const char *ssid, const char *password)
{
    esp_err_t err = stop_wifi_if_running();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set APSTA mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = configure_ap_interface();
    if (err != ESP_OK) {
        return err;
    }

    err = configure_sta_interface(ssid, password);
    if (err != ESP_OK) {
        return err;
    }

    prepare_sta_connection_wait();
    set_mode(WIFI_MGR_MODE_APSTA, false);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "APSTA start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "APSTA started, connecting STA to %s", ssid);
    err = wait_for_sta_connection();
    if (err == ESP_OK) {
        set_mode(WIFI_MGR_MODE_APSTA, true);
        ESP_LOGI(TAG, "Candidate Wi-Fi validated via APSTA");
        return ESP_OK;
    }

    set_mode(WIFI_MGR_MODE_APSTA, false);
    ESP_LOGW(TAG, "Candidate Wi-Fi validation failed for %s", ssid);
    return ESP_ERR_TIMEOUT;
}

static void wifi_transition_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TRANSITION_DELAY_MS)) > 0) {
        }

        switch (s_pending_transition) {
        case WIFI_TRANSITION_NONE:
            break;
        case WIFI_TRANSITION_STA_ONLY:
            if (switch_to_sta_only() != ESP_OK) {
                ESP_LOGW(TAG, "deferred STA-only transition failed");
            }
            break;
        case WIFI_TRANSITION_AP_ONLY:
            if (start_ap() != ESP_OK) {
                ESP_LOGW(TAG, "deferred AP-only transition failed");
            }
            break;
        case WIFI_TRANSITION_RESTORE_ACTIVE:
            if (s_has_restore_sta_config && start_sta_with_credentials(s_restore_ssid, s_restore_pass) == ESP_OK) {
                ESP_LOGI(TAG, "restored last known good Wi-Fi");
            } else if (start_ap() != ESP_OK) {
                ESP_LOGW(TAG, "deferred restore failed and AP fallback could not start");
            }
            break;
        }
    }
}

static void cancel_transition_task(void)
{
    s_pending_transition = WIFI_TRANSITION_NONE;
}

static esp_err_t schedule_transition(wifi_transition_action_t action)
{
    if (!s_transition_task) {
        ESP_LOGE(TAG, "Wi-Fi transition worker is not available");
        return ESP_ERR_INVALID_STATE;
    }

    s_pending_transition = action;
    xTaskNotifyGive(s_transition_task);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            s_sta_retry_count = 0;
            ESP_LOGI(TAG, "STA started, initiating connection");
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "STA initial connect failed: %s", esp_err_to_name(err));
                xEventGroupSetBits(s_wifi_event_group, STA_FAIL_BIT);
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_sta_retry_count < STA_MAX_RETRY) {
                s_sta_retry_count++;
                ESP_LOGI(TAG, "STA disconnected, retrying (%d/%d)", s_sta_retry_count, STA_MAX_RETRY);
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "STA reconnect failed: %s", esp_err_to_name(err));
                    xEventGroupSetBits(s_wifi_event_group, STA_FAIL_BIT);
                }
            } else {
                ESP_LOGW(TAG, "STA connect failed after %d retries", STA_MAX_RETRY);
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
    if (err == ESP_OK) {
        s_event_loop_owned = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create netif interfaces
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_ap_netif || !s_sta_netif) {
        ESP_LOGE(TAG, "failed to create default Wi-Fi netifs");
        wifi_manager_cleanup_partial_init();
        return ESP_ERR_NO_MEM;
    }

    // Init WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        wifi_manager_cleanup_partial_init();
        return err;
    }
    s_wifi_inited = true;

    // Disable WiFi power save for real-time audio streaming latency
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Create event group for STA signaling
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "failed to create event group");
        wifi_manager_cleanup_partial_init();
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreate(wifi_transition_task,
                                     "wifi_transition",
                                     TRANSITION_TASK_STACK,
                                     NULL,
                                     tskIDLE_PRIORITY + 1,
                                     &s_transition_task);
    if (task_ok != pdPASS) {
        s_transition_task = NULL;
        ESP_LOGE(TAG, "failed to create Wi-Fi transition worker");
        wifi_manager_cleanup_partial_init();
        return ESP_ERR_NO_MEM;
    }

    // Register event handlers
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL, &s_wifi_event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register WiFi event handler failed: %s", esp_err_to_name(err));
        wifi_manager_cleanup_partial_init();
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL, &s_ip_event_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register IP event handler failed: %s", esp_err_to_name(err));
        wifi_manager_cleanup_partial_init();
        return err;
    }

    // Build AP SSID
    build_ap_ssid();

    err = load_active_sta_credentials();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "found active STA credentials for %s", s_active_ssid);
        err = start_sta_with_credentials(s_active_ssid, s_active_pass);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "active STA connect failed, entering AP recovery mode");
            clear_restore_sta_state();
            err = start_ap();
        } else {
            set_restore_sta_state(s_active_ssid, s_active_pass);
        }
    } else {
        ESP_LOGI(TAG, "no active STA credentials, starting AP-only");
        clear_restore_sta_state();
        err = start_ap();
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Wi-Fi ready: mode=%s, ap_ssid=%s, connected=%s, ip=%s",
                 wifi_mode_name_local(s_mode),
                 s_ap_ssid[0] ? s_ap_ssid : "<unset>",
                 s_connected ? "yes" : "no",
                 s_ip_str[0] ? s_ip_str : "<none>");
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

    bool can_restore = s_has_restore_sta_config;

    cancel_transition_task();

    esp_err_t err = start_apsta_validation_mode(ssid, password ? password : "");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "candidate Wi-Fi validation failed for %s", ssid);
        if (can_restore) {
            err = schedule_transition(WIFI_TRANSITION_RESTORE_ACTIVE);
        } else {
            err = schedule_transition(WIFI_TRANSITION_AP_ONLY);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to queue recovery transition: %s", esp_err_to_name(err));
        }
        return ESP_ERR_TIMEOUT;
    }

    err = write_active_sta_credentials(ssid, password ? password : "");
    if (err != ESP_OK) {
        if (can_restore) {
            esp_err_t restore_err = schedule_transition(WIFI_TRANSITION_RESTORE_ACTIVE);
            if (restore_err != ESP_OK) {
                ESP_LOGE(TAG, "failed to queue restore after NVS write failure: %s", esp_err_to_name(restore_err));
            }
        } else {
            esp_err_t ap_err = schedule_transition(WIFI_TRANSITION_AP_ONLY);
            if (ap_err != ESP_OK) {
                ESP_LOGE(TAG, "failed to queue AP fallback after NVS write failure: %s", esp_err_to_name(ap_err));
            }
        }
        return err;
    }

    set_restore_sta_state(ssid, password ? password : "");

    err = schedule_transition(WIFI_TRANSITION_STA_ONLY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA-only transition scheduling failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "new active STA config validated and saved: %s", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_clear_sta_config(void)
{
    cancel_transition_task();

    esp_err_t err = erase_active_sta_credentials();
    if (err != ESP_OK) {
        return err;
    }

    if (s_mode != WIFI_MGR_MODE_AP || s_connected) {
        err = schedule_transition(WIFI_TRANSITION_AP_ONLY);
        if (err != ESP_OK) {
            return err;
        }
    }

    ESP_LOGI(TAG, "active STA credentials cleared");
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

bool wifi_manager_has_sta_config(void)
{
    return s_has_active_sta_config;
}

bool wifi_manager_can_restore_sta_config(void)
{
    return s_has_restore_sta_config;
}
