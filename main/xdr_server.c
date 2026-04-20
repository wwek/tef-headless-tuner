#include "xdr_server.h"
#include "tuner_controller.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/sha1.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>

#define XDR_PORT          7373
#define MAX_XDR_CLIENTS   4
#define RX_BUF_SIZE       256
#define STATUS_INTERVAL_MS 67
#define AUTH_SALT_LEN     16
#define SHA1_HEX_LEN      40
#define XDR_TASK_STACK    6144
#define XDR_TASK_PRIO     4

static const char *TAG = "xdr";

typedef struct {
    int sock;
    bool authenticated;
    bool active;
    uint16_t last_pi;
    char last_ps[9];
    char last_rt[65];
} xdr_client_t;

static xdr_client_t s_clients[MAX_XDR_CLIENTS];
static int s_listen_sock = -1;
static TaskHandle_t s_task;
static volatile bool s_running;

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
    if (c->sock >= 0) {
        close(c->sock);
        c->sock = -1;
    }
    c->active = false;
    c->authenticated = false;
    c->last_pi = 0;
    c->last_ps[0] = '\0';
    c->last_rt[0] = '\0';
}

static int send_all(int sock, const char *buf, size_t len)
{
    while (len > 0) {
        int n = send(sock, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= n;
    }
    return 0;
}

static int send_str(int sock, const char *str)
{
    return send_all(sock, str, strlen(str));
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
        send_str(sock, "a0\n");
        close(sock);
        ESP_LOGW(TAG, "rejected connection: max clients reached");
        return;
    }

    if (set_nonblocking(sock) < 0) {
        close(sock);
        ESP_LOGE(TAG, "failed to set nonblocking on new client");
        return;
    }

    memset(c, 0, sizeof(*c));
    c->sock = sock;
    c->active = true;

    // Authentication handshake
    char salt[AUTH_SALT_LEN + 1];
    generate_salt(salt);

    char *stored_key = read_stored_key();
    if (stored_key[0] == '\0') {
        // No key set: skip auth
        c->authenticated = true;
        send_str(sock, "o1,0\n");
        free(stored_key);
        ESP_LOGI(TAG, "client connected (no auth)");
        return;
    }

    // Send salt, wait for SHA1 response
    char challenge[AUTH_SALT_LEN + 2];
    memcpy(challenge, salt, AUTH_SALT_LEN);
    challenge[AUTH_SALT_LEN] = '\n';
    challenge[AUTH_SALT_LEN + 1] = '\0';
    send_str(sock, challenge);

    // Compute expected response
    char expected[SHA1_HEX_LEN + 1];
    compute_sha1_hex(salt, stored_key, expected);
    free(stored_key);

    // Read client response (blocking with short timeout)
    // The client is still blocking at this point; set a recv timeout
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char resp[SHA1_HEX_LEN + 2] = {0};
    int n = recv(sock, resp, sizeof(resp) - 1, 0);

    // Restore non-blocking
    set_nonblocking(sock);

    if (n <= 0) {
        ESP_LOGW(TAG, "auth: no response from client");
        close_client(c);
        return;
    }

    // Strip trailing newline
    if (n > 0 && resp[n - 1] == '\n') resp[n - 1] = '\0';
    if (n > 1 && resp[n - 2] == '\r') resp[n - 2] = '\0';

    if (strlen(resp) == SHA1_HEX_LEN && memcmp(resp, expected, SHA1_HEX_LEN) == 0) {
        c->authenticated = true;
        send_str(sock, "o1,0\n");
        ESP_LOGI(TAG, "client authenticated");
    } else {
        send_str(sock, "a0\n");
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
    case 'T': {
        // Tune: freq in 10Hz units, convert to kHz
        long freq_10hz = strtol(arg, NULL, 10);
        if (freq_10hz <= 0) {
            break;
        }
        uint32_t freq_khz = (uint32_t)(freq_10hz / 100);
        tuner_state_t state = tuner_controller_get_state();
        if (state.active_band == TEF_BAND_FM) {
            tuner_controller_tune_fm(freq_khz);
        } else {
            tuner_controller_tune_am(freq_khz);
        }
        break;
    }
    case 'M': {
        int mode = atoi(arg);
        if (mode == 0) {
            tuner_controller_switch_band(TEF_BAND_FM, NULL);
        } else if (mode == 1) {
            tuner_controller_switch_band(TEF_BAND_MW, NULL);
        }
        break;
    }
    case 'Y': {
        int vol = atoi(arg);
        uint8_t scaled = (uint8_t)(vol * 30 / 100);
        tuner_controller_set_volume(scaled);
        break;
    }
    case 'D': {
        int val = atoi(arg);
        uint16_t deemp;
        if (val == 0) deemp = 50;
        else if (val == 1) deemp = 75;
        else deemp = 0;
        tuner_controller_set_deemphasis(deemp);
        break;
    }
    case 'A': {
        int val = atoi(arg);
        tuner_controller_set_softmute_fm(val != 0);
        break;
    }
    case 'W': {
        uint16_t bw = (uint16_t)atoi(arg);
        tuner_controller_set_bandwidth_fm(bw);
        break;
    }
    case 'C': {
        int dir = atoi(arg);
        tuner_controller_abort_seek();
        tuner_controller_start_seek(dir == 1, false);
        break;
    }
    case 'x': {
        // Init: send current frequency sync
        tuner_state_t state = tuner_controller_get_state();
        char resp[32];
        // T<freq_10hz>
        uint32_t freq_10hz = (uint32_t)state.status.frequency * 100;
        snprintf(resp, sizeof(resp), "T%lu\n", (unsigned long)freq_10hz);
        send_str(c->sock, resp);
        // M<mode>
        snprintf(resp, sizeof(resp), "M%d\n", state.active_band == TEF_BAND_FM ? 0 : 1);
        send_str(c->sock, resp);
        break;
    }
    default:
        break;
    }
}

static void push_status(xdr_client_t *c)
{
    if (!c->authenticated) return;

    tuner_state_t state = tuner_controller_get_state();

    // Signal status: S<m><level>,<wam>,<usn>,<bw>\n\n
    char stereo_ch = state.status.stereo ? 's' : 'm';
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "S%c%d,%u,%u,%u\n\n",
                       stereo_ch,
                       (int)state.quality.level,
                       (unsigned)state.quality.wam,
                       (unsigned)state.quality.usn,
                       (unsigned)state.quality.bandwidth);
    if (len > 0) {
        send_all(c->sock, buf, (size_t)len);
    }

    // RDS: only push on change
    if (state.rds.has_data) {
        // PI code
        if (state.rds.pi != c->last_pi) {
            c->last_pi = state.rds.pi;
            len = snprintf(buf, sizeof(buf), "P%04X\n", state.rds.pi);
            send_all(c->sock, buf, (size_t)len);
        }
        // PS name
        if (memcmp(state.rds.ps, c->last_ps, 8) != 0) {
            memcpy(c->last_ps, state.rds.ps, 9);
            len = snprintf(buf, sizeof(buf), "%.8s\n", state.rds.ps);
            send_all(c->sock, buf, (size_t)len);
        }
        // RadioText
        if (strcmp(state.rds.rt, c->last_rt) != 0) {
            strncpy(c->last_rt, state.rds.rt, 64);
            c->last_rt[64] = '\0';
            // Simplified RDS format: R<bbbb><cccc><dddd><ee>
            // Push raw RT line
            len = snprintf(buf, sizeof(buf), "R%s\n", state.rds.rt);
            send_all(c->sock, buf, (size_t)len);
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

    char rx_buf[RX_BUF_SIZE];

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

            int r = recv(c->sock, rx_buf, sizeof(rx_buf) - 1, 0);
            if (r <= 0) {
                ESP_LOGI(TAG, "client disconnected (recv=%d)", r);
                close_client(c);
                continue;
            }
            rx_buf[r] = '\0';

            // Process line by line
            char *line = rx_buf;
            char *nl;
            while ((nl = strchr(line, '\n')) != NULL) {
                *nl = '\0';
                // Strip trailing CR
                if (nl > line && *(nl - 1) == '\r') *(nl - 1) = '\0';
                if (*line) {
                    handle_xdr_command(c, line);
                }
                line = nl + 1;
            }
        }

        // Periodic status push on timeout (n == 0 means select timed out)
        if (n == 0) {
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
    vTaskDelete(NULL);
}

esp_err_t xdr_server_start(void)
{
    if (s_running) return ESP_OK;

    memset(s_clients, 0, sizeof(s_clients));
    for (int i = 0; i < MAX_XDR_CLIENTS; i++) {
        s_clients[i].sock = -1;
    }

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
    s_running = false;
    // Task will clean up and delete itself
}
