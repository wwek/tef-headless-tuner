#include "audio.h"
#include "usb_audio_desc.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "tusb.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "audio";

#ifndef CONFIG_AUDIO_TASK_CORE
#define CONFIG_AUDIO_TASK_CORE 1
#endif

#define AUDIO_CHANNEL_COUNT 2
#define AUDIO_BYTES_PER_SAMPLE (CONFIG_AUDIO_BITS_PER_SAMPLE / 8)
#define AUDIO_SAMPLE_FRAME_BYTES (AUDIO_CHANNEL_COUNT * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_FRAME_SAMPLES (((CONFIG_AUDIO_SAMPLE_RATE) + 999) / 1000)
#define AUDIO_FRAME_BYTES (AUDIO_FRAME_SAMPLES * AUDIO_SAMPLE_FRAME_BYTES)
#define AUDIO_RING_BUF_SIZE (32 * 1024)
#define AUDIO_USB_PREROLL_BYTES (AUDIO_FRAME_BYTES * 8)
#define AUDIO_VOLUME_RANGE_MIN (-90 * 256)
#define AUDIO_VOLUME_RANGE_MAX 0
#define AUDIO_VOLUME_RANGE_RES 256
#define AUDIO_TASK_STACK_BYTES 4096
#define AUDIO_TASK_PRIO 6

static i2s_chan_handle_t s_rx_chan;
static TaskHandle_t s_audio_task;
static volatile bool s_running;
static RingbufHandle_t s_ring_buf;
static uint32_t s_sample_rate_hz;
static uint8_t s_clock_valid = 1;
static bool s_mute[AUDIO_CHANNEL_COUNT + 1];
static int16_t s_volume[AUDIO_CHANNEL_COUNT + 1];
static size_t s_ring_queued_bytes;
static uint8_t s_usb_stage_buf[AUDIO_FRAME_BYTES];
static size_t s_usb_stage_len;
static size_t s_usb_stage_offset;
static bool s_usb_preroll_done;
static uint32_t s_ring_drop_count;

#define MAX_AUDIO_CONSUMERS 4

typedef struct {
    audio_data_cb_t cb;
    void *ctx;
} audio_consumer_t;

static audio_consumer_t s_consumers[MAX_AUDIO_CONSUMERS];

void audio_register_data_cb(audio_data_cb_t cb, void *ctx)
{
    if (!cb) return;
    for (int i = 0; i < MAX_AUDIO_CONSUMERS; i++) {
        if (!s_consumers[i].cb) {
            s_consumers[i].cb = cb;
            s_consumers[i].ctx = ctx;
            return;
        }
    }
    ESP_LOGW(TAG, "no free audio consumer slot");
}

void audio_unregister_data_cb(audio_data_cb_t cb)
{
    if (!cb) return;
    for (int i = 0; i < MAX_AUDIO_CONSUMERS; i++) {
        if (s_consumers[i].cb == cb) {
            s_consumers[i].cb = NULL;
            s_consumers[i].ctx = NULL;
        }
    }
}

static struct TU_ATTR_PACKED {
    uint16_t wNumSubRanges;
    struct TU_ATTR_PACKED {
        int32_t bMin;
        int32_t bMax;
        uint32_t bRes;
    } subrange[1];
} s_sample_rate_range;

static i2s_data_bit_width_t audio_data_bit_width_from_config(int bits_per_sample)
{
    switch (bits_per_sample) {
        case 16:
            return I2S_DATA_BIT_WIDTH_16BIT;
        case 24:
            return I2S_DATA_BIT_WIDTH_24BIT;
        case 32:
            return I2S_DATA_BIT_WIDTH_32BIT;
        default:
            return I2S_DATA_BIT_WIDTH_16BIT;
    }
}

static int16_t audio_effective_volume_q8(int channel)
{
    int16_t volume = s_volume[0];

    if (channel > 0 && channel <= AUDIO_CHANNEL_COUNT) {
        volume = s_volume[channel];
    }

    if (volume < AUDIO_VOLUME_RANGE_MIN) {
        volume = AUDIO_VOLUME_RANGE_MIN;
    }
    if (volume > AUDIO_VOLUME_RANGE_MAX) {
        volume = AUDIO_VOLUME_RANGE_MAX;
    }

    return volume;
}

static bool audio_is_muted(int channel)
{
    return s_mute[0] || (channel > 0 && channel <= AUDIO_CHANNEL_COUNT && s_mute[channel]);
}

static int32_t audio_apply_gain_q8(int32_t sample, int16_t volume_q8)
{
    int32_t gain_q8 = 256 + volume_q8;
    if (gain_q8 < 0) {
        gain_q8 = 0;
    }
    return (sample * gain_q8) / 256;
}

static void audio_process_pcm_16(uint8_t *data, size_t len)
{
    int16_t *samples = (int16_t *)data;
    size_t sample_count = len / sizeof(int16_t);

    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        int32_t left = samples[i];
        int32_t right = samples[i + 1];

        if (audio_is_muted(1)) {
            left = 0;
        } else {
            left = audio_apply_gain_q8(left, audio_effective_volume_q8(1));
        }

        if (audio_is_muted(2)) {
            right = 0;
        } else {
            right = audio_apply_gain_q8(right, audio_effective_volume_q8(2));
        }

        if (left > INT16_MAX) left = INT16_MAX;
        if (left < INT16_MIN) left = INT16_MIN;
        if (right > INT16_MAX) right = INT16_MAX;
        if (right < INT16_MIN) right = INT16_MIN;

        samples[i] = (int16_t)left;
        samples[i + 1] = (int16_t)right;
    }
}

#if CONFIG_AUDIO_BITS_PER_SAMPLE == 24
static void audio_process_pcm_24(uint8_t *data, size_t len)
{
    size_t frame_stride = 6;

    for (size_t i = 0; i + frame_stride <= len; i += frame_stride) {
        int32_t left = (int32_t)((data[i + 2] << 24) | (data[i + 1] << 16) | (data[i] << 8)) >> 8;
        int32_t right = (int32_t)((data[i + 5] << 24) | (data[i + 4] << 16) | (data[i + 3] << 8)) >> 8;

        if (audio_is_muted(1)) {
            left = 0;
        } else {
            left = audio_apply_gain_q8(left, audio_effective_volume_q8(1));
        }

        if (audio_is_muted(2)) {
            right = 0;
        } else {
            right = audio_apply_gain_q8(right, audio_effective_volume_q8(2));
        }

        if (left > 0x7FFFFF) left = 0x7FFFFF;
        if (left < -0x800000) left = -0x800000;
        if (right > 0x7FFFFF) right = 0x7FFFFF;
        if (right < -0x800000) right = -0x800000;

        data[i] = (uint8_t)(left & 0xFF);
        data[i + 1] = (uint8_t)((left >> 8) & 0xFF);
        data[i + 2] = (uint8_t)((left >> 16) & 0xFF);
        data[i + 3] = (uint8_t)(right & 0xFF);
        data[i + 4] = (uint8_t)((right >> 8) & 0xFF);
        data[i + 5] = (uint8_t)((right >> 16) & 0xFF);
    }
}
#endif

static void audio_process_buffer(uint8_t *data, size_t len)
{
#if CONFIG_AUDIO_BITS_PER_SAMPLE == 24
    audio_process_pcm_24(data, len);
#else
    audio_process_pcm_16(data, len);
#endif
}

static void audio_ring_reset(void)
{
    if (!s_ring_buf) {
        s_ring_queued_bytes = 0;
        s_ring_drop_count = 0;
        return;
    }

    while (true) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(s_ring_buf, &item_size, 0);
        if (!item) {
            break;
        }
        vRingbufferReturnItem(s_ring_buf, item);
    }

    s_ring_queued_bytes = 0;
    s_ring_drop_count = 0;
}

static void audio_usb_state_reset(void)
{
    s_usb_stage_len = 0;
    s_usb_stage_offset = 0;
    s_usb_preroll_done = false;
}

static size_t audio_ring_fill_bytes(void)
{
    return s_ring_queued_bytes;
}

static bool audio_load_usb_stage_buffer(void)
{
    size_t item_size = 0;
    uint8_t *data = (uint8_t *)xRingbufferReceive(s_ring_buf, &item_size, 0);
    if (!data || item_size == 0) {
        return false;
    }

    if (item_size > sizeof(s_usb_stage_buf)) {
        item_size = sizeof(s_usb_stage_buf);
    }

    memcpy(s_usb_stage_buf, data, item_size);
    vRingbufferReturnItem(s_ring_buf, data);
    if (s_ring_queued_bytes >= item_size) {
        s_ring_queued_bytes -= item_size;
    } else {
        s_ring_queued_bytes = 0;
    }

    audio_process_buffer(s_usb_stage_buf, item_size);
    s_usb_stage_len = item_size;
    s_usb_stage_offset = 0;
    return true;
}

static void audio_ring_push(const uint8_t *data, size_t len)
{
    if (!s_ring_buf || !data || len == 0) {
        return;
    }

    BaseType_t wr = xRingbufferSend(s_ring_buf, data, len, 0);
    if (wr == pdTRUE) {
        s_ring_queued_bytes += len;
        return;
    }

    size_t dropped_size = 0;
    void *dropped = xRingbufferReceive(s_ring_buf, &dropped_size, 0);
    if (dropped) {
        vRingbufferReturnItem(s_ring_buf, dropped);
        if (s_ring_queued_bytes >= dropped_size) {
            s_ring_queued_bytes -= dropped_size;
        } else {
            s_ring_queued_bytes = 0;
        }
        s_ring_drop_count++;
        if (s_ring_drop_count == 1 || (s_ring_drop_count % 128) == 0) {
            ESP_LOGW(TAG,
                     "USB audio ring overflow: dropped=%" PRIu32 " last_drop=%u queued=%u capacity=%u",
                     s_ring_drop_count,
                     (unsigned)dropped_size,
                     (unsigned)s_ring_queued_bytes,
                     (unsigned)AUDIO_RING_BUF_SIZE);
        }
    }

    if (xRingbufferSend(s_ring_buf, data, len, 0) == pdTRUE) {
        s_ring_queued_bytes += len;
    } else {
        ESP_LOGW(TAG, "USB audio ring write failed after drop: len=%u queued=%u",
                 (unsigned)len, (unsigned)s_ring_queued_bytes);
    }
}

esp_err_t audio_init(const audio_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    i2s_data_bit_width_t width = audio_data_bit_width_from_config(config->bits_per_sample);

#if CFG_TUD_AUDIO
    s_ring_buf = xRingbufferCreateWithCaps(
        AUDIO_RING_BUF_SIZE,
        RINGBUF_TYPE_NOSPLIT,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );
    if (!s_ring_buf) {
        ESP_LOGE(TAG, "Failed to create internal audio ring buffer");
        return ESP_ERR_NO_MEM;
    }
#else
    s_ring_buf = NULL;
    ESP_LOGW(TAG, "TinyUSB audio class is disabled; running I2S capture without USB audio streaming");
#endif

    // Create I2S RX channel (slave mode - TEF6686 is I2S master)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(config->i2s_port, I2S_ROLE_SLAVE);
    chan_cfg.auto_clear = false;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
#if CFG_TUD_AUDIO
        if (s_ring_buf) {
            vRingbufferDeleteWithCaps(s_ring_buf);
            s_ring_buf = NULL;
        }
#endif
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->bclk_pin,
            .ws = config->ws_pin,
            .dout = I2S_GPIO_UNUSED,
            .din = config->data_pin,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    if (width == I2S_DATA_BIT_WIDTH_24BIT) {
        std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    }

    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        (void)i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
#if CFG_TUD_AUDIO
        if (s_ring_buf) {
            vRingbufferDeleteWithCaps(s_ring_buf);
            s_ring_buf = NULL;
        }
#endif
        ESP_LOGE(TAG, "Failed to init I2S std mode: %s", esp_err_to_name(err));
        return err;
    }

    s_sample_rate_hz = config->sample_rate;
    s_clock_valid = 1;
    memset(s_mute, 0, sizeof(s_mute));
    memset(s_volume, 0, sizeof(s_volume));
    audio_ring_reset();
    audio_usb_state_reset();
    s_sample_rate_range.wNumSubRanges = tu_htole16(1);
    s_sample_rate_range.subrange[0].bMin = (int32_t)config->sample_rate;
    s_sample_rate_range.subrange[0].bMax = (int32_t)config->sample_rate;
    s_sample_rate_range.subrange[0].bRes = 0;

    ESP_LOGI(TAG, "Audio initialized: %" PRIu32 "Hz %dbit stereo, frame=%u bytes, ring=%u bytes, BCLK=%d WS=%d DIN=%d",
             config->sample_rate, config->bits_per_sample,
             (unsigned)AUDIO_FRAME_BYTES,
             (unsigned)AUDIO_RING_BUF_SIZE,
             config->bclk_pin, config->ws_pin, config->data_pin);
    return ESP_OK;
}

static void feed_usb_audio(void)
{
#if CFG_TUD_AUDIO
    if (!s_ring_buf) {
        return;
    }
    if (!tud_audio_mounted()) {
        audio_usb_state_reset();
        return;
    }

    if (!s_usb_preroll_done) {
        if (audio_ring_fill_bytes() < AUDIO_USB_PREROLL_BYTES) {
            return;
        }
        s_usb_preroll_done = true;
    }

    while (true) {
        if (s_usb_stage_offset >= s_usb_stage_len) {
            if (!audio_load_usb_stage_buffer()) {
                break;
            }
        }

        size_t remaining = s_usb_stage_len - s_usb_stage_offset;
        uint16_t written = tud_audio_write(s_usb_stage_buf + s_usb_stage_offset, (uint16_t)remaining);
        if (written == 0) {
            break;
        }

        s_usb_stage_offset += written;
        if (s_usb_stage_offset < s_usb_stage_len) {
            break;
        }

        s_usb_stage_len = 0;
        s_usb_stage_offset = 0;
    }
#endif
}

static void audio_task_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Audio task started");
    uint8_t buf[AUDIO_FRAME_BYTES];

    audio_ring_reset();
    audio_usb_state_reset();

    esp_err_t err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(err));
        s_running = false;
        s_audio_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_running) {
        size_t bytes_read = 0;
        err = i2s_channel_read(s_rx_chan, buf, sizeof(buf), &bytes_read, pdMS_TO_TICKS(100));
        if (err == ESP_OK && bytes_read > 0) {
            for (int i = 0; i < MAX_AUDIO_CONSUMERS; i++) {
                if (s_consumers[i].cb) {
                    s_consumers[i].cb(buf, bytes_read, s_consumers[i].ctx);
                }
            }

            audio_ring_push(buf, bytes_read);
        }

        // Try to feed USB if TinyUSB audio is ready
        feed_usb_audio();
    }

    (void)i2s_channel_disable(s_rx_chan);
    s_running = false;
    s_audio_task = NULL;
    ESP_LOGI(TAG, "Audio task stopped");
    vTaskDelete(NULL);
}

esp_err_t audio_start(void)
{
    if (s_running || s_audio_task != NULL) return ESP_OK;
    if (!s_rx_chan) return ESP_ERR_INVALID_STATE;

    s_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        audio_task_fn,
        "audio",
        AUDIO_TASK_STACK_BYTES,
        NULL,
        AUDIO_TASK_PRIO,
        &s_audio_task,
        CONFIG_AUDIO_TASK_CORE < 0 ? tskNO_AFFINITY : CONFIG_AUDIO_TASK_CORE
    );
    if (ret != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "Failed to create audio task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_stop(void)
{
    if (!s_running && s_audio_task == NULL) return ESP_OK;
    s_running = false;
    while (s_audio_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    audio_ring_reset();
    audio_usb_state_reset();
#if CFG_TUD_AUDIO
    (void)tud_audio_clear_ep_in_ff();
#endif
    return ESP_OK;
}

bool audio_is_running(void)
{
    return s_running;
}

#if CFG_TUD_AUDIO
static bool audio_get_input_terminal_request(uint8_t rhport, tusb_control_request_t const *request)
{
    uint8_t control_selector = TU_U16_HIGH(request->wValue);

    if (control_selector != AUDIO_TE_CTRL_CONNECTOR) {
        return false;
    }

    audio_desc_channel_cluster_t cluster = {
        .bNrChannels = AUDIO_CHANNEL_COUNT,
        .bmChannelConfig = AUDIO_STEREO_CHANNEL_CFG,
        .iChannelNames = 0,
    };

    return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &cluster, sizeof(cluster));
}

static bool audio_get_feature_unit_request(uint8_t rhport, audio_control_request_t const *request)
{
    uint8_t channel = request->bChannelNumber;

    if (channel > AUDIO_CHANNEL_COUNT) {
        return false;
    }

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t cur_mute = {
            .bCur = s_mute[channel] ? 1 : 0,
        };
        return tud_audio_buffer_and_schedule_control_xfer(
            rhport,
            (tusb_control_request_t const *)request,
            &cur_mute,
            sizeof(cur_mute)
        );
    }

    if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t cur_volume = {
                .bCur = tu_htole16((uint16_t)s_volume[channel]),
            };
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport,
                (tusb_control_request_t const *)request,
                &cur_volume,
                sizeof(cur_volume)
            );
        }

        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_2_n_t(1) volume_range = {
                .wNumSubRanges = tu_htole16(1),
                .subrange[0] = {
                    .bMin = tu_htole16((uint16_t)AUDIO_VOLUME_RANGE_MIN),
                    .bMax = tu_htole16((uint16_t)AUDIO_VOLUME_RANGE_MAX),
                    .bRes = tu_htole16((uint16_t)AUDIO_VOLUME_RANGE_RES),
                },
            };
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport,
                (tusb_control_request_t const *)request,
                &volume_range,
                sizeof(volume_range)
            );
        }
    }

    return false;
}

static bool audio_set_feature_unit_request(audio_control_request_t const *request, uint8_t *buf)
{
    uint8_t channel = request->bChannelNumber;

    if (channel > AUDIO_CHANNEL_COUNT || request->bRequest != AUDIO_CS_REQ_CUR) {
        return false;
    }

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE) {
        if (request->wLength != sizeof(audio_control_cur_1_t)) {
            return false;
        }
        s_mute[channel] = ((audio_control_cur_1_t const *)buf)->bCur != 0;
        return true;
    }

    if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (request->wLength != sizeof(audio_control_cur_2_t)) {
            return false;
        }
        s_volume[channel] = ((audio_control_cur_2_t const *)buf)->bCur;
        return true;
    }

    return false;
}

static bool audio_get_clock_request(uint8_t rhport, audio_control_request_t const *request)
{
    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_4_t cur_freq = {
                .bCur = (int32_t)s_sample_rate_hz,
            };
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport,
                (tusb_control_request_t const *)request,
                &cur_freq,
                sizeof(cur_freq)
            );
        }

        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport,
                (tusb_control_request_t const *)request,
                &s_sample_rate_range,
                sizeof(s_sample_rate_range)
            );
        }
    }

    if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t cur_valid = {
            .bCur = s_clock_valid,
        };
        return tud_audio_buffer_and_schedule_control_xfer(
            rhport,
            (tusb_control_request_t const *)request,
            &cur_valid,
            sizeof(cur_valid)
        );
    }

    return false;
}

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buf)
{
    (void)rhport;
    (void)request;
    (void)buf;
    return false;
}

bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buf)
{
    (void)rhport;
    (void)request;
    (void)buf;
    return false;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;

    uint8_t alt_setting = TU_U16_LOW(tu_le16toh(request->wValue));
    if (alt_setting != 0) {
        audio_ring_reset();
        audio_usb_state_reset();
        (void)tud_audio_clear_ep_in_ff();
    }

    return true;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buf)
{
    (void)rhport;

    audio_control_request_t const *audio_request = (audio_control_request_t const *)request;

    if (audio_request->bEntityID == UAC2_ENTITY_FEATURE_UNIT) {
        return audio_set_feature_unit_request(audio_request, buf);
    }

    return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    (void)request;
    return false;
}

bool tud_audio_get_req_itf_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    (void)request;
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    audio_control_request_t const *audio_request = (audio_control_request_t const *)request;

    if (audio_request->bEntityID == UAC2_ENTITY_INPUT_TERMINAL) {
        return audio_get_input_terminal_request(rhport, request);
    }

    if (audio_request->bEntityID == UAC2_ENTITY_FEATURE_UNIT) {
        return audio_get_feature_unit_request(rhport, audio_request);
    }

    if (audio_request->bEntityID == UAC2_ENTITY_CLOCK) {
        return audio_get_clock_request(rhport, audio_request);
    }

    return false;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    (void)request;

    audio_ring_reset();
    audio_usb_state_reset();
    (void)tud_audio_clear_ep_in_ff();
    return true;
}
#endif
