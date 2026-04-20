#include "xdr_server.h"
#include "tuner_controller.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/sha1.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <fcntl.h>
#include <errno.h>

#define XDR_PORT          7373
#define MAX_XDR_CLIENTS   4
#define RX_BUF_SIZE       256
#define TX_BUF_SIZE       4096
#define STATUS_INTERVAL_MS 67
#define AUTH_SALT_LEN     16
#define SHA1_HEX_LEN      40
#define XDR_TASK_STACK    6144
#define XDR_TASK_PRIO     4
#define XDR_SCAN_SETTLE_MS 20
#define CLIENT_SEND_TIMEOUT_MS 1000

static const char *TAG = "xdr";

typedef struct {
    int sock;
    bool authenticated;
    bool active;
    uint32_t session_id;
    size_t rx_len;
    char rx_buf[RX_BUF_SIZE];
    size_t tx_len;
    char tx_buf[TX_BUF_SIZE];
    uint16_t last_pi;
    uint16_t last_rds_b;
    uint16_t last_rds_c;
    uint16_t last_rds_d;
    uint8_t last_rds_err;
    bool last_seeking;
} xdr_client_t;

static xdr_client_t s_clients[MAX_XDR_CLIENTS];
static int s_listen_sock = -1;
static TaskHandle_t s_task;
static volatile bool s_running;
static tef_band_t s_last_am_band = TEF_BAND_MW;

typedef struct {
    bool autosquelch_enabled;
    uint8_t fm_scan_sensitivity;
    bool dx_scan_enabled;
    uint8_t scan_hold;
    int16_t squelch;
    uint32_t scanner_start_khz;
    uint32_t scanner_end_khz;
    uint32_t scanner_step_khz;
    uint16_t scanner_bandwidth_khz;
    int scanner_filter;
} xdr_runtime_t;

typedef struct {
    TaskHandle_t task;
    bool active;
    xdr_client_t *owner;
    uint32_t owner_session_id;
    uint32_t start_khz;
    uint32_t end_khz;
    uint32_t step_khz;
    uint16_t bandwidth_khz;
    int filter;
} xdr_scan_state_t;

static SemaphoreHandle_t s_state_mutex;
static xdr_runtime_t s_xdr_runtime = {
    .autosquelch_enabled = false,
    .fm_scan_sensitivity = 15,
    .dx_scan_enabled = false,
    .scan_hold = 5,
    .squelch = -1,
    .scanner_start_khz = 87500,
    .scanner_end_khz = 108000,
    .scanner_step_khz = 100,
    .scanner_bandwidth_khz = 0,
    .scanner_filter = 0,
};

static xdr_scan_state_t s_scan_state;
static uint32_t s_next_session_id = 1;

static int set_nonblocking(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

static void set_keepalive(int sock)
{
    int keepalive = 1;
    int idle = 5;
    int interval = 1;
    int count = 3;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
}

static xdr_client_t *find_free_client(void)
{
    for (int i = 0; i < MAX_XDR_CLIENTS; i++) {
        if (!s_clients[i].active) return &s_clients[i];
    }
    return NULL;
}

static void close_client(xdr_client_t *c)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_scan_state.active
        && s_scan_state.owner == c
        && s_scan_state.owner_session_id == c->session_id) {
        s_scan_state.active = false;
        s_scan_state.owner = NULL;
        s_scan_state.owner_session_id = 0;
    }
    xSemaphoreGive(s_state_mutex);

    if (c->sock >= 0) {
        close(c->sock);
        c->sock = -1;
    }
    c->active = false;
    c->authenticated = false;
    c->session_id = 0;
    c->rx_len = 0;
    c->tx_len = 0;
    c->last_pi = 0;
    c->last_rds_b = 0;
    c->last_rds_c = 0;
    c->last_rds_d = 0;
    c->last_rds_err = 0;
    c->last_seeking = false;
}

static int wait_socket_writable(int sock, int timeout_ms)
{
    fd_set write_fds;
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);

    int ret = select(sock + 1, NULL, &write_fds, NULL, &tv);
    if (ret <= 0) {
        return -1;
    }

    return FD_ISSET(sock, &write_fds) ? 0 : -1;
}

static int recv_line_blocking(int sock, char *buf, size_t size, int timeout_ms)
{
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    size_t len = 0;

    if (size < 2) {
        return -1;
    }

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (len + 1 < size) {
        int n = recv(sock, buf + len, size - len - 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }

        len += (size_t)n;
        buf[len] = '\0';

        char *nl = strchr(buf, '\n');
        if (nl != NULL) {
            *nl = '\0';
            if (nl > buf && *(nl - 1) == '\r') {
                *(nl - 1) = '\0';
            }
            return (int)strlen(buf);
        }

        if (len == SHA1_HEX_LEN) {
            return (int)len;
        }
    }

    return -1;
}

static int send_all_retry(int sock, const char *buf, size_t len, int timeout_ms)
{
    while (len > 0) {
        int n = send(sock, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (wait_socket_writable(sock, timeout_ms) == 0) {
                    continue;
                }
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        buf += n;
        len -= n;
    }
    return 0;
}

static int send_str_retry(int sock, const char *str, int timeout_ms)
{
    return send_all_retry(sock, str, strlen(str), timeout_ms);
}

static int flush_client_tx_wait(xdr_client_t *c, int timeout_ms)
{
    int waited_ms = 0;

    while (s_running && waited_ms <= timeout_ms) {
        int sock;
        size_t tx_len;
        ssize_t sent;

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (!c->active || c->sock < 0) {
            xSemaphoreGive(s_state_mutex);
            return -1;
        }
        if (c->tx_len == 0) {
            xSemaphoreGive(s_state_mutex);
            return 0;
        }
        sock = c->sock;
        tx_len = c->tx_len;
        xSemaphoreGive(s_state_mutex);

        sent = send(sock, c->tx_buf, tx_len, 0);
        if (sent > 0) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if ((size_t)sent < c->tx_len) {
                memmove(c->tx_buf, c->tx_buf + sent, c->tx_len - (size_t)sent);
            }
            c->tx_len -= (size_t)sent;
            xSemaphoreGive(s_state_mutex);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int slice_ms = (timeout_ms - waited_ms) > 10 ? 10 : (timeout_ms - waited_ms);
            if (slice_ms <= 0) {
                return 0;
            }
            (void)wait_socket_writable(sock, slice_ms);
            waited_ms += slice_ms;
            continue;
        }

        close_client(c);
        return -1;
    }

    return 0;
}

static int queue_client_data(xdr_client_t *c, uint32_t session_id, const char *buf, size_t len, int timeout_ms)
{
    int waited_ms = 0;

    if (len > TX_BUF_SIZE) {
        return -1;
    }

    while (s_running) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (!c->active || c->sock < 0 || c->session_id != session_id) {
            xSemaphoreGive(s_state_mutex);
            return -1;
        }
        if ((TX_BUF_SIZE - c->tx_len) >= len) {
            memcpy(c->tx_buf + c->tx_len, buf, len);
            c->tx_len += len;
            xSemaphoreGive(s_state_mutex);
            return 0;
        }
        xSemaphoreGive(s_state_mutex);

        if (timeout_ms <= 0 || waited_ms >= timeout_ms) {
            return -1;
        }

        int remaining_ms = timeout_ms - waited_ms;
        int slice_ms = remaining_ms > 50 ? 50 : remaining_ms;
        if (flush_client_tx_wait(c, slice_ms) < 0) {
            return -1;
        }
        waited_ms += slice_ms;
    }

    return -1;
}

static int queue_client_str_drop(xdr_client_t *c, const char *str)
{
    return queue_client_data(c, c->session_id, str, strlen(str), 0);
}

static int queue_client_str(xdr_client_t *c, const char *str)
{
    if (queue_client_data(c, c->session_id, str, strlen(str), CLIENT_SEND_TIMEOUT_MS) != 0) {
        close_client(c);
        return -1;
    }
    return 0;
}

static int queue_client_str_wait(xdr_client_t *c, uint32_t session_id, const char *str, int timeout_ms)
{
    return queue_client_data(c, session_id, str, strlen(str), timeout_ms);
}

static int queue_client_frame_drop(xdr_client_t *c, const char *buf, size_t len)
{
    return queue_client_data(c, c->session_id, buf, len, 0);
}

static int queue_client_frame(xdr_client_t *c, const char *buf, size_t len)
{
    if (queue_client_data(c, c->session_id, buf, len, CLIENT_SEND_TIMEOUT_MS) != 0) {
        close_client(c);
        return -1;
    }
    return 0;
}

static int queue_client_frame_wait(xdr_client_t *c, uint32_t session_id, const char *buf, size_t len, int timeout_ms)
{
    return queue_client_data(c, session_id, buf, len, timeout_ms);
}

static void flush_client_tx(xdr_client_t *c)
{
    while (true) {
        int sock;
        size_t tx_len;
        ssize_t sent;

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (!c->active || c->sock < 0 || c->tx_len == 0) {
            xSemaphoreGive(s_state_mutex);
            return;
        }
        sock = c->sock;
        tx_len = c->tx_len;
        sent = send(sock, c->tx_buf, tx_len, 0);
        if (sent > 0) {
            if ((size_t)sent < c->tx_len) {
                memmove(c->tx_buf, c->tx_buf + sent, c->tx_len - (size_t)sent);
            }
            c->tx_len -= (size_t)sent;
            xSemaphoreGive(s_state_mutex);
            continue;
        }

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            xSemaphoreGive(s_state_mutex);
            return;
        }

        xSemaphoreGive(s_state_mutex);
        close_client(c);
        return;
    }
}

static int scan_owner_sock(void)
{
    int sock = -1;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_scan_state.active
        && s_scan_state.owner != NULL
        && s_scan_state.owner->active
        && s_scan_state.owner->sock >= 0
        && s_scan_state.owner->session_id == s_scan_state.owner_session_id) {
        sock = s_scan_state.owner->sock;
    }
    xSemaphoreGive(s_state_mutex);

    return sock;
}

static bool scan_is_active_for_client(const xdr_client_t *c)
{
    bool active;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    active = s_scan_state.active
        && s_scan_state.owner == c
        && s_scan_state.owner_session_id == c->session_id;
    xSemaphoreGive(s_state_mutex);

    return active;
}

static void scan_finish(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_scan_state.active = false;
    s_scan_state.owner = NULL;
    s_scan_state.owner_session_id = 0;
    s_scan_state.task = NULL;
    xSemaphoreGive(s_state_mutex);
}

static bool band_mode_is_am(tef_band_t band)
{
    return band == TEF_BAND_LW || band == TEF_BAND_MW || band == TEF_BAND_SW;
}

static uint8_t pack_rds_error_bits(uint16_t dec_error)
{
    uint8_t packed = 0;

    packed |= (uint8_t)(((dec_error >> 8) & 0x30U) >> 4);
    packed |= (uint8_t)((dec_error >> 8) & 0x0CU);
    packed |= (uint8_t)(((dec_error >> 8) & 0x03U) << 4);

    return packed;
}

static void format_signal_level(char *buf, size_t size, int16_t level_tenth_db)
{
    int32_t abs_level = level_tenth_db;

    if (abs_level < 0) {
        abs_level = -abs_level;
    }

    snprintf(buf, size, "%s%d.%d",
             level_tenth_db < 0 ? "-" : "",
             (int)(abs_level / 10),
             (int)(abs_level % 10));
}

static uint16_t bandwidth_from_index(int index)
{
    static const uint16_t s_bandwidths_khz[] = {
        56, 64, 72, 84, 97, 114, 133, 151,
        168, 184, 200, 217, 236, 254, 287, 311,
    };

    if (index < 0) {
        return 0;
    }
    if (index >= (int)(sizeof(s_bandwidths_khz) / sizeof(s_bandwidths_khz[0]))) {
        return UINT16_MAX;
    }

    return s_bandwidths_khz[index];
}

static uint16_t bandwidth_from_filter_code(int filter_code)
{
    switch (filter_code) {
    case 0: return 56;
    case 26: return 64;
    case 1: return 72;
    case 2: return 77;
    case 28: return 84;
    case 29: return 97;
    case 3: return 114;
    case 4: return 133;
    case 6: return 142;
    case 5: return 151;
    case 7: return 168;
    case 8: return 184;
    case 9: return 200;
    case 10: return 217;
    case 11: return 236;
    case 12: return 254;
    case 13: return 287;
    case 14: return 269;
    case 15: return 311;
    default: return UINT16_MAX;
    }
}

static int filter_code_from_bandwidth(uint16_t bandwidth_khz)
{
    switch (bandwidth_khz) {
    case 0: return -1;
    case 56: return 0;
    case 64: return 26;
    case 72: return 1;
    case 77: return 2;
    case 84: return 28;
    case 97: return 29;
    case 114: return 3;
    case 133: return 4;
    case 142: return 6;
    case 151: return 5;
    case 168: return 7;
    case 184: return 8;
    case 200: return 9;
    case 217: return 10;
    case 236: return 11;
    case 254: return 12;
    case 269: return 14;
    case 287: return 13;
    case 311: return 15;
    default: return -2;
    }
}

static bool scan_uses_fm(uint32_t start_khz, uint32_t end_khz, tef_band_t current_band)
{
    if (start_khz >= 64000 || end_khz >= 64000) {
        return true;
    }

    return !band_mode_is_am(current_band);
}

static int xdr_scan_level_from_state(const tuner_state_t *state)
{
    return (int)((state->quality.level + 5) / 10);
}

static void xdr_scan_task(void *arg)
{
    (void)arg;

    tuner_state_t saved_state = tuner_controller_get_state();
    tuner_xdr_settings_t saved_settings = {0};
    xdr_client_t *owner = NULL;
    uint32_t owner_session_id = 0;
    const bool scan_is_fm = scan_uses_fm(s_scan_state.start_khz, s_scan_state.end_khz, saved_state.active_band);
    const uint32_t step_khz = s_scan_state.step_khz == 0 ? (scan_is_fm ? 100U : 9U) : s_scan_state.step_khz;
    esp_err_t err = tuner_controller_set_scan_mute(true);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    owner = s_scan_state.owner;
    owner_session_id = s_scan_state.owner_session_id;
    xSemaphoreGive(s_state_mutex);

    tuner_controller_get_xdr_settings(&saved_settings);

    if (scan_is_fm && err == ESP_OK) {
        (void)tuner_controller_set_bandwidth_fm(s_scan_state.bandwidth_khz);
    }

    if (err != ESP_OK || owner == NULL || queue_client_str_wait(owner, owner_session_id, "U", 1000) != 0) {
        err = err == ESP_OK ? ESP_FAIL : err;
    }

    if (err == ESP_OK) {
        for (uint32_t freq_khz = s_scan_state.start_khz;
             freq_khz <= s_scan_state.end_khz && s_running;
             freq_khz += step_khz) {
            if (scan_owner_sock() < 0) {
                break;
            }

            err = scan_is_fm
                ? tuner_controller_tune_fm(freq_khz)
                : tuner_controller_tune_am(freq_khz);
            if (err != ESP_OK) {
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(XDR_SCAN_SETTLE_MS));

            tuner_state_t state = tuner_controller_get_state();
            char chunk[48];
            int len = snprintf(chunk, sizeof(chunk), "%" PRIu32 " = %d, ",
                               freq_khz,
                               xdr_scan_level_from_state(&state));
            if (queue_client_frame_wait(owner, owner_session_id, chunk, (size_t)len, 1000) != 0) {
                break;
            }

            if (freq_khz > UINT32_MAX - step_khz) {
                break;
            }
        }

        (void)queue_client_str_wait(owner, owner_session_id, "\n", 1000);
    } else {
        if (owner != NULL) {
            (void)queue_client_str_wait(owner, owner_session_id, "E_SCAN\n", 1000);
        }
    }

    if (scan_is_fm) {
        (void)tuner_controller_set_bandwidth_fm(saved_settings.fm_bandwidth_khz);
    }

    if (band_mode_is_am(saved_state.active_band)) {
        (void)tuner_controller_tune_am(saved_state.status.frequency);
    } else {
        (void)tuner_controller_tune_fm(saved_state.status.frequency);
    }
    (void)tuner_controller_set_scan_mute(false);

    scan_finish();
    vTaskDelete(NULL);
}

static int send_xdr_g_state_client(xdr_client_t *c)
{
    tuner_xdr_settings_t settings = {0};
    char buf[8];
    int len;

    tuner_controller_get_xdr_settings(&settings);
    len = snprintf(buf, sizeof(buf), "G%d%d\n",
                   settings.eq_enabled ? 0 : 1,
                   settings.ims_enabled ? 0 : 1);
    return queue_client_frame(c, buf, (size_t)len);
}

static int send_xdr_tune_state_client(xdr_client_t *c)
{
    tuner_state_t state = tuner_controller_get_state();
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "T%" PRIu32 "\n", state.status.frequency);
    return queue_client_frame(c, buf, (size_t)len);
}

static int send_xdr_b_state_client(xdr_client_t *c)
{
    tuner_xdr_settings_t settings = {0};
    char buf[8];
    int value;
    int len;

    tuner_controller_get_xdr_settings(&settings);
    value = settings.audio_mode == TUNER_XDR_AUDIO_MODE_STEREO_AUTO ? 0 : 1;
    len = snprintf(buf, sizeof(buf), "B%d\n", value);
    return queue_client_frame(c, buf, (size_t)len);
}

static int send_xdr_a_state_client(xdr_client_t *c)
{
    tuner_xdr_settings_t settings = {0};
    char buf[8];
    int len;

    tuner_controller_get_xdr_settings(&settings);
    len = snprintf(buf, sizeof(buf), "A%u\n", (unsigned)settings.agc_index);
    return queue_client_frame(c, buf, (size_t)len);
}

static int send_xdr_d_state_client(xdr_client_t *c)
{
    tuner_xdr_settings_t settings = {0};
    char buf[8];
    int value = 2;
    int len;

    tuner_controller_get_xdr_settings(&settings);
    if (settings.deemphasis_us == 50) {
        value = 0;
    } else if (settings.deemphasis_us == 75) {
        value = 1;
    }

    len = snprintf(buf, sizeof(buf), "D%d\n", value);
    return queue_client_frame(c, buf, (size_t)len);
}

static int send_xdr_w_state_client(xdr_client_t *c)
{
    tuner_xdr_settings_t settings = {0};
    char buf[16];
    unsigned bandwidth_hz = 0;
    int len;

    tuner_controller_get_xdr_settings(&settings);
    if (settings.fm_bandwidth_khz > 0) {
        bandwidth_hz = (unsigned)settings.fm_bandwidth_khz * 1000U;
    }

    len = snprintf(buf, sizeof(buf), "W%u\n", bandwidth_hz);
    return queue_client_frame(c, buf, (size_t)len);
}

static int send_xdr_f_state_client(xdr_client_t *c)
{
    tuner_xdr_settings_t settings = {0};
    char buf[16];
    int filter_code;
    int len;

    tuner_controller_get_xdr_settings(&settings);
    filter_code = filter_code_from_bandwidth(settings.fm_bandwidth_khz);
    if (filter_code < -1) {
        return 0;
    }

    len = snprintf(buf, sizeof(buf), "F%d\n", filter_code);
    return queue_client_frame(c, buf, (size_t)len);
}

static int send_xdr_z_state_client(xdr_client_t *c)
{
    tuner_xdr_settings_t settings = {0};
    char buf[8];
    int len;

    tuner_controller_get_xdr_settings(&settings);
    len = snprintf(buf, sizeof(buf), "Z%u\n", (unsigned)settings.antenna_index);
    return queue_client_frame(c, buf, (size_t)len);
}

// Generate random hex salt of AUTH_SALT_LEN chars
static void generate_salt(char *out)
{
    uint8_t bytes[AUTH_SALT_LEN / 2];
    for (int i = 0; i < AUTH_SALT_LEN / 2; i++) {
        bytes[i] = (uint8_t)(esp_random() & 0xFF);
    }
    for (int i = 0; i < AUTH_SALT_LEN / 2; i++) {
        sprintf(out + i * 2, "%02x", bytes[i]);
    }
    out[AUTH_SALT_LEN] = '\0';
}

// Read stored XDR key from NVS. Returns heap-allocated string (empty if not set).
static char *read_stored_key(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi_mgr", NVS_READONLY, &h);
    if (err != ESP_OK) {
        return strdup("");
    }
    size_t len = 0;
    err = nvs_get_str(h, "xdr_key", NULL, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(h);
        return strdup("");
    }
    char *key = malloc(len);
    if (key) {
        nvs_get_str(h, "xdr_key", key, &len);
    }
    nvs_close(h);
    return key ? key : strdup("");
}

// Compute SHA1 hex of salt+key into out (must be >= 41 bytes)
static void compute_sha1_hex(const char *salt, const char *key, char *out)
{
    size_t salt_len = strlen(salt);
    size_t key_len = strlen(key);
    size_t total = salt_len + key_len;
    uint8_t *buf = malloc(total);
    if (!buf) {
        out[0] = '\0';
        return;
    }
    memcpy(buf, salt, salt_len);
    memcpy(buf + salt_len, key, key_len);
    uint8_t digest[20];
    mbedtls_sha1(buf, total, digest);
    free(buf);
    for (int i = 0; i < 20; i++) {
        sprintf(out + i * 2, "%02x", digest[i]);
    }
    out[40] = '\0';
}

static void handle_new_connection(void)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int sock = accept(s_listen_sock, (struct sockaddr *)&addr, &len);
    if (sock < 0) return;

    set_keepalive(sock);

    xdr_client_t *c = find_free_client();
    if (!c) {
        send_str_retry(sock, "a0\n", 1000);
        close(sock);
        ESP_LOGW(TAG, "rejected connection: max clients reached");
        return;
    }

    memset(c, 0, sizeof(*c));
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_next_session_id == 0) {
        s_next_session_id = 1;
    }
    c->session_id = s_next_session_id++;
    if (s_next_session_id == 0) {
        s_next_session_id = 1;
    }
    xSemaphoreGive(s_state_mutex);
    c->sock = sock;
    c->active = true;

    // Authentication handshake
    char salt[AUTH_SALT_LEN + 1];
    generate_salt(salt);

    char *stored_key = read_stored_key();
    if (stored_key[0] == '\0') {
        // No key set: skip auth
        c->authenticated = true;
        if (set_nonblocking(sock) < 0) {
            close_client(c);
            free(stored_key);
            ESP_LOGE(TAG, "failed to set nonblocking on new client");
            return;
        }
        (void)queue_client_str(c, "o1,0\n");
        (void)send_xdr_g_state_client(c);
        free(stored_key);
        ESP_LOGI(TAG, "client connected (no auth)");
        return;
    }

    // Send salt, wait for SHA1 response
    char challenge[AUTH_SALT_LEN + 2];
    memcpy(challenge, salt, AUTH_SALT_LEN);
    challenge[AUTH_SALT_LEN] = '\n';
    challenge[AUTH_SALT_LEN + 1] = '\0';
    send_str_retry(sock, challenge, 1000);

    // Compute expected response
    char expected[SHA1_HEX_LEN + 1];
    compute_sha1_hex(salt, stored_key, expected);
    free(stored_key);

    // Read client response (blocking with short timeout)
    // The client is still blocking at this point; set a recv timeout
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char resp[SHA1_HEX_LEN + 2] = {0};
    int n = recv_line_blocking(sock, resp, sizeof(resp), 5000);

    if (n <= 0) {
        ESP_LOGW(TAG, "auth: no response from client");
        close_client(c);
        return;
    }

    if (strlen(resp) == SHA1_HEX_LEN && memcmp(resp, expected, SHA1_HEX_LEN) == 0) {
        c->authenticated = true;
        if (set_nonblocking(sock) < 0) {
            close_client(c);
            ESP_LOGE(TAG, "failed to set nonblocking on authenticated client");
            return;
        }
        (void)queue_client_str(c, "o1,0\n");
        (void)send_xdr_g_state_client(c);
        ESP_LOGI(TAG, "client authenticated");
    } else {
        send_str_retry(sock, "a0\n", 1000);
        ESP_LOGW(TAG, "client auth failed");
        close_client(c);
    }
}

static void handle_xdr_command(xdr_client_t *c, const char *line)
{
    if (!c->authenticated) return;

    char cmd = line[0];
    const char *arg = line + 1;

    switch (cmd) {
    case 'A': {
        int agc = atoi(arg);
        if (agc < 0 || agc > 3) {
            break;
        }
        if (tuner_controller_set_agc_index((uint8_t)agc) == ESP_OK) {
            char resp[8];
            int len = snprintf(resp, sizeof(resp), "A%d\n", agc);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'B': {
        int mode = atoi(arg);
        tuner_xdr_audio_mode_t audio_mode;

        if (mode == 0) {
            audio_mode = TUNER_XDR_AUDIO_MODE_FORCED_MONO;
        } else if (mode == 1) {
            audio_mode = TUNER_XDR_AUDIO_MODE_STEREO_AUTO;
        } else {
            audio_mode = TUNER_XDR_AUDIO_MODE_MPX;
        }

        if (tuner_controller_set_xdr_audio_mode(audio_mode) == ESP_OK) {
            char resp[8];
            int value = mode > 1 ? 2 : mode;
            int len = snprintf(resp, sizeof(resp), "B%d\n", value);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'T': {
        long freq = strtol(arg, NULL, 10);
        tuner_state_t before = tuner_controller_get_state();
        tuner_state_t after;
        esp_err_t err;
        char resp[32];
        int len;

        if (freq <= 0) {
            break;
        }

        if (freq >= 64000 && freq <= 108000) {
            err = tuner_controller_tune_fm((uint32_t)freq);
        } else if (freq >= 144 && freq <= 27000) {
            err = tuner_controller_tune_am((uint32_t)freq);
        } else {
            break;
        }

        if (err != ESP_OK) {
            break;
        }

        after = tuner_controller_get_state();
        if (band_mode_is_am(after.active_band)) {
            s_last_am_band = after.active_band;
        }

        if (band_mode_is_am(before.active_band) != band_mode_is_am(after.active_band)) {
            len = snprintf(resp, sizeof(resp), "M%d\n", band_mode_is_am(after.active_band) ? 1 : 0);
            (void)queue_client_frame(c, resp, (size_t)len);
        }

        len = snprintf(resp, sizeof(resp), "T%" PRIu32 "\n", after.status.frequency);
        (void)queue_client_frame(c, resp, (size_t)len);
        break;
    }
    case 'M': {
        int mode = atoi(arg);
        uint32_t applied_freq = 0;
        esp_err_t err;

        if (mode == 0) {
            err = tuner_controller_switch_band(TEF_BAND_FM, &applied_freq);
        } else if (mode == 1) {
            err = tuner_controller_switch_band(s_last_am_band, &applied_freq);
        } else {
            break;
        }

        if (err == ESP_OK) {
            char resp[32];
            int len = snprintf(resp, sizeof(resp), "M%d\nT%" PRIu32 "\n", mode, applied_freq);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'Y': {
        int volume = atoi(arg);
        uint8_t scaled;

        if (volume == 0) {
            if (tuner_controller_set_mute(true) == ESP_OK) {
                (void)queue_client_str(c, "Y0\n");
            }
            break;
        }
        if (volume < 0) {
            break;
        }

        if (volume > 255) {
            volume = 255;
        }
        scaled = (uint8_t)(((unsigned)volume * 30U + 127U) / 255U);
        if (scaled == 0) {
            scaled = 1;
        }
        if (scaled > 30) {
            scaled = 30;
        }

        if (tuner_controller_set_mute(false) == ESP_OK
            && tuner_controller_set_volume(scaled) == ESP_OK) {
            char resp[16];
            int len = snprintf(resp, sizeof(resp), "Y%d\n", volume);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'D': {
        int val = atoi(arg);
        uint16_t deemp;
        if (val == 0) deemp = 50;
        else if (val == 1) deemp = 75;
        else deemp = 0;
        if (tuner_controller_set_deemphasis(deemp) == ESP_OK) {
            char resp[8];
            int len = snprintf(resp, sizeof(resp), "D%d\n", val);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'F': {
        int index = atoi(arg);
        uint16_t bandwidth_khz = bandwidth_from_index(index);

        if (bandwidth_khz == UINT16_MAX) {
            break;
        }

        if (tuner_controller_set_bandwidth_fm(bandwidth_khz) == ESP_OK) {
            char resp[16];
            int len = snprintf(resp, sizeof(resp), "F%d\n", index);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'G': {
        int value = atoi(arg);
        bool ims_enabled;
        bool eq_enabled;

        if (value == 0) {
            ims_enabled = true;
            eq_enabled = true;
        } else if (value == 10) {
            ims_enabled = true;
            eq_enabled = false;
        } else if (value == 1) {
            ims_enabled = false;
            eq_enabled = true;
        } else if (value == 11) {
            ims_enabled = false;
            eq_enabled = false;
        } else {
            break;
        }

        if (tuner_controller_set_xdr_eq(ims_enabled, eq_enabled) == ESP_OK) {
            char resp[8];
            int len = snprintf(resp, sizeof(resp), "G%02d\n", value);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'H': {
        int value = atoi(arg);
        bool enabled;
        esp_err_t err;

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (value == 0) {
            enabled = false;
        } else if (value == 1) {
            enabled = true;
        } else {
            enabled = !s_xdr_runtime.autosquelch_enabled;
        }
        xSemaphoreGive(s_state_mutex);

        err = tuner_controller_set_auto_squelch(enabled);
        if (err == ESP_OK) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_xdr_runtime.autosquelch_enabled = enabled;
            xSemaphoreGive(s_state_mutex);
            char resp[8];
            int len = snprintf(resp, sizeof(resp), "H%d\n", enabled ? 1 : 0);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'I': {
        int value = atoi(arg);

        if (value > 0 && value < 31) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_xdr_runtime.fm_scan_sensitivity = (uint8_t)value;
            xSemaphoreGive(s_state_mutex);
        }

        {
            char resp[8];
            unsigned current;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            current = (unsigned)s_xdr_runtime.fm_scan_sensitivity;
            xSemaphoreGive(s_state_mutex);
            int len = snprintf(resp, sizeof(resp), "I%u\n", current);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'J': {
        int value = atoi(arg);

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (value == 0) {
            s_xdr_runtime.dx_scan_enabled = false;
        } else if (value == 1) {
            s_xdr_runtime.dx_scan_enabled = true;
        }
        bool enabled = s_xdr_runtime.dx_scan_enabled;
        xSemaphoreGive(s_state_mutex);

        {
            char resp[8];
            int len = snprintf(resp, sizeof(resp), "J%d\n", enabled ? 1 : 0);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'K': {
        int value = atoi(arg);

        if (value >= 0 && value < 31) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_xdr_runtime.scan_hold = (uint8_t)value;
            xSemaphoreGive(s_state_mutex);
        }

        {
            char resp[8];
            unsigned current;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            current = (unsigned)s_xdr_runtime.scan_hold;
            xSemaphoreGive(s_state_mutex);
            int len = snprintf(resp, sizeof(resp), "K%u\n", current);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'W': {
        int bandwidth = atoi(arg);
        uint16_t bandwidth_khz;

        if (bandwidth < 0) {
            break;
        }

        if (bandwidth == 0) {
            bandwidth_khz = 0;
        } else if (bandwidth >= 1000) {
            bandwidth_khz = (uint16_t)(bandwidth / 1000);
        } else {
            bandwidth_khz = (uint16_t)bandwidth;
        }

        if (tuner_controller_set_bandwidth_fm(bandwidth_khz) == ESP_OK) {
            char resp[16];
            int len = snprintf(resp, sizeof(resp), "W%d\n", bandwidth);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'Q': {
        int value = atoi(arg);
        esp_err_t err;
        int16_t squelch = (int16_t)value;

        err = tuner_controller_set_squelch(squelch);
        if (err == ESP_OK) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_xdr_runtime.squelch = squelch;
            xSemaphoreGive(s_state_mutex);
            if (squelch == -1) {
                (void)queue_client_str(c, "Q-1\n");
            } else {
                char resp[16];
                int len = snprintf(resp, sizeof(resp), "Q%d\n", value);
                (void)queue_client_frame(c, resp, (size_t)len);
            }
        }
        break;
    }
    case 'S': {
        switch (arg[0]) {
        case 'a':
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_xdr_runtime.scanner_start_khz = (uint32_t)strtoul(arg + 1, NULL, 10);
            xSemaphoreGive(s_state_mutex);
            break;
        case 'b':
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_xdr_runtime.scanner_end_khz = (uint32_t)strtoul(arg + 1, NULL, 10);
            xSemaphoreGive(s_state_mutex);
            break;
        case 'c':
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_xdr_runtime.scanner_step_khz = (uint32_t)strtoul(arg + 1, NULL, 10);
            xSemaphoreGive(s_state_mutex);
            break;
        case 'f': {
            uint16_t bandwidth_khz = bandwidth_from_filter_code(atoi(arg + 1));
            if (bandwidth_khz != UINT16_MAX) {
                if (tuner_controller_set_bandwidth_fm(bandwidth_khz) == ESP_OK) {
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_xdr_runtime.scanner_filter = atoi(arg + 1);
                    s_xdr_runtime.scanner_bandwidth_khz = bandwidth_khz;
                    xSemaphoreGive(s_state_mutex);
                }
            }
            break;
        }
        case 'w': {
            uint32_t bandwidth_hz = (uint32_t)strtoul(arg + 1, NULL, 10);
            uint16_t bandwidth_khz = (uint16_t)(bandwidth_hz / 1000U);
            if (tuner_controller_set_bandwidth_fm(bandwidth_khz) == ESP_OK) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_xdr_runtime.scanner_bandwidth_khz = bandwidth_khz;
                xSemaphoreGive(s_state_mutex);
            }
            break;
        }
        case '\0':
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (!s_scan_state.active) {
                s_scan_state.active = true;
                s_scan_state.owner = c;
                s_scan_state.owner_session_id = c->session_id;
                s_scan_state.start_khz = s_xdr_runtime.scanner_start_khz;
                s_scan_state.end_khz = s_xdr_runtime.scanner_end_khz;
                s_scan_state.step_khz = s_xdr_runtime.scanner_step_khz;
                s_scan_state.bandwidth_khz = s_xdr_runtime.scanner_bandwidth_khz;
                s_scan_state.filter = s_xdr_runtime.scanner_filter;
                xSemaphoreGive(s_state_mutex);
                if (xTaskCreate(xdr_scan_task, "xdr_scan", 4096, NULL, XDR_TASK_PRIO, &s_scan_state.task) != pdPASS) {
                    scan_finish();
                    (void)queue_client_str(c, "E_SCAN\n");
                }
            } else {
                xSemaphoreGive(s_state_mutex);
                (void)queue_client_str(c, "E_BUSY\n");
            }
            break;
        default:
            break;
        }
        break;
    }
    case 'Z': {
        int antenna = atoi(arg);

        if (antenna < 0 || antenna > 3) {
            break;
        }

        if (tuner_controller_set_antenna((uint8_t)antenna) == ESP_OK) {
            char resp[8];
            int len = snprintf(resp, sizeof(resp), "Z%d\n", antenna);
            (void)queue_client_frame(c, resp, (size_t)len);
        }
        break;
    }
    case 'C': {
        int dir = atoi(arg);
        bool am_mode = band_mode_is_am(tuner_controller_get_state().active_band);
        tuner_controller_abort_seek();
        if (dir == 1) {
            if (tuner_controller_start_seek(false, am_mode) == ESP_OK) {
                c->last_seeking = true;
                (void)queue_client_str(c, "C1\n");
            }
        } else if (dir == 2) {
            if (tuner_controller_start_seek(true, am_mode) == ESP_OK) {
                c->last_seeking = true;
                (void)queue_client_str(c, "C2\n");
            }
        }
        break;
    }
    case 'x': {
        (void)queue_client_str(c, "OK\n");
        (void)send_xdr_tune_state_client(c);
        (void)send_xdr_b_state_client(c);
        (void)send_xdr_g_state_client(c);
        (void)send_xdr_a_state_client(c);
        (void)send_xdr_f_state_client(c);
        (void)send_xdr_w_state_client(c);
        (void)send_xdr_d_state_client(c);
        (void)send_xdr_z_state_client(c);
        break;
    }
    case 'X': {
        close_client(c);
        break;
    }
    default:
        break;
    }
}

static void push_status(xdr_client_t *c)
{
    if (!c->authenticated) return;
    if (scan_is_active_for_client(c)) return;

    tuner_state_t state = tuner_controller_get_state();
    tuner_xdr_settings_t settings = {0};
    tef_rds_raw_t raw = {0};
    char level_buf[16];

    // Signal status: S<m><level>,<wam>,<usn>,<bw>\n\n
    char stereo_ch = 'm';
    char buf[128];
    int len;

    tuner_controller_get_xdr_settings(&settings);
    if (band_mode_is_am(state.active_band)) {
        s_last_am_band = state.active_band;
    } else if (settings.audio_mode != TUNER_XDR_AUDIO_MODE_STEREO_AUTO) {
        stereo_ch = 'M';
    } else {
        stereo_ch = state.status.stereo ? 's' : 'm';
    }

    format_signal_level(level_buf, sizeof(level_buf), state.quality.level);
    len = snprintf(buf, sizeof(buf), "S%c%s,%u,%u,%u\n\n",
                       stereo_ch,
                       level_buf,
                       (unsigned)(state.quality.wam / 10U),
                       (unsigned)(state.quality.usn / 10U),
                       (unsigned)state.quality.bandwidth);
    if (len > 0) {
        (void)queue_client_frame_drop(c, buf, (size_t)len);
    }

    if (c->last_seeking && !state.seeking) {
        (void)queue_client_str_drop(c, "C0\n");
        c->last_seeking = false;
    }

    if (state.active_band == TEF_BAND_FM
        && tuner_controller_get_rds_data(&raw) == ESP_OK
        && (raw.status & (1U << 9)) != 0) {
        uint8_t packed_error = pack_rds_error_bits(raw.dec_error);

        if (((raw.dec_error >> 14) & 0x03U) < 3U && raw.block_a != c->last_pi) {
            c->last_pi = raw.block_a;
            len = snprintf(buf, sizeof(buf), "P%04X\n", raw.block_a);
            (void)queue_client_frame_drop(c, buf, (size_t)len);
        }

        if (raw.block_b != c->last_rds_b
            || raw.block_c != c->last_rds_c
            || raw.block_d != c->last_rds_d
            || packed_error != c->last_rds_err) {
            c->last_rds_b = raw.block_b;
            c->last_rds_c = raw.block_c;
            c->last_rds_d = raw.block_d;
            c->last_rds_err = packed_error;
            len = snprintf(buf, sizeof(buf), "R%04X%04X%04X%02X\n",
                           raw.block_b,
                           raw.block_c,
                           raw.block_d,
                           packed_error);
            (void)queue_client_frame_drop(c, buf, (size_t)len);
        }
    }
}

static void xdr_tcp_task(void *arg)
{
    (void)arg;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(XDR_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_listen_sock < 0) {
        ESP_LOGE(TAG, "failed to create socket: %d", errno);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(s_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed: %d", errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (listen(s_listen_sock, 4) < 0) {
        ESP_LOGE(TAG, "listen failed: %d", errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    set_nonblocking(s_listen_sock);
    ESP_LOGI(TAG, "listening on port %d", XDR_PORT);

    char rx_chunk[RX_BUF_SIZE];
    TickType_t last_status_push_tick = xTaskGetTickCount();

    while (s_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);

        int max_fd = s_listen_sock;
        FD_SET(s_listen_sock, &read_fds);

        for (int i = 0; i < MAX_XDR_CLIENTS; i++) {
            if (s_clients[i].active && s_clients[i].sock >= 0) {
                FD_SET(s_clients[i].sock, &read_fds);
                if (s_clients[i].sock > max_fd) max_fd = s_clients[i].sock;
            }
        }

        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = STATUS_INTERVAL_MS * 1000,
        };

        int n = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "select error: %d", errno);
            break;
        }

        // New connection
        if (n > 0 && FD_ISSET(s_listen_sock, &read_fds)) {
            handle_new_connection();
            n--;
        }

        // Client data
        for (int i = 0; i < MAX_XDR_CLIENTS && n > 0; i++) {
            xdr_client_t *c = &s_clients[i];
            if (!c->active || c->sock < 0) continue;
            if (!FD_ISSET(c->sock, &read_fds)) continue;
            n--;

            int r = recv(c->sock, rx_chunk, sizeof(rx_chunk), 0);
            if (r <= 0) {
                ESP_LOGI(TAG, "client disconnected (recv=%d)", r);
                close_client(c);
                continue;
            }

            if (c->rx_len + (size_t)r >= sizeof(c->rx_buf)) {
                ESP_LOGW(TAG, "client rx overflow");
                close_client(c);
                continue;
            }

            memcpy(c->rx_buf + c->rx_len, rx_chunk, (size_t)r);
            c->rx_len += (size_t)r;
            c->rx_buf[c->rx_len] = '\0';

            char *line = c->rx_buf;
            char *nl;
            while ((nl = memchr(line, '\n', c->rx_buf + c->rx_len - line)) != NULL) {
                *nl = '\0';
                if (nl > line && *(nl - 1) == '\r') {
                    *(nl - 1) = '\0';
                }
                if (*line) {
                    handle_xdr_command(c, line);
                }
                line = nl + 1;
            }

            size_t remaining = (size_t)(c->rx_buf + c->rx_len - line);
            if (remaining > 0 && line != c->rx_buf) {
                memmove(c->rx_buf, line, remaining);
            }
            c->rx_len = remaining;
            c->rx_buf[c->rx_len] = '\0';
        }

        for (int i = 0; i < MAX_XDR_CLIENTS; i++) {
            if (s_clients[i].active) {
                flush_client_tx(&s_clients[i]);
            }
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_status_push_tick) >= pdMS_TO_TICKS(STATUS_INTERVAL_MS)) {
            last_status_push_tick = now;
            for (int i = 0; i < MAX_XDR_CLIENTS; i++) {
                if (s_clients[i].active && s_clients[i].authenticated) {
                    push_status(&s_clients[i]);
                }
            }
        }
    }

    // Cleanup
    for (int i = 0; i < MAX_XDR_CLIENTS; i++) {
        if (s_clients[i].active) close_client(&s_clients[i]);
    }
    if (s_listen_sock >= 0) {
        close(s_listen_sock);
        s_listen_sock = -1;
    }
    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t xdr_server_start(void)
{
    if (s_running) return ESP_OK;

    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(s_clients, 0, sizeof(s_clients));
    for (int i = 0; i < MAX_XDR_CLIENTS; i++) {
        s_clients[i].sock = -1;
    }
    s_last_am_band = TEF_BAND_MW;

    s_running = true;
    BaseType_t ret = xTaskCreate(xdr_tcp_task, "xdr_tcp", XDR_TASK_STACK,
                                  NULL, XDR_TASK_PRIO, &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create task");
        s_running = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

void xdr_server_stop(void)
{
    TaskHandle_t task = s_task;
    if (task == NULL) {
        s_running = false;
        return;
    }

    s_running = false;
    if (s_listen_sock >= 0) {
        shutdown(s_listen_sock, SHUT_RDWR);
    }
    while (s_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
