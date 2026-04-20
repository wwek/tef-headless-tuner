#include "web_server.h"
#include "tuner_controller.h"
#include "wifi_manager.h"
#include "version.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web_server";

// Embedded HTML
extern const char html_index_start[] asm("_binary_index_html_start");
extern const char html_index_end[]   asm("_binary_index_html_end");
extern const char html_wifi_start[]  asm("_binary_wifi_html_start");
extern const char html_wifi_end[]    asm("_binary_wifi_html_end");
extern const char html_system_start[] asm("_binary_system_html_start");
extern const char html_system_end[]   asm("_binary_system_html_end");

static httpd_handle_t s_server;

static esp_err_t set_cors_headers(httpd_req_t *req);

static const char *wifi_mode_name(wifi_mgr_mode_t mode)
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

// --- SSE ---

#define MAX_SSE_CLIENTS 4
#define SSE_REQ_QUEUE_LEN   MAX_SSE_CLIENTS
#define SSE_REQ_ITEM_SIZE   sizeof(httpd_req_t *)
#define SSE_MSG_QUEUE_LEN   16
#define SSE_MSG_ITEM_SIZE   sizeof(char *)
#define SSE_WORKER_STACK 4096
#define SSE_WORKER_PRIO  5
#define SSE_KEEPALIVE_MS 1000

typedef struct {
    httpd_req_t *req;
    bool active;
} sse_client_t;

static sse_client_t s_sse_clients[MAX_SSE_CLIENTS];
static QueueHandle_t s_sse_req_queue;
static QueueHandle_t s_sse_msg_queue;
static TaskHandle_t s_sse_worker_task;

static void sse_remove_client(int idx)
{
    if (idx < 0 || idx >= MAX_SSE_CLIENTS) return;
    sse_client_t *c = &s_sse_clients[idx];
    c->req = NULL;
    c->active = false;
}

static int sse_find_slot(void)
{
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (!s_sse_clients[i].active) return i;
    }
    return -1;
}

static void sse_complete_client(int idx)
{
    if (idx < 0 || idx >= MAX_SSE_CLIENTS) {
        return;
    }
    sse_client_t *c = &s_sse_clients[idx];
    if (c->req) {
        httpd_req_async_handler_complete(c->req);
    }
    sse_remove_client(idx);
}

static void sse_send_to_active_clients(const char *msg)
{
    if (!s_server) {
        return;
    }

    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        sse_client_t *c = &s_sse_clients[i];
        if (!c->active || !c->req) {
            continue;
        }
        esp_err_t err = httpd_resp_send_chunk(c->req, msg, strlen(msg));
        if (err != ESP_OK) {
            sse_complete_client(i);
        }
    }
}

static void sse_broadcast(const char *event, const char *json)
{
    // Format: "event: <name>\ndata: <json>\n\n"
    int len = strlen(event) + strlen(json) + 16;
    char *msg = malloc(len);
    if (!msg) return;
    snprintf(msg, len, "event: %s\ndata: %s\n\n", event, json);
    if (!s_sse_msg_queue || xQueueSend(s_sse_msg_queue, &msg, 0) != pdTRUE) {
        free(msg);
    }
}

static void sse_worker(void *ctx)
{
    (void)ctx;
    for (;;) {
        httpd_req_t *async_req = NULL;
        while (xQueueReceive(s_sse_req_queue, &async_req, 0) == pdTRUE) {
            if (!async_req) {
                continue;
            }

            int slot = sse_find_slot();
            if (slot < 0) {
                httpd_resp_send_err(async_req, HTTPD_500_INTERNAL_SERVER_ERROR, "too many SSE clients");
                httpd_req_async_handler_complete(async_req);
                continue;
            }

            s_sse_clients[slot].req = async_req;
            s_sse_clients[slot].active = true;

            if (set_cors_headers(async_req) != ESP_OK ||
                httpd_resp_set_type(async_req, "text/event-stream") != ESP_OK ||
                httpd_resp_set_hdr(async_req, "Cache-Control", "no-cache") != ESP_OK ||
                httpd_resp_set_hdr(async_req, "Connection", "keep-alive") != ESP_OK ||
                httpd_resp_send_chunk(async_req, ": connected\n\n", strlen(": connected\n\n")) != ESP_OK) {
                sse_complete_client(slot);
            }
        }

        char *msg = NULL;
        if (xQueueReceive(s_sse_msg_queue, &msg, pdMS_TO_TICKS(SSE_KEEPALIVE_MS)) == pdTRUE) {
            if (msg) {
                sse_send_to_active_clients(msg);
                free(msg);
            }
        } else {
            sse_send_to_active_clients(": keepalive\n\n");
        }
    }
}

static esp_err_t sse_submit_async(httpd_req_t *req)
{
    if (!s_sse_req_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_req_t *copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) {
        return err;
    }

    if (xQueueSend(s_sse_req_queue, &copy, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_req_async_handler_complete(copy);
        return ESP_ERR_HTTPD_HANDLERS_FULL;
    }

    return ESP_OK;
}

// Status callback registered with cmd_handler
static void status_cb(const tuner_state_t *state, bool rds_changed, void *ctx)
{
    (void)ctx;

    // Status event
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "band", tef_band_name(state->status.band));
    cJSON_AddNumberToObject(root, "freq", state->status.frequency);
    cJSON_AddBoolToObject(root, "tuned", state->status.tuned);
    cJSON_AddBoolToObject(root, "stereo", state->status.stereo);
    cJSON_AddBoolToObject(root, "rds", state->status.rds_sync);
    cJSON_AddNumberToObject(root, "level", state->status.level / 10.0);
    cJSON_AddNumberToObject(root, "snr", state->status.snr);
    cJSON_AddBoolToObject(root, "seeking", state->seeking);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    sse_broadcast("status", json);
    free(json);

    // Quality event
    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "level", state->quality.level / 10.0);
    cJSON_AddNumberToObject(root, "usn", state->quality.usn);
    cJSON_AddNumberToObject(root, "wam", state->quality.wam);
    cJSON_AddNumberToObject(root, "offset", state->quality.offset);
    cJSON_AddNumberToObject(root, "bw", state->quality.bandwidth);
    cJSON_AddNumberToObject(root, "mod", state->quality.modulation);
    cJSON_AddNumberToObject(root, "snr", state->quality.snr);
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    sse_broadcast("quality", json);
    free(json);

    // RDS event
    if (rds_changed && state->rds.has_data) {
        root = cJSON_CreateObject();
        char pi[5];
        snprintf(pi, sizeof(pi), "%04X", state->rds.pi);
        cJSON_AddStringToObject(root, "pi", pi);
        cJSON_AddStringToObject(root, "ps", state->rds.ps);
        cJSON_AddStringToObject(root, "rt", state->rds.rt);
        cJSON_AddNumberToObject(root, "pty", state->rds.pty);
        cJSON_AddBoolToObject(root, "tp", state->rds.tp);
        cJSON_AddBoolToObject(root, "ta", state->rds.ta);
        cJSON_AddBoolToObject(root, "ms", state->rds.ms);
        json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        sse_broadcast("rds", json);
        free(json);
    }
}

// --- Helpers ---

static esp_err_t set_cors_headers(httpd_req_t *req)
{
    esp_err_t err;
    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err != ESP_OK) return err;
    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    if (err != ESP_OK) return err;
    return httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static esp_err_t send_json_response(httpd_req_t *req, cJSON *root)
{
    esp_err_t err = set_cors_headers(req);
    if (err != ESP_OK) return err;
    err = httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) return err;
    char *json = cJSON_PrintUnformatted(root);
    err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static cJSON *parse_body(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 256) return NULL;
    char buf[257];
    int received = httpd_req_recv(req, buf, len);
    if (received <= 0) return NULL;
    buf[received] = '\0';
    return cJSON_Parse(buf);
}

static void add_wifi_state(cJSON *root)
{
    char ap_ssid[32] = {0};
    const char *ip = wifi_manager_get_ip();
    wifi_mgr_mode_t mode = wifi_manager_get_mode();

    wifi_manager_get_ap_ssid(ap_ssid, sizeof(ap_ssid));
    cJSON_AddStringToObject(root, "mode", wifi_mode_name(mode));
    cJSON_AddBoolToObject(root, "connected", wifi_manager_is_connected());
    cJSON_AddBoolToObject(root, "has_saved", wifi_manager_has_sta_config());
    cJSON_AddStringToObject(root, "ap_ssid", ap_ssid);
    cJSON_AddStringToObject(root, "ip", ip ? ip : "");
}

static void add_system_state(cJSON *root)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    cJSON_AddStringToObject(root, "version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "commit", FIRMWARE_COMMIT);
    cJSON_AddStringToObject(root, "branch", FIRMWARE_BRANCH);
    cJSON_AddStringToObject(root, "date", FIRMWARE_BUILD_DATE);
    cJSON_AddStringToObject(root, "running_partition", running ? running->label : "");
    cJSON_AddStringToObject(root, "boot_partition", boot ? boot->label : "");
    cJSON_AddStringToObject(root, "next_partition", next ? next->label : "");
}

// --- URI handlers ---

static esp_err_t h_index(httpd_req_t *req)
{
    esp_err_t err = set_cors_headers(req);
    if (err != ESP_OK) return err;
    err = httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (err != ESP_OK) return err;
    size_t len = html_index_end - html_index_start;
    ESP_LOGI(TAG, "HTTP GET %s -> index (%u bytes)", req->uri, (unsigned)len);
    return httpd_resp_send(req, html_index_start, len);
}

static esp_err_t h_wifi_page(httpd_req_t *req)
{
    char ap_ssid[32] = {0};
    const char *ip = wifi_manager_get_ip();

    esp_err_t err = set_cors_headers(req);
    if (err != ESP_OK) return err;
    err = httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (err != ESP_OK) return err;
    size_t len = html_wifi_end - html_wifi_start;
    wifi_manager_get_ap_ssid(ap_ssid, sizeof(ap_ssid));
    ESP_LOGI(TAG,
             "HTTP GET %s -> wifi page (%u bytes), mode=%d, ap_ssid=%s, ip=%s",
             req->uri,
             (unsigned)len,
             (int)wifi_manager_get_mode(),
             ap_ssid[0] ? ap_ssid : "<unset>",
             ip ? ip : "<none>");
    return httpd_resp_send(req, html_wifi_start, len);
}

static esp_err_t h_system_page(httpd_req_t *req)
{
    esp_err_t err = set_cors_headers(req);
    if (err != ESP_OK) return err;
    err = httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (err != ESP_OK) return err;
    size_t len = html_system_end - html_system_start;
    ESP_LOGI(TAG, "HTTP GET %s -> system page (%u bytes)", req->uri, (unsigned)len);
    return httpd_resp_send(req, html_system_start, len);
}

static esp_err_t h_get_version(httpd_req_t *req)
{
    esp_err_t err = set_cors_headers(req);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "commit", FIRMWARE_COMMIT);
    cJSON_AddStringToObject(root, "branch", FIRMWARE_BRANCH);
    cJSON_AddStringToObject(root, "date", FIRMWARE_BUILD_DATE);
    err = send_json_response(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t h_get_wifi(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    add_wifi_state(root);
    esp_err_t err = send_json_response(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t h_get_system(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    add_system_state(root);
    esp_err_t err = send_json_response(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t h_get_status(httpd_req_t *req)
{
    tuner_state_t state = tuner_controller_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "band", tef_band_name(state.status.band));
    cJSON_AddNumberToObject(root, "freq", state.status.frequency);
    cJSON_AddBoolToObject(root, "tuned", state.status.tuned);
    cJSON_AddBoolToObject(root, "stereo", state.status.stereo);
    cJSON_AddBoolToObject(root, "rds", state.status.rds_sync);
    cJSON_AddNumberToObject(root, "level", state.status.level / 10.0);
    cJSON_AddNumberToObject(root, "snr", state.status.snr);
    cJSON_AddBoolToObject(root, "seeking", state.seeking);
    esp_err_t err = send_json_response(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t h_get_quality(httpd_req_t *req)
{
    tuner_state_t state = tuner_controller_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "level", state.quality.level / 10.0);
    cJSON_AddNumberToObject(root, "usn", state.quality.usn);
    cJSON_AddNumberToObject(root, "wam", state.quality.wam);
    cJSON_AddNumberToObject(root, "offset", state.quality.offset);
    cJSON_AddNumberToObject(root, "bw", state.quality.bandwidth);
    cJSON_AddNumberToObject(root, "mod", state.quality.modulation);
    cJSON_AddNumberToObject(root, "snr", state.quality.snr);
    esp_err_t err = send_json_response(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t h_get_rds(httpd_req_t *req)
{
    tuner_state_t state = tuner_controller_get_state();
    cJSON *root = cJSON_CreateObject();
    char pi[5];
    snprintf(pi, sizeof(pi), "%04X", state.rds.pi);
    cJSON_AddStringToObject(root, "pi", pi);
    cJSON_AddStringToObject(root, "ps", state.rds.ps);
    cJSON_AddStringToObject(root, "rt", state.rds.rt);
    cJSON_AddNumberToObject(root, "pty", state.rds.pty);
    cJSON_AddBoolToObject(root, "tp", state.rds.tp);
    cJSON_AddBoolToObject(root, "ta", state.rds.ta);
    cJSON_AddBoolToObject(root, "ms", state.rds.ms);
    esp_err_t err = send_json_response(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t h_post_tune(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    const cJSON *jfreq = cJSON_GetObjectItem(body, "freq");
    const cJSON *jband = cJSON_GetObjectItem(body, "band");
    if (!cJSON_IsNumber(jfreq) || !cJSON_IsString(jband)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing freq or band");
        return ESP_FAIL;
    }

    uint32_t freq = (uint32_t)jfreq->valuedouble;
    const char *band = jband->valuestring;
    esp_err_t err;

    if (strcasecmp(band, "FM") == 0) {
        if (freq < 64000U || freq > 108000U) {
            cJSON_Delete(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "FM frequency out of range");
            return ESP_FAIL;
        }
        err = tuner_controller_tune_fm(freq);
    } else if (strcasecmp(band, "LW") == 0) {
        if (freq < 144U || freq > 519U) {
            cJSON_Delete(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "LW frequency out of range");
            return ESP_FAIL;
        }
        err = tuner_controller_tune_am(freq);
    } else if (strcasecmp(band, "MW") == 0) {
        if (freq < 520U || freq > 1710U) {
            cJSON_Delete(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "MW frequency out of range");
            return ESP_FAIL;
        }
        err = tuner_controller_tune_am(freq);
    } else if (strcasecmp(band, "SW") == 0) {
        if (freq < 1711U || freq > 27000U) {
            cJSON_Delete(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SW frequency out of range");
            return ESP_FAIL;
        }
        err = tuner_controller_tune_am(freq);
    } else {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported band");
        return ESP_FAIL;
    }

    cJSON_Delete(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tune failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "tune failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_post_seek(httpd_req_t *req)
{
    esp_err_t err;
    cJSON *body = parse_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    const cJSON *jdir = cJSON_GetObjectItem(body, "direction");
    const cJSON *jband = cJSON_GetObjectItem(body, "band");
    if (!cJSON_IsString(jdir) || !cJSON_IsString(jband)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing direction or band");
        return ESP_FAIL;
    }

    bool up;
    bool is_am;

    if (strcasecmp(jdir->valuestring, "UP") == 0) {
        up = true;
    } else if (strcasecmp(jdir->valuestring, "DOWN") == 0) {
        up = false;
    } else {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "direction must be UP or DOWN");
        return ESP_FAIL;
    }

    if (strcasecmp(jband->valuestring, "FM") == 0) {
        is_am = false;
    } else if (strcasecmp(jband->valuestring, "LW") == 0
            || strcasecmp(jband->valuestring, "MW") == 0
            || strcasecmp(jband->valuestring, "SW") == 0) {
        is_am = true;
    } else {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported band");
        return ESP_FAIL;
    }

    tuner_controller_abort_seek();
    err = tuner_controller_start_seek(up, is_am);
    if (err != ESP_OK) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "seek failed");
        return ESP_FAIL;
    }

    cJSON_Delete(body);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_post_seekstop(httpd_req_t *req)
{
    tuner_controller_abort_seek();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_post_volume(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    const cJSON *jvol = cJSON_GetObjectItem(body, "volume");
    if (!cJSON_IsNumber(jvol)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing volume");
        return ESP_FAIL;
    }

    int vol = jvol->valueint;
    if (vol < 0) vol = 0;
    if (vol > 30) vol = 30;
    tuner_controller_set_volume((uint8_t)vol);

    cJSON_Delete(body);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_post_mute(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    const cJSON *jmute = cJSON_GetObjectItem(body, "mute");
    if (!cJSON_IsBool(jmute)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing mute");
        return ESP_FAIL;
    }

    tuner_controller_set_mute(cJSON_IsTrue(jmute));

    cJSON_Delete(body);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_post_wifi(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    const cJSON *jssid = cJSON_GetObjectItem(body, "ssid");
    const cJSON *jpass = cJSON_GetObjectItem(body, "password");
    if (!cJSON_IsString(jssid) || !cJSON_IsString(jpass)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid or password");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HTTP POST %s -> validate Wi-Fi candidate for SSID=%s", req->uri, jssid->valuestring);
    esp_err_t err = wifi_manager_set_sta_config(jssid->valuestring, jpass->valuestring);
    cJSON_Delete(body);

    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid wifi parameters");
        return ESP_FAIL;
    }

    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "wifi config failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wifi config failed");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(root, "saved", err == ESP_OK);
    cJSON_AddBoolToObject(root, "applied", err == ESP_OK);
    cJSON_AddBoolToObject(root, "rollback", err == ESP_ERR_TIMEOUT && wifi_manager_can_restore_sta_config());
    add_wifi_state(root);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi candidate validated and promoted to active config");
    } else {
        ESP_LOGW(TAG, "Wi-Fi candidate validation failed; active config unchanged");
    }

    esp_err_t send_err = send_json_response(req, root);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t h_post_wifi_clear(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP POST %s -> clear saved STA config", req->uri);

    esp_err_t err = wifi_manager_clear_sta_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "clear wifi config failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "clear wifi config failed");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "cleared", true);
    add_wifi_state(root);
    esp_err_t send_err = send_json_response(req, root);
    cJSON_Delete(root);
    return send_err;
}

// SSE handler: long-lived connection
static esp_err_t h_sse_events(httpd_req_t *req)
{
    esp_err_t err = sse_submit_async(req);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err == ESP_ERR_HTTPD_HANDLERS_FULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "too many SSE clients");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sse unavailable");
    }
    return ESP_FAIL;
}

// --- Registration ---

typedef struct {
    const char  *uri;
    httpd_method_t method;
    esp_err_t   (*handler)(httpd_req_t *req);
} uri_entry_t;

static const uri_entry_t s_uris[] = {
    { "/",              HTTP_GET,  h_index        },
    { "/wifi",          HTTP_GET,  h_wifi_page     },
    { "/system",        HTTP_GET,  h_system_page   },
    { "/api/wifi",      HTTP_GET,  h_get_wifi      },
    { "/api/system",    HTTP_GET,  h_get_system    },
    { "/api/status",    HTTP_GET,  h_get_status    },
    { "/api/tune",      HTTP_POST, h_post_tune     },
    { "/api/seek",      HTTP_POST, h_post_seek     },
    { "/api/seekstop",  HTTP_POST, h_post_seekstop },
    { "/api/volume",    HTTP_POST, h_post_volume   },
    { "/api/mute",      HTTP_POST, h_post_mute     },
    { "/api/quality",   HTTP_GET,  h_get_quality   },
    { "/api/rds",       HTTP_GET,  h_get_rds       },
    { "/api/events",    HTTP_GET,  h_sse_events    },
    { "/api/wifi",      HTTP_POST, h_post_wifi     },
    { "/api/wifi/clear", HTTP_POST, h_post_wifi_clear },
    { "/api/version",   HTTP_GET,  h_get_version   },
};

#define NUM_URIS (sizeof(s_uris) / sizeof(s_uris[0]))

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = 8192;
    config.max_uri_handlers  = 18;
    config.max_resp_headers  = 8;
    config.uri_match_fn      = httpd_uri_match_wildcard;

    ESP_LOGI(TAG,
             "http server config: req_hdr_max=%d, uri_max=%d, handlers=%u, stack=%u",
             HTTPD_MAX_REQ_HDR_LEN,
             HTTPD_MAX_URI_LEN,
             (unsigned)config.max_uri_handlers,
             (unsigned)config.stack_size);

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sse_req_queue = xQueueCreate(SSE_REQ_QUEUE_LEN, SSE_REQ_ITEM_SIZE);
    s_sse_msg_queue = xQueueCreate(SSE_MSG_QUEUE_LEN, SSE_MSG_ITEM_SIZE);
    if (!s_sse_req_queue || !s_sse_msg_queue) {
        if (s_sse_req_queue) {
            vQueueDelete(s_sse_req_queue);
            s_sse_req_queue = NULL;
        }
        if (s_sse_msg_queue) {
            vQueueDelete(s_sse_msg_queue);
            s_sse_msg_queue = NULL;
        }
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGE(TAG, "failed to create SSE worker resources");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(sse_worker, "sse_worker", SSE_WORKER_STACK, NULL, SSE_WORKER_PRIO, &s_sse_worker_task) != pdPASS) {
        vQueueDelete(s_sse_req_queue);
        s_sse_req_queue = NULL;
        vQueueDelete(s_sse_msg_queue);
        s_sse_msg_queue = NULL;
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGE(TAG, "failed to create SSE worker task");
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < NUM_URIS; i++) {
        httpd_uri_t uri = {
            .uri      = s_uris[i].uri,
            .method   = s_uris[i].method,
            .handler  = s_uris[i].handler,
            .user_ctx = NULL,
        };
        err = httpd_register_uri_handler(s_server, &uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", s_uris[i].uri, esp_err_to_name(err));
            httpd_stop(s_server);
            s_server = NULL;
            return err;
        }
    }

    tuner_controller_register_cb(status_cb, NULL);
    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (!s_server) return;

    tuner_controller_unregister_cb(status_cb);

    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (s_sse_clients[i].active) {
            sse_complete_client(i);
        }
    }

    if (s_sse_worker_task) {
        vTaskDelete(s_sse_worker_task);
        s_sse_worker_task = NULL;
    }
    if (s_sse_req_queue) {
        httpd_req_t *pending_req = NULL;
        while (xQueueReceive(s_sse_req_queue, &pending_req, 0) == pdTRUE) {
            if (pending_req) {
                httpd_req_async_handler_complete(pending_req);
            }
        }
        vQueueDelete(s_sse_req_queue);
        s_sse_req_queue = NULL;
    }
    if (s_sse_msg_queue) {
        char *pending_msg = NULL;
        while (xQueueReceive(s_sse_msg_queue, &pending_msg, 0) == pdTRUE) {
            free(pending_msg);
        }
        vQueueDelete(s_sse_msg_queue);
        s_sse_msg_queue = NULL;
    }

    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "web server stopped");
}
