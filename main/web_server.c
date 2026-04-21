#include "web_server.h"
#include "app_settings.h"
#include "audio.h"
#include "tuner_controller.h"
#include "wifi_manager.h"
#include "version.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include <inttypes.h>
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
static void sse_broadcast(const char *event, const char *json);

static void close_http_session_after_response(httpd_req_t *req)
{
    if (req == NULL || req->handle == NULL) {
        return;
    }

    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        (void)httpd_sess_trigger_close(req->handle, sockfd);
    }
}

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

static const char *chip_model_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:
        return "ESP32";
    case CHIP_ESP32S2:
        return "ESP32-S2";
    case CHIP_ESP32S3:
        return "ESP32-S3";
    case CHIP_ESP32C3:
        return "ESP32-C3";
    case CHIP_ESP32C2:
        return "ESP32-C2";
    case CHIP_ESP32C6:
        return "ESP32-C6";
    case CHIP_ESP32H2:
        return "ESP32-H2";
    case CHIP_ESP32P4:
        return "ESP32-P4";
    case CHIP_ESP32C61:
        return "ESP32-C61";
#if defined(CHIP_ESP32C5)
    case CHIP_ESP32C5:
        return "ESP32-C5";
#endif
    case CHIP_POSIX_LINUX:
        return "POSIX";
    default:
        return "UNKNOWN";
    }
}

static uint32_t configured_flash_size_bytes(void)
{
#if CONFIG_ESPTOOLPY_FLASHSIZE_1MB
    return 1U * 1024U * 1024U;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_2MB
    return 2U * 1024U * 1024U;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_4MB
    return 4U * 1024U * 1024U;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_8MB
    return 8U * 1024U * 1024U;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_16MB
    return 16U * 1024U * 1024U;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_32MB
    return 32U * 1024U * 1024U;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_64MB
    return 64U * 1024U * 1024U;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_128MB
    return 128U * 1024U * 1024U;
#else
    return 0;
#endif
}

static uint32_t get_flash_size_bytes(void)
{
    uint32_t flash_size = 0;

    if (esp_flash_get_physical_size(NULL, &flash_size) == ESP_OK && flash_size > 0) {
        return flash_size;
    }
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK && flash_size > 0) {
        return flash_size;
    }

    return configured_flash_size_bytes();
}

static const char *get_preferred_wifi_ip(char *buf, size_t buf_len, wifi_mgr_mode_t mode)
{
    const char *sta_ip = wifi_manager_get_ip();
    if (sta_ip && buf && buf_len > 0) {
        strncpy(buf, sta_ip, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return buf;
    }

    if (mode == WIFI_MGR_MODE_AP || mode == WIFI_MGR_MODE_APSTA) {
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t ip_info = {0};
        if (ap_netif != NULL &&
            esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0 &&
            buf && buf_len > 0) {
            snprintf(buf, buf_len, IPSTR, IP2STR(&ip_info.ip));
            return buf;
        }
    }

    return NULL;
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

// --- Spectrum scan ---

#define SPECTRUM_SCAN_STACK      4096
#define SPECTRUM_SCAN_PRIO       4
#define SPECTRUM_SETTLE_MS       20
#define SPECTRUM_STEP_KHZ        100U
#define SPECTRUM_BANDWIDTH_KHZ   56U
#define SPECTRUM_OIRT_START_KHZ  64000U
#define SPECTRUM_OIRT_END_KHZ    86000U
#define SPECTRUM_FM_START_KHZ    86000U
#define SPECTRUM_FM_END_KHZ      108000U

typedef struct {
    uint32_t scan_id;
    uint32_t start_khz;
    uint32_t end_khz;
    uint32_t step_khz;
    uint16_t bandwidth_khz;
} spectrum_scan_req_t;

static SemaphoreHandle_t s_spectrum_mutex;
static TaskHandle_t s_spectrum_task;
static bool s_spectrum_active;
static uint32_t s_spectrum_scan_id;
static volatile bool s_spectrum_cancel_requested;

static size_t spectrum_count_points(uint32_t start_khz, uint32_t end_khz, uint32_t step_khz)
{
    if (step_khz == 0 || end_khz < start_khz) {
        return 0;
    }

    return ((end_khz - start_khz) / step_khz) + 1U;
}

static void spectrum_set_active(bool active)
{
    if (!s_spectrum_mutex) {
        s_spectrum_active = active;
        return;
    }

    xSemaphoreTake(s_spectrum_mutex, portMAX_DELAY);
    s_spectrum_active = active;
    xSemaphoreGive(s_spectrum_mutex);
}

static bool spectrum_try_begin(uint32_t *scan_id_out)
{
    bool ok = false;

    if (!s_spectrum_mutex) {
        return false;
    }

    xSemaphoreTake(s_spectrum_mutex, portMAX_DELAY);
    if (!s_spectrum_active) {
        s_spectrum_active = true;
        s_spectrum_scan_id++;
        if (scan_id_out) {
            *scan_id_out = s_spectrum_scan_id;
        }
        ok = true;
    }
    xSemaphoreGive(s_spectrum_mutex);

    return ok;
}

static void spectrum_resolve_range(const tuner_state_t *state, uint32_t *start_khz, uint32_t *end_khz)
{
    if (state && state->active_band == TEF_BAND_FM &&
        state->status.frequency >= SPECTRUM_OIRT_START_KHZ &&
        state->status.frequency < SPECTRUM_OIRT_END_KHZ) {
        *start_khz = SPECTRUM_OIRT_START_KHZ;
        *end_khz = SPECTRUM_OIRT_END_KHZ;
        return;
    }

    *start_khz = SPECTRUM_FM_START_KHZ;
    *end_khz = SPECTRUM_FM_END_KHZ;
}

static void spectrum_broadcast_status(const char *state, const spectrum_scan_req_t *req)
{
    char json[192];
    size_t total = spectrum_count_points(req->start_khz, req->end_khz, req->step_khz);

    snprintf(json,
             sizeof(json),
             "{\"state\":\"%s\",\"scan_id\":%" PRIu32 ",\"start\":%" PRIu32 ","
             "\"end\":%" PRIu32 ",\"step\":%" PRIu32 ",\"bw\":%u,\"total\":%u}",
             state,
             req->scan_id,
             req->start_khz,
             req->end_khz,
             req->step_khz,
             (unsigned)req->bandwidth_khz,
             (unsigned)total);
    sse_broadcast("spectrum_status", json);
}

static void spectrum_broadcast_point(const spectrum_scan_req_t *req,
                                     size_t index,
                                     uint32_t freq_khz,
                                     int16_t level_tenth_db)
{
    char json[160];

    snprintf(json,
             sizeof(json),
             "{\"scan_id\":%" PRIu32 ",\"index\":%u,\"freq\":%" PRIu32 ",\"level\":%.1f}",
             req->scan_id,
             (unsigned)index,
             freq_khz,
             level_tenth_db / 10.0f);
    sse_broadcast("spectrum_point", json);
}

static void spectrum_scan_task(void *arg)
{
    spectrum_scan_req_t *req = (spectrum_scan_req_t *)arg;
    tuner_state_t saved_state = tuner_controller_get_state();
    tuner_xdr_settings_t saved_settings = {0};
    esp_err_t err = ESP_OK;

    tuner_controller_get_xdr_settings(&saved_settings);
    spectrum_broadcast_status("running", req);

    err = tuner_controller_set_scan_mute(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spectrum scan mute failed: %s", esp_err_to_name(err));
    }

    if (err == ESP_OK && req->bandwidth_khz > 0 && saved_settings.fm_bandwidth_khz != req->bandwidth_khz) {
        esp_err_t bw_err = tuner_controller_set_bandwidth_fm(req->bandwidth_khz);
        if (bw_err != ESP_OK) {
            ESP_LOGW(TAG, "spectrum scan bandwidth set failed: %s", esp_err_to_name(bw_err));
        }
    }

    if (err == ESP_OK) {
        size_t index = 0;

        for (uint32_t freq_khz = req->start_khz;
             freq_khz <= req->end_khz;
             freq_khz += req->step_khz, index++) {
            if (s_spectrum_cancel_requested) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }

            int16_t level_tenth_db = -200;
            esp_err_t tune_err = tuner_controller_tune_fm(freq_khz);

            if (tune_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "spectrum tune failed at %" PRIu32 " kHz: %s",
                         freq_khz,
                         esp_err_to_name(tune_err));
            } else {
                vTaskDelay(pdMS_TO_TICKS(SPECTRUM_SETTLE_MS));
                if (s_spectrum_cancel_requested) {
                    err = ESP_ERR_INVALID_STATE;
                    break;
                }
                tuner_state_t state = tuner_controller_get_state();
                level_tenth_db = state.quality.level;
            }

            spectrum_broadcast_point(req, index, freq_khz, level_tenth_db);

            if (freq_khz > UINT32_MAX - req->step_khz) {
                break;
            }
        }
    }

    if (saved_settings.fm_bandwidth_khz != req->bandwidth_khz) {
        esp_err_t restore_bw_err = tuner_controller_set_bandwidth_fm(saved_settings.fm_bandwidth_khz);
        if (restore_bw_err != ESP_OK) {
            ESP_LOGW(TAG, "spectrum restore bandwidth failed: %s", esp_err_to_name(restore_bw_err));
        }
    }

    if (saved_state.active_band == TEF_BAND_FM) {
        esp_err_t restore_tune_err = tuner_controller_tune_fm(saved_state.status.frequency);
        if (restore_tune_err != ESP_OK) {
            ESP_LOGW(TAG, "spectrum restore tune failed: %s", esp_err_to_name(restore_tune_err));
            err = restore_tune_err;
        }
    }

    if (tuner_controller_set_scan_mute(false) != ESP_OK) {
        ESP_LOGW(TAG, "spectrum unmute restore failed");
    }

    app_settings_end_busy(APP_SETTINGS_BUSY_OWNER_WEB_SPECTRUM);
    spectrum_broadcast_status(err == ESP_OK ? "complete" : "error", req);
    spectrum_set_active(false);
    s_spectrum_cancel_requested = false;
    s_spectrum_task = NULL;
    free(req);
    vTaskDelete(NULL);
}

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

// --- WebSocket Audio Streaming ---

#define WS_AUDIO_MAX_CLIENTS  2
#define WS_AUDIO_RING_SIZE    (32 * 1024)
#define WS_AUDIO_SEND_STACK   6144
#define WS_AUDIO_SEND_PRIO    4
#define WS_AUDIO_CHUNK_SIZE   4096
#define WS_AUDIO_POLL_MS      10

typedef struct {
    int fd;
    volatile bool active;
} ws_audio_client_t;

static ws_audio_client_t s_ws_clients[WS_AUDIO_MAX_CLIENTS];
static RingbufHandle_t s_ws_audio_ring;
static TaskHandle_t s_ws_audio_task;
static volatile bool s_ws_audio_running;
static SemaphoreHandle_t s_ws_ring_mutex;

static bool ws_audio_has_clients(void)
{
    for (int i = 0; i < WS_AUDIO_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].active) return true;
    }
    return false;
}

static bool ws_audio_lock(TickType_t ticks_to_wait)
{
    return s_ws_ring_mutex && xSemaphoreTake(s_ws_ring_mutex, ticks_to_wait) == pdTRUE;
}

static void ws_audio_unlock(void)
{
    if (s_ws_ring_mutex) {
        xSemaphoreGive(s_ws_ring_mutex);
    }
}

static void ws_audio_reset_ring_locked(void)
{
    if (!s_ws_audio_ring) {
        return;
    }

    while (true) {
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_ws_audio_ring, &item_size, 0, WS_AUDIO_RING_SIZE);
        if (!item) {
            break;
        }
        vRingbufferReturnItem(s_ws_audio_ring, item);
    }
}

// Audio callback — runs in audio_task context (prio 6). Must be FAST.
static void ws_audio_data_cb(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (!s_ws_audio_ring || !ws_audio_has_clients()) return;
    if (!ws_audio_lock(0)) return;

    if (xRingbufferSend(s_ws_audio_ring, data, len, 0) != pdTRUE) {
        // Overflow — drop oldest to make room
        size_t dummy_size = 0;
        void *dummy = xRingbufferReceiveUpTo(s_ws_audio_ring, &dummy_size, 0, len);
        if (dummy) {
            vRingbufferReturnItem(s_ws_audio_ring, dummy);
        }
        (void)xRingbufferSend(s_ws_audio_ring, data, len, 0);
    }
    ws_audio_unlock();
}

// Completely independent task — never blocks httpd or audio_task.
static void ws_audio_sender_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ws audio sender task started");

    while (s_ws_audio_running) {
        bool ring_locked = false;

        vTaskDelay(pdMS_TO_TICKS(WS_AUDIO_POLL_MS));
        if (!ws_audio_has_clients() || !s_ws_audio_ring) {
            continue;
        }

        // Drain available data from ring buffer (mutex-protected)
        if (!ws_audio_lock(portMAX_DELAY)) {
            continue;
        }
        ring_locked = true;

        while (s_ws_audio_running) {
            size_t item_size = 0;
            uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(
                s_ws_audio_ring, &item_size, 0, WS_AUDIO_CHUNK_SIZE);
            if (!data || item_size == 0) {
                break;
            }

            // Copy data so we can release ring buffer item before potentially blocking send
            uint8_t *send_buf = malloc(item_size);
            if (!send_buf) {
                vRingbufferReturnItem(s_ws_audio_ring, data);
                break;
            }
            memcpy(send_buf, data, item_size);
            vRingbufferReturnItem(s_ws_audio_ring, data);
            ws_audio_unlock();
            ring_locked = false;

            httpd_ws_frame_t pkt = {
                .final = true,
                .fragmented = false,
                .type = HTTPD_WS_TYPE_BINARY,
                .payload = send_buf,
                .len = item_size,
            };

            for (int i = 0; i < WS_AUDIO_MAX_CLIENTS; i++) {
                if (!s_ws_clients[i].active) continue;
                esp_err_t err = httpd_ws_send_frame_async(s_server, s_ws_clients[i].fd, &pkt);
                if (err != ESP_OK) {
                    ESP_LOGD(TAG, "ws audio send failed fd=%d, removing", s_ws_clients[i].fd);
                    s_ws_clients[i].active = false;
                    s_ws_clients[i].fd = -1;
                }
            }

            free(send_buf);

            if (!s_ws_audio_running) {
                break;
            }
            if (!ws_audio_lock(portMAX_DELAY)) {
                break;
            }
            ring_locked = true;
        }

        if (ring_locked) {
            ws_audio_unlock();
        }
    }

    s_ws_audio_task = NULL;
    vTaskDelete(NULL);
}

// Called by httpd when ANY session closes — clean up WS audio clients.
static void http_session_close_cb(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    for (int i = 0; i < WS_AUDIO_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].active && s_ws_clients[i].fd == sockfd) {
            ESP_LOGI(TAG, "ws audio client disconnected fd=%d", sockfd);
            s_ws_clients[i].active = false;
            s_ws_clients[i].fd = -1;
        }
    }

    close(sockfd);
}

// WS /api/audio handler — only handles handshake + incoming control frames.
static esp_err_t h_ws_audio(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        bool had_clients = ws_audio_has_clients();

        // Pre-handshake: find a slot for this client
        int slot = -1;
        for (int i = 0; i < WS_AUDIO_MAX_CLIENTS; i++) {
            if (!s_ws_clients[i].active) { slot = i; break; }
        }
        if (slot < 0) {
            ESP_LOGW(TAG, "ws audio: no client slots");
            return ESP_FAIL;
        }

        int fd = httpd_req_to_sockfd(req);
        s_ws_clients[slot].fd = fd;
        s_ws_clients[slot].active = true;

        if (!had_clients && ws_audio_lock(pdMS_TO_TICKS(100))) {
            ws_audio_reset_ring_locked();
            ws_audio_unlock();
        }

        ESP_LOGI(TAG, "ws audio client connected fd=%d slot=%d", fd, slot);
        return ESP_OK;
    }

    // After handshake: handle incoming frames (close/ping)
    httpd_ws_frame_t pkt;
    uint8_t buf[4] = {0};
    memset(&pkt, 0, sizeof(pkt));
    pkt.payload = buf;

    if (httpd_ws_recv_frame(req, &pkt, sizeof(buf)) != ESP_OK) {
        return ESP_FAIL;
    }

    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        for (int i = 0; i < WS_AUDIO_MAX_CLIENTS; i++) {
            if (s_ws_clients[i].active && s_ws_clients[i].fd == fd) {
                s_ws_clients[i].active = false;
                s_ws_clients[i].fd = -1;
                break;
            }
        }
        // Echo close frame back
        pkt.type = HTTPD_WS_TYPE_CLOSE;
        httpd_ws_send_frame(req, &pkt);
    }

    return ESP_OK;
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

static esp_err_t set_short_response_headers(httpd_req_t *req)
{
    esp_err_t err = set_cors_headers(req);
    if (err != ESP_OK) return err;
    return httpd_resp_set_hdr(req, "Connection", "close");
}

static esp_err_t send_json_response(httpd_req_t *req, cJSON *root)
{
    if (root == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"json allocation failed\"}");
        close_http_session_after_response(req);
        return ESP_FAIL;
    }

    esp_err_t err = set_short_response_headers(req);
    if (err != ESP_OK) return err;
    err = httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) return err;
    char *json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        ESP_LOGE(TAG, "JSON print failed for %s", req->uri);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"json print failed\"}");
        close_http_session_after_response(req);
        return ESP_FAIL;
    }
    err = httpd_resp_sendstr(req, json);
    free(json);
    if (err == ESP_OK) {
        close_http_session_after_response(req);
    }
    return err;
}

static cJSON *parse_body(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 256) {
        return NULL;
    }

    char buf[257];
    int offset = 0;
    int timeout_count = 0;
    while (offset < len) {
        int received = httpd_req_recv(req, buf + offset, len - offset);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            timeout_count++;
            if (timeout_count >= 3) {
                return NULL;
            }
            continue;
        }
        if (received <= 0) {
            return NULL;
        }
        timeout_count = 0;
        offset += received;
    }

    buf[len] = '\0';
    return cJSON_ParseWithLength(buf, (size_t)len);
}

static void add_wifi_state(cJSON *root)
{
    if (root == NULL) {
        return;
    }

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
    if (root == NULL) {
        return;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_chip_info_t chip_info = {0};
    wifi_mgr_mode_t wifi_mode = wifi_manager_get_mode();
    bool psram_enabled = esp_psram_is_initialized();
    char wifi_ip[16] = {0};
    char ap_ssid[32] = {0};

    esp_chip_info(&chip_info);
    wifi_manager_get_ap_ssid(ap_ssid, sizeof(ap_ssid));

    cJSON_AddStringToObject(root, "version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "commit", FIRMWARE_COMMIT);
    cJSON_AddStringToObject(root, "branch", FIRMWARE_BRANCH);
    cJSON_AddStringToObject(root, "date", FIRMWARE_BUILD_DATE);
    cJSON_AddStringToObject(root, "running_partition", running ? running->label : "");
    cJSON_AddStringToObject(root, "boot_partition", boot ? boot->label : "");
    cJSON_AddStringToObject(root, "next_partition", next ? next->label : "");
    cJSON_AddStringToObject(root, "chip_model", chip_model_name(chip_info.model));
    cJSON_AddNumberToObject(root, "chip_revision", chip_info.revision);
    cJSON_AddNumberToObject(root, "cpu_cores", chip_info.cores);
    cJSON_AddNumberToObject(root, "flash_size_bytes", get_flash_size_bytes());
    cJSON_AddBoolToObject(root, "psram_enabled", psram_enabled);
    cJSON_AddNumberToObject(root, "psram_size_bytes", psram_enabled ? esp_psram_get_size() : 0);
    cJSON_AddNumberToObject(root, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "minimum_free_heap_bytes", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000LL);
    cJSON_AddStringToObject(root, "wifi_mode", wifi_mode_name(wifi_mode));
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected());
    cJSON_AddBoolToObject(root, "wifi_ap_active", wifi_mode != WIFI_MGR_MODE_STA);
    cJSON_AddStringToObject(root, "wifi_ip", get_preferred_wifi_ip(wifi_ip, sizeof(wifi_ip), wifi_mode) ?: "");
    cJSON_AddStringToObject(root, "wifi_ap_ssid", wifi_mode == WIFI_MGR_MODE_STA ? "" : ap_ssid);
}

// --- URI handlers ---

static esp_err_t h_index(httpd_req_t *req)
{
    esp_err_t err = set_short_response_headers(req);
    if (err != ESP_OK) return err;
    err = httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (err != ESP_OK) return err;
    size_t len = html_index_end - html_index_start;
    ESP_LOGI(TAG, "HTTP GET %s -> index (%u bytes)", req->uri, (unsigned)len);
    err = httpd_resp_send(req, html_index_start, len);
    if (err == ESP_OK) {
        close_http_session_after_response(req);
    }
    return err;
}

static esp_err_t h_wifi_page(httpd_req_t *req)
{
    char ap_ssid[32] = {0};
    const char *ip = wifi_manager_get_ip();

    esp_err_t err = set_short_response_headers(req);
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
    err = httpd_resp_send(req, html_wifi_start, len);
    if (err == ESP_OK) {
        close_http_session_after_response(req);
    }
    return err;
}

static esp_err_t h_system_page(httpd_req_t *req)
{
    esp_err_t err = set_short_response_headers(req);
    if (err != ESP_OK) return err;
    err = httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (err != ESP_OK) return err;
    size_t len = html_system_end - html_system_start;
    ESP_LOGI(TAG, "HTTP GET %s -> system page (%u bytes)", req->uri, (unsigned)len);
    err = httpd_resp_send(req, html_system_start, len);
    if (err == ESP_OK) {
        close_http_session_after_response(req);
    }
    return err;
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
    esp_err_t header_err = set_short_response_headers(req);
    if (header_err != ESP_OK) return header_err;

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
        err = app_settings_tune_fm(freq);
    } else if (strcasecmp(band, "LW") == 0) {
        if (freq < 144U || freq > 519U) {
            cJSON_Delete(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "LW frequency out of range");
            return ESP_FAIL;
        }
        err = app_settings_tune_am(freq);
    } else if (strcasecmp(band, "MW") == 0) {
        if (freq < 520U || freq > 1710U) {
            cJSON_Delete(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "MW frequency out of range");
            return ESP_FAIL;
        }
        err = app_settings_tune_am(freq);
    } else if (strcasecmp(band, "SW") == 0) {
        if (freq < 1711U || freq > 27000U) {
            cJSON_Delete(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SW frequency out of range");
            return ESP_FAIL;
        }
        err = app_settings_tune_am(freq);
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

    err = httpd_resp_sendstr(req, "{\"ok\":true}");
    if (err == ESP_OK) {
        close_http_session_after_response(req);
    }
    return err;
}

static esp_err_t h_post_seek(httpd_req_t *req)
{
    esp_err_t header_err = set_short_response_headers(req);
    if (header_err != ESP_OK) return header_err;

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
    err = app_settings_start_seek(up, is_am);
    if (err != ESP_OK) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "seek failed");
        return ESP_FAIL;
    }

    cJSON_Delete(body);
    err = httpd_resp_sendstr(req, "{\"ok\":true}");
    if (err == ESP_OK) {
        close_http_session_after_response(req);
    }
    return err;
}

static esp_err_t h_post_seekstop(httpd_req_t *req)
{
    esp_err_t header_err = set_short_response_headers(req);
    if (header_err != ESP_OK) return header_err;

    tuner_controller_abort_seek();
    esp_err_t err = httpd_resp_sendstr(req, "{\"ok\":true}");
    if (err == ESP_OK) {
        close_http_session_after_response(req);
    }
    return err;
}

static esp_err_t h_post_volume(httpd_req_t *req)
{
    esp_err_t header_err = set_short_response_headers(req);
    if (header_err != ESP_OK) return header_err;

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
    esp_err_t err = app_settings_set_volume((uint8_t)vol);
    cJSON_Delete(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set volume failed");
        return ESP_FAIL;
    }
    err = httpd_resp_sendstr(req, "{\"ok\":true}");
    if (err == ESP_OK) {
        close_http_session_after_response(req);
    }
    return err;
}

static esp_err_t h_post_mute(httpd_req_t *req)
{
    esp_err_t header_err = set_short_response_headers(req);
    if (header_err != ESP_OK) return header_err;

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

    esp_err_t err = app_settings_set_mute(cJSON_IsTrue(jmute));
    cJSON_Delete(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set mute failed");
        return ESP_FAIL;
    }
    err = httpd_resp_sendstr(req, "{\"ok\":true}");
    if (err == ESP_OK) {
        close_http_session_after_response(req);
    }
    return err;
}

static esp_err_t h_post_spectrum_scan(httpd_req_t *req)
{
    tuner_state_t state = tuner_controller_get_state();

    if (state.active_band != TEF_BAND_FM) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "spectrum is only available in FM band");
        return ESP_FAIL;
    }
    if (state.seeking) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "seek is active");
        return ESP_FAIL;
    }
    if (app_settings_begin_busy(APP_SETTINGS_BUSY_OWNER_WEB_SPECTRUM) != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "tuner busy");
        return ESP_FAIL;
    }

    uint32_t scan_id = 0;
    if (!spectrum_try_begin(&scan_id)) {
        app_settings_end_busy(APP_SETTINGS_BUSY_OWNER_WEB_SPECTRUM);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "spectrum scan busy");
        return ESP_FAIL;
    }

    spectrum_scan_req_t *scan = calloc(1, sizeof(*scan));
    if (!scan) {
        spectrum_set_active(false);
        app_settings_end_busy(APP_SETTINGS_BUSY_OWNER_WEB_SPECTRUM);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    scan->scan_id = scan_id;
    scan->step_khz = SPECTRUM_STEP_KHZ;
    scan->bandwidth_khz = SPECTRUM_BANDWIDTH_KHZ;
    spectrum_resolve_range(&state, &scan->start_khz, &scan->end_khz);

    if (xTaskCreate(spectrum_scan_task,
                    "web_spectrum",
                    SPECTRUM_SCAN_STACK,
                    scan,
                    SPECTRUM_SCAN_PRIO,
                    &s_spectrum_task) != pdPASS) {
        free(scan);
        spectrum_set_active(false);
        app_settings_end_busy(APP_SETTINGS_BUSY_OWNER_WEB_SPECTRUM);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan task create failed");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "scan_id", scan->scan_id);
    cJSON_AddNumberToObject(root, "start", scan->start_khz);
    cJSON_AddNumberToObject(root, "end", scan->end_khz);
    cJSON_AddNumberToObject(root, "step", scan->step_khz);
    cJSON_AddNumberToObject(root, "bw", scan->bandwidth_khz);
    cJSON_AddNumberToObject(root,
                            "total",
                            (double)spectrum_count_points(scan->start_khz, scan->end_khz, scan->step_khz));
    esp_err_t err = send_json_response(req, root);
    cJSON_Delete(root);
    return err;
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
    { "/api/spectrum/scan", HTTP_POST, h_post_spectrum_scan },
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
    config.max_open_sockets = 7;
    config.max_uri_handlers  = 24;
    config.max_resp_headers  = 8;
    config.lru_purge_enable  = true;
    config.uri_match_fn      = httpd_uri_match_wildcard;
    config.close_fn          = http_session_close_cb;

    ESP_LOGI(TAG,
             "http server config: req_hdr_max=%d, uri_max=%d, handlers=%u, sockets=%u, lru=%d, stack=%u",
             HTTPD_MAX_REQ_HDR_LEN,
             HTTPD_MAX_URI_LEN,
             (unsigned)config.max_uri_handlers,
             (unsigned)config.max_open_sockets,
             config.lru_purge_enable ? 1 : 0,
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

    s_spectrum_mutex = xSemaphoreCreateMutex();
    if (!s_spectrum_mutex) {
        vTaskDelete(s_sse_worker_task);
        s_sse_worker_task = NULL;
        vQueueDelete(s_sse_req_queue);
        s_sse_req_queue = NULL;
        vQueueDelete(s_sse_msg_queue);
        s_sse_msg_queue = NULL;
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGE(TAG, "failed to create spectrum mutex");
        return ESP_ERR_NO_MEM;
    }
    s_spectrum_active = false;
    s_spectrum_task = NULL;
    s_spectrum_cancel_requested = false;

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

    // --- WebSocket audio streaming setup ---
    s_ws_ring_mutex = xSemaphoreCreateMutex();
    s_ws_audio_ring = xRingbufferCreate(WS_AUDIO_RING_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_ws_ring_mutex || !s_ws_audio_ring) {
        ESP_LOGE(TAG, "failed to create ws audio resources");
    } else {
        httpd_uri_t ws_audio_uri = {
            .uri        = "/api/audio",
            .method     = HTTP_GET,
            .handler    = h_ws_audio,
            .user_ctx   = NULL,
            .is_websocket = true,
        };
        err = httpd_register_uri_handler(s_server, &ws_audio_uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register ws /api/audio failed: %s", esp_err_to_name(err));
        }

        s_ws_audio_running = true;
        if (xTaskCreate(ws_audio_sender_fn, "ws_audio", WS_AUDIO_SEND_STACK, NULL,
                        WS_AUDIO_SEND_PRIO, (TaskHandle_t *)&s_ws_audio_task) != pdPASS) {
            s_ws_audio_running = false;
            ESP_LOGE(TAG, "failed to create ws audio sender task");
        } else {
            audio_register_data_cb(ws_audio_data_cb, NULL);
            ESP_LOGI(TAG, "ws audio streaming ready");
        }
    }

    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (!s_server) return;

    tuner_controller_unregister_cb(status_cb);
    audio_unregister_data_cb(ws_audio_data_cb);
    spectrum_set_active(false);
    s_spectrum_cancel_requested = true;
    if (s_spectrum_task) {
        while (s_spectrum_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    // Stop WS audio sender task
    s_ws_audio_running = false;
    if (s_ws_audio_task) {
        while (s_ws_audio_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(WS_AUDIO_POLL_MS));
        }
    }
    for (int i = 0; i < WS_AUDIO_MAX_CLIENTS; i++) {
        s_ws_clients[i].active = false;
        s_ws_clients[i].fd = -1;
    }
    if (s_ws_audio_ring) {
        vRingbufferDelete(s_ws_audio_ring);
        s_ws_audio_ring = NULL;
    }
    if (s_ws_ring_mutex) {
        vSemaphoreDelete(s_ws_ring_mutex);
        s_ws_ring_mutex = NULL;
    }

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
    if (s_spectrum_mutex) {
        vSemaphoreDelete(s_spectrum_mutex);
        s_spectrum_mutex = NULL;
    }
    s_spectrum_task = NULL;

    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "web server stopped");
}
