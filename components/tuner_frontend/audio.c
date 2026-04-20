#include "audio.h"
#include "usb_audio_desc.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "tusb.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "audio";

#define AUDIO_RING_BUF_SIZE (32 * 1024)
#define AUDIO_FRAME_BYTES (((CONFIG_AUDIO_SAMPLE_RATE * 2 * (CONFIG_AUDIO_BITS_PER_SAMPLE / 8)) + 999) / 1000)
#define AUDIO_CHANNEL_COUNT 2
#define AUDIO_VOLUME_RANGE_MIN (-90 * 256)
#define AUDIO_VOLUME_RANGE_MAX 0
#define AUDIO_VOLUME_RANGE_RES 256

static i2s_chan_handle_t s_rx_chan;
static TaskHandle_t s_audio_task;
static volatile bool s_running;
static RingbufHandle_t s_ring_buf;
static uint32_t s_sample_rate_hz;
static uint8_t s_clock_valid = 1;
static bool s_mute[AUDIO_CHANNEL_COUNT + 1];
static int16_t s_volume[AUDIO_CHANNEL_COUNT + 1];
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
        return;
    }

    while (true) {
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_ring_buf, &item_size, 0, AUDIO_RING_BUF_SIZE);
        if (!item) {
            break;
        }
        vRingbufferReturnItem(s_ring_buf, item);
    }
}

esp_err_t audio_init(const audio_config_t *config)
{
#if !CFG_TUD_AUDIO
    (void)config;
    ESP_LOGW(TAG, "TinyUSB audio class is disabled; USB audio path is unavailable");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    i2s_data_bit_width_t width = audio_data_bit_width_from_config(config->bits_per_sample);

    // Create ring buffer for I2S -> USB audio pipeline
    s_ring_buf = xRingbufferCreate(AUDIO_RING_BUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_ring_buf) {
        ESP_LOGE(TAG, "Failed to create audio ring buffer");
        return ESP_ERR_NO_MEM;
    }

    // Create I2S RX channel (slave mode - TEF6686 is I2S master)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(config->i2s_port, I2S_ROLE_SLAVE);
    chan_cfg.auto_clear = false;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
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
        ESP_LOGE(TAG, "Failed to init I2S std mode: %s", esp_err_to_name(err));
        return err;
    }

    s_sample_rate_hz = config->sample_rate;
    s_clock_valid = 1;
    memset(s_mute, 0, sizeof(s_mute));
    memset(s_volume, 0, sizeof(s_volume));
    s_sample_rate_range.wNumSubRanges = tu_htole16(1);
    s_sample_rate_range.subrange[0].bMin = (int32_t)config->sample_rate;
    s_sample_rate_range.subrange[0].bMax = (int32_t)config->sample_rate;
    s_sample_rate_range.subrange[0].bRes = 0;

    ESP_LOGI(TAG, "Audio initialized: %" PRIu32 "Hz %dbit stereo, BCLK=%d WS=%d DIN=%d",
             config->sample_rate, config->bits_per_sample,
             config->bclk_pin, config->ws_pin, config->data_pin);
    return ESP_OK;
#endif
}

static void feed_usb_audio(void)
{
#if CFG_TUD_AUDIO
    if (!s_ring_buf) {
        return;
    }
    if (!tud_audio_mounted()) {
        return;
    }

    size_t item_size = 0;
    uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(s_ring_buf, &item_size, 0, AUDIO_FRAME_BYTES);
    if (!data || item_size == 0) {
        return;
    }

    audio_process_buffer(data, item_size);
    (void)tud_audio_write(data, (uint16_t)item_size);

    vRingbufferReturnItem(s_ring_buf, data);
#endif
}

static void audio_task_fn(void *arg)
{
    ESP_LOGI(TAG, "Audio task started");
    uint8_t buf[AUDIO_FRAME_BYTES];

    i2s_channel_enable(s_rx_chan);
    s_running = true;

    while (s_running) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, buf, sizeof(buf), &bytes_read, pdMS_TO_TICKS(100));
        if (err == ESP_OK && bytes_read > 0) {
            BaseType_t wr = xRingbufferSend(s_ring_buf, buf, bytes_read, pdMS_TO_TICKS(10));
            if (wr != pdTRUE) {
                // Buffer overflow - drop oldest data
                size_t dummy_size;
                void *dummy = xRingbufferReceiveUpTo(s_ring_buf, &dummy_size, 0, bytes_read);
                if (dummy) vRingbufferReturnItem(s_ring_buf, dummy);
                xRingbufferSend(s_ring_buf, buf, bytes_read, 0);
            }
        }

        // Try to feed USB if TinyUSB audio is ready
        feed_usb_audio();
    }

    i2s_channel_disable(s_rx_chan);
    ESP_LOGI(TAG, "Audio task stopped");
    vTaskDelete(NULL);
}

esp_err_t audio_start(void)
{
#if !CFG_TUD_AUDIO
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_running) return ESP_OK;
    if (!s_rx_chan || !s_ring_buf) return ESP_ERR_INVALID_STATE;

    BaseType_t ret = xTaskCreate(audio_task_fn, "audio", 4096, NULL, 6, &s_audio_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}

esp_err_t audio_stop(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));
    audio_ring_reset();
#if CFG_TUD_AUDIO
    (void)tud_audio_clear_ep_in_ff();
#endif
    s_audio_task = NULL;
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
    (void)tud_audio_clear_ep_in_ff();
    return true;
}
#endif
