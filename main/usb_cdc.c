#include "usb_cdc.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdarg.h>

static const char *TAG = "usb_cdc";

static char s_rx_buf[256];
static int s_rx_len;
static bool s_connected;

static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    // Data available signal - actual reading done in read_line
}

static void cdc_line_state_cb(int itf, cdcacm_event_t *event)
{
    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    s_connected = dtr && rts;
    ESP_LOGI(TAG, "Line state: DTR=%d RTS=%d connected=%d", dtr, rts, s_connected);
}

esp_err_t usb_cdc_interface_init(void)
{
    tinyusb_config_cdcacm_t cdc_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 0,
        .callback_rx = cdc_rx_cb,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = cdc_line_state_cb,
        .callback_line_coding_changed = NULL,
    };
    esp_err_t err = tusb_cdc_acm_init(&cdc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CDC ACM init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "USB CDC interface initialized");
    return ESP_OK;
}

esp_err_t usb_cdc_write(const char *data, size_t len)
{
    if (len == 0) return ESP_OK;
    size_t written = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)data, len);
    if (written < len) {
        ESP_LOGW(TAG, "CDC TX overflow: %d/%d written", (int)written, (int)len);
    }
    return tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
}

esp_err_t usb_cdc_write_str(const char *str)
{
    return usb_cdc_write(str, strlen(str));
}

esp_err_t usb_cdc_write_fmt(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) return ESP_FAIL;
    if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;
    return usb_cdc_write(buf, len);
}

int usb_cdc_read_line(char *buf, int buf_size)
{
    if (buf_size <= 0) return 0;

    while (s_rx_len < (int)sizeof(s_rx_buf) - 1) {
        uint8_t byte;
        size_t rx_size = 0;
        esp_err_t err = tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, &byte, 1, &rx_size);
        if (err != ESP_OK || rx_size == 0) break;

        if (byte == '\r' || byte == '\n') {
            if (s_rx_len > 0) {
                break;
            }
            continue;
        }

        s_rx_buf[s_rx_len++] = (char)byte;
    }

    if (s_rx_len == 0) return 0;

    int copy_len = s_rx_len;
    if (copy_len >= buf_size) copy_len = buf_size - 1;
    memcpy(buf, s_rx_buf, copy_len);
    buf[copy_len] = '\0';
    s_rx_len = 0;
    return copy_len;
}

bool usb_cdc_connected(void)
{
    return s_connected;
}
