#include "tef6686.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

// Include firmware patches
#include "tef6686_patch_v102.h"
#include "tef6686_patch_v205.h"

static const char *TAG = "tef6686";

// TEF6686 module IDs (from NXP TEF6686 API)
#define MOD_FM    0x20
#define MOD_AM    0x21
#define MOD_AUDIO 0x30
#define MOD_APPL  0x40

// APPL commands
#define APPL_SET_OP_MODE      1
#define APPL_SET_GPIO         3
#define APPL_GET_OP_STATUS    128
#define APPL_GET_IDENT        130

// FM/AM radio commands
#define RADIO_TUNE_TO         1
#define RADIO_SET_BANDWIDTH   10
#define RADIO_SET_RFAGC       11
#define RADIO_SET_ANTENNA     12
#define RADIO_SET_COCHAN      14
#define RADIO_SET_MPH_SUP     20
#define RADIO_SET_CHAN_EQ     22
#define RADIO_SET_NB          23
#define RADIO_SET_NB_AUDIO    24
#define RADIO_SET_DEEMPH      31
#define RADIO_SET_SI          32
#define RADIO_SET_LEVEL_OFF   39
#define RADIO_SET_SOFTMUTE    45
#define RADIO_SET_HC_LEVEL    52
#define RADIO_SET_HC_NOISE    53
#define RADIO_SET_HC_MPH      54
#define RADIO_SET_HC_MAX      55
#define RADIO_SET_ST_LEVEL    62
#define RADIO_SET_ST_NOISE    63
#define RADIO_SET_ST_MPH      64
#define RADIO_SET_ST_MIN      66
#define RADIO_SET_STHB_LEVEL  72
#define RADIO_SET_STHB_NOISE  73
#define RADIO_SET_STHB_MPH    74
#define RADIO_SET_STHB_MAX    75
#define RADIO_SET_RDS         81
#define RADIO_SET_SPECIALS    85
#define RADIO_SET_BW_OPTIONS  86
#define RADIO_SET_STB_TIME    90
#define RADIO_SET_STB_GAIN    91
#define RADIO_SET_STB_BIAS    92
#define RADIO_GET_QUAL_STATUS 128
#define RADIO_GET_QUAL_DATA   129
#define RADIO_GET_RDS_STATUS  130
#define RADIO_GET_RDS_DATA    131
#define RADIO_GET_SIG_STATUS  133
#define RADIO_GET_PROC_STATUS 134

// Audio commands
#define AUDIO_SET_VOLUME      10
#define AUDIO_SET_MUTE        11
#define AUDIO_SET_INPUT       12
#define AUDIO_SET_DIG_IO      22
#define AUDIO_SET_WAVEGEN     24

// Init table (from PE5PVB reference, configures FM parameters)
static const uint8_t s_init_tab[][18] = {
    {17, 0x20, 0x26, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfc, 0xff, 0xf8, 0x00, 0x00},
    { 9, 0x20, 0x36, 0x01, 0x00, 0x00, 0x01, 0x68, 0x01, 0x2C},
    { 7, 0x20, 0x37, 0x01, 0x00, 0x00, 0x0F, 0xA0},
    { 7, 0x20, 0x39, 0x01, 0x00, 0x01, 0x00, 0x01},
    { 7, 0x20, 0x3A, 0x01, 0x00, 0x01, 0x00, 0x01},
    {11, 0x20, 0x3C, 0x01, 0x00, 0x3C, 0x00, 0x78, 0x00, 0x64, 0x00, 0xC8},
    {11, 0x20, 0x46, 0x01, 0x01, 0xF4, 0x07, 0xD0, 0x00, 0xC8, 0x00, 0xC8},
    { 9, 0x20, 0x48, 0x01, 0x00, 0x00, 0x02, 0x58, 0x00, 0xF0},
    { 9, 0x20, 0x49, 0x01, 0x00, 0x00, 0x00, 0xA0, 0x00, 0x8C},
    { 9, 0x20, 0x4A, 0x01, 0x00, 0x00, 0x00, 0xA0, 0x00, 0x8C},
    { 7, 0x20, 0x4B, 0x01, 0x00, 0x00, 0x0F, 0xA0},
    { 7, 0x30, 0x15, 0x01, 0x00, 0x80, 0x00, 0x01},
    { 7, 0x30, 0x0d, 0x01, 0x00, 0x80, 0x00, 0xe0},
    {0}, // end marker
};

// Clock configuration data
static const uint8_t s_clock_4000[]  = {0x00, 0x3D, 0x09, 0x00, 0x00, 0x00};
static const uint8_t s_clock_9216[]  = {0x00, 0x8C, 0xA0, 0x00, 0x00, 0x00};
static const uint8_t s_clock_12000[] = {0x00, 0xB7, 0x1B, 0x00, 0x00, 0x00};
static const uint8_t s_clock_55000[] = {0x03, 0x4E, 0x5A, 0xAE, 0x00, 0x01};

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static tef_band_t s_current_band = TEF_BAND_FM;
static uint32_t s_current_freq = 87500;
static bool s_mpx_mode;

static const char *band_names[] = {"FM", "LW", "MW", "SW"};

const char *tef_band_name(tef_band_t band)
{
    if (band >= 0 && band <= TEF_BAND_SW) {
        return band_names[band];
    }
    return "UNKNOWN";
}

// --- Low-level I2C communication ---

static esp_err_t tef_write_raw(const uint8_t *buf, uint16_t len)
{
    return i2c_master_transmit(s_dev, buf, len, pdMS_TO_TICKS(100));
}

static tef_band_t tef_infer_am_band(uint32_t freq_khz)
{
    if (freq_khz < 520) {
        return TEF_BAND_LW;
    }

    if (freq_khz <= 1710) {
        return TEF_BAND_MW;
    }

    return TEF_BAND_SW;
}

// Send command with data: [module, cmd, 0x01, data_words...]
static esp_err_t tef_send_cmd(uint8_t module, uint8_t cmd, int num_words, ...)
{
    uint8_t buf[20];
    int len = 3 + num_words * 2;
    if (len > (int)sizeof(buf)) return ESP_ERR_INVALID_ARG;

    buf[0] = module;
    buf[1] = cmd;
    buf[2] = 0x01;

    va_list args;
    va_start(args, num_words);
    for (int i = 0; i < num_words; i++) {
        uint16_t word = (uint16_t)va_arg(args, int);
        buf[3 + i * 2] = (uint8_t)(word >> 8);
        buf[4 + i * 2] = (uint8_t)(word & 0xFF);
    }
    va_end(args);

    return tef_write_raw(buf, len);
}

// Send command header with repeated-start, then read response words
static esp_err_t tef_read_cmd(uint8_t module, uint8_t cmd, uint16_t *data, int num_words)
{
    uint8_t header[3] = {module, cmd, 0x01};
    int byte_count = num_words * 2;
    uint8_t buf[16];
    if (byte_count > (int)sizeof(buf)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = i2c_master_transmit_receive(s_dev, header, sizeof(header),
                                                 buf, byte_count, pdMS_TO_TICKS(100));
    if (err != ESP_OK) return err;

    if (data) {
        for (int i = 0; i < num_words; i++) {
            data[i] = (uint16_t)((buf[i * 2] << 8) | buf[i * 2 + 1]);
        }
    }
    return ESP_OK;
}

// --- Initialization helpers ---

static esp_err_t tef_reset(void)
{
    uint8_t buf[5] = {0x1e, 0x5a, 0x01, 0x5a, 0x5a};
    return tef_write_raw(buf, sizeof(buf));
}

static esp_err_t tef_patch_load(const uint8_t *patch, uint16_t size)
{
    uint8_t buf[25];
    buf[0] = 0x1b;

    while (size > 0) {
        uint16_t chunk = (size > 24) ? 24 : size;
        size -= chunk;
        memcpy(&buf[1], patch, chunk);
        esp_err_t err = tef_write_raw(buf, chunk + 1);
        if (err != ESP_OK) {
            return err;
        }
        patch += chunk;
    }

    return ESP_OK;
}

static esp_err_t tef_load_patch(tef_version_t version)
{
    const uint8_t *patch_data = NULL;
    size_t patch_size = 0;
    const uint8_t *lut_data = NULL;
    size_t lut_size = 0;
    esp_err_t err = tef_reset();
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    // Enter patch mode
    uint8_t buf[3] = {0x1c, 0x00, 0x00};
    err = tef_write_raw(buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    buf[2] = 0x74;
    err = tef_write_raw(buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    if (version == TEF_VERSION_V102) {
        patch_data = pPatchBytes102;
        patch_size = PatchSize102;
        lut_data = pLutBytes102;
        lut_size = LutSize102;
    } else if (version == TEF_VERSION_V205) {
        patch_data = pPatchBytes205;
        patch_size = PatchSize205;
        lut_data = pLutBytes205;
        lut_size = LutSize205;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    err = tef_patch_load(patch_data, patch_size);
    if (err != ESP_OK) {
        return err;
    }

    buf[2] = 0x00;
    err = tef_write_raw(buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    buf[2] = 0x75;
    err = tef_write_raw(buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    err = tef_patch_load(lut_data, lut_size);
    if (err != ESP_OK) {
        return err;
    }

    buf[2] = 0x00;
    return tef_write_raw(buf, sizeof(buf));
}

static esp_err_t tef_init_clock(uint8_t xtal_type)
{
    esp_err_t err;

    // Activate
    uint8_t activate[3] = {0x14, 0x00, 0x01};
    err = tef_write_raw(activate, sizeof(activate));
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    // Set clock
    const uint8_t *clock_data;
    switch (xtal_type) {
        case 1: clock_data = s_clock_9216; break;
        case 2: clock_data = s_clock_12000; break;
        case 3: clock_data = s_clock_55000; break;
        default: clock_data = s_clock_4000; break;
    }

    uint8_t clock_cmd[9] = {0x40, 0x04, 0x01};
    memcpy(&clock_cmd[3], clock_data, 6);
    err = tef_write_raw(clock_cmd, sizeof(clock_cmd));
    if (err != ESP_OK) {
        return err;
    }

    // Enable
    uint8_t enable[5] = {0x40, 0x05, 0x01, 0x00, 0x01};
    err = tef_write_raw(enable, sizeof(enable));
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}

static esp_err_t tef_load_init_tab(void)
{
    for (int i = 0; s_init_tab[i][0] != 0; i++) {
        esp_err_t err = tef_write_raw(&s_init_tab[i][1], s_init_tab[i][0]);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

// --- Public API: Initialization and power ---

esp_err_t tef6686_init(const tef_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = config->i2c_port,
        .sda_io_num = config->sda_pin,
        .scl_io_num = config->scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus create failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TEF6686_I2C_ADDR,
        .scl_speed_hz = config->i2c_freq_hz,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    err = tef_load_patch(config->chip_version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Patch load failed: %s", esp_err_to_name(err));
        return err;
    }

    err = tef_init_clock(config->xtal_type);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Clock init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = tef6686_set_power(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Power on failed: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    err = tef_load_init_tab();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Init table load failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t device, hw_ver, sw_ver;
    if (tef6686_get_identification(&device, &hw_ver, &sw_ver) == ESP_OK) {
        ESP_LOGI(TAG, "TEF device=%04X hw=%04X sw=%04X", device, hw_ver, sw_ver);
    }

    ESP_LOGI(TAG, "Initialized (I2C port %d, SDA=%d, SCL=%d, patch V%d)",
             config->i2c_port, config->sda_pin, config->scl_pin, config->chip_version);
    return ESP_OK;
}

esp_err_t tef6686_set_power(bool on)
{
    esp_err_t err = tef_send_cmd(MOD_APPL, APPL_SET_OP_MODE, 1, on ? 1 : 0);
    if (err == ESP_OK && !on) {
        // Park the demodulator at 10 MHz on power off
        tef_send_cmd(MOD_FM, RADIO_TUNE_TO, 2, 1, 10000);
    }
    return err;
}

esp_err_t tef6686_get_op_status(uint8_t *bootstatus)
{
    if (!bootstatus) return ESP_ERR_INVALID_ARG;
    uint16_t data[1] = {0};
    esp_err_t err = tef_read_cmd(MOD_APPL, APPL_GET_OP_STATUS, data, 1);
    if (err != ESP_OK) return err;
    *bootstatus = (uint8_t)data[0];
    return ESP_OK;
}

// --- Public API: Tuning ---

esp_err_t tef6686_tune_fm(uint32_t freq_khz)
{
    uint16_t tef_freq = (uint16_t)(freq_khz / 10U);
    esp_err_t err = tef_send_cmd(MOD_FM, RADIO_TUNE_TO, 2, 4, tef_freq);
    if (err == ESP_OK) {
        s_current_band = TEF_BAND_FM;
        s_current_freq = freq_khz;
    }
    return err;
}

esp_err_t tef6686_tune_am(uint32_t freq_khz)
{
    esp_err_t err = tef_send_cmd(MOD_AM, RADIO_TUNE_TO, 2, 1, (uint16_t)freq_khz);
    if (err == ESP_OK) {
        s_current_band = tef_infer_am_band(freq_khz);
        s_current_freq = freq_khz;
    }
    return err;
}

esp_err_t tef6686_set_band(tef_band_t band)
{
    if (band < TEF_BAND_FM || band > TEF_BAND_SW) {
        return ESP_ERR_INVALID_ARG;
    }
    s_current_band = band;
    return ESP_OK;
}

// --- Public API: Audio control ---

esp_err_t tef6686_set_volume(uint8_t volume)
{
    if (volume > 30) volume = 30;
    return tef_send_cmd(MOD_AUDIO, AUDIO_SET_VOLUME, 1, (int)(volume * 10));
}

esp_err_t tef6686_set_mute(bool mute)
{
    if (s_mpx_mode) {
        tef_send_cmd(MOD_FM, RADIO_SET_SPECIALS, 1, mute ? 0 : 1);
    }
    return tef_send_cmd(MOD_AUDIO, AUDIO_SET_MUTE, 1, mute ? 1 : 0);
}

esp_err_t tef6686_set_specials(uint16_t audio)
{
    esp_err_t err = tef_send_cmd(MOD_FM, RADIO_SET_SPECIALS, 1, audio);
    if (err == ESP_OK) {
        s_mpx_mode = (audio != 0);
    }
    return err;
}

esp_err_t tef6686_set_wavegen(bool on, int16_t amplitude, uint16_t freq)
{
    if (on) {
        tef_send_cmd(MOD_AUDIO, AUDIO_SET_INPUT, 1, 240);
        return tef_send_cmd(MOD_AUDIO, AUDIO_SET_WAVEGEN, 6, 5, 0, (int)(amplitude * 10), freq, (int)(amplitude * 10), freq);
    }
    tef_send_cmd(MOD_AUDIO, AUDIO_SET_INPUT, 1, 0);
    return tef_send_cmd(MOD_AUDIO, AUDIO_SET_WAVEGEN, 6, 0, 0, 0, 0, 0, 0);
}

esp_err_t tef6686_set_i2s_input(bool on)
{
    return tef_send_cmd(MOD_AUDIO, AUDIO_SET_INPUT, 1, on ? 32 : 0);
}

esp_err_t tef6686_set_i2s_output(uint32_t sample_rate_hz)
{
    uint16_t tef_sample_rate;

    switch (sample_rate_hz) {
        case 44100:
            tef_sample_rate = 4410;
            break;
        case 48000:
            tef_sample_rate = 4800;
            break;
        default:
            ESP_LOGE(TAG, "unsupported TEF I2S output sample rate: %" PRIu32, sample_rate_hz);
            return ESP_ERR_INVALID_ARG;
    }

    return tef_send_cmd(
        MOD_AUDIO,
        AUDIO_SET_DIG_IO,
        5,
        33,              // I2S digital audio output IIS_SD_1
        2,               // 16-bit I2S format
        16,              // digital audio source
        256,             // TEF6686 drives BCK/WS in master mode
        tef_sample_rate  // TEF API uses sample rate in 10 Hz units
    );
}

// --- Public API: FM signal processing ---

esp_err_t tef6686_set_bandwidth_fm(uint16_t bandwidth_khz)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_BANDWIDTH, 2, 0, (int)(bandwidth_khz * 10));
}

esp_err_t tef6686_set_bandwidth_fm_auto(void)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_BANDWIDTH, 2, 1, 3110);
}

esp_err_t tef6686_extend_bandwidth(bool on)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_BW_OPTIONS, 1, on ? 400 : 950);
}

esp_err_t tef6686_set_deemphasis(uint16_t timeconstant)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_DEEMPH, 1, timeconstant);
}

esp_err_t tef6686_set_agc(uint8_t agc)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_RFAGC, 2, (int)(agc * 10), 0);
}

esp_err_t tef6686_set_softmute_fm(bool on)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_SOFTMUTE, 2, on ? 1 : 0, 200);
}

esp_err_t tef6686_set_mono(bool mono)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_ST_MIN, 1, mono ? 2 : 0);
}

esp_err_t tef6686_set_offset(int8_t offset)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_LEVEL_OFF, 1, (int)(offset * 10 - 70));
}

esp_err_t tef6686_set_fm_noise_blanker(uint16_t start)
{
    if (start == 0) {
        return tef_send_cmd(MOD_FM, RADIO_SET_NB, 2, 0, 1000);
    }
    return tef_send_cmd(MOD_FM, RADIO_SET_NB, 2, 1, (int)(start * 10));
}

esp_err_t tef6686_set_high_cut_level(uint16_t limit)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_HC_MAX, 2, 1, (int)(limit * 100));
}

esp_err_t tef6686_set_high_cut_offset(uint8_t start)
{
    uint8_t mode = (start == 0) ? 0 : 3;
    tef_send_cmd(MOD_FM, RADIO_SET_HC_LEVEL, 3, mode, (int)(start * 10), 300);
    tef_send_cmd(MOD_FM, RADIO_SET_HC_NOISE, 3, mode, 360, 300);
    return tef_send_cmd(MOD_FM, RADIO_SET_HC_MPH, 3, mode, 360, 300);
}

esp_err_t tef6686_set_stereo_level(uint8_t start)
{
    uint8_t mode = (start == 0) ? 0 : 3;
    tef_send_cmd(MOD_FM, RADIO_SET_ST_LEVEL, 3, mode, (int)(start * 10), 60);
    tef_send_cmd(MOD_FM, RADIO_SET_ST_NOISE, 3, mode, 240, 200);
    return tef_send_cmd(MOD_FM, RADIO_SET_ST_MPH, 3, mode, 240, 200);
}

esp_err_t tef6686_set_st_hi_blend_level(uint16_t limit)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_STHB_MAX, 2, 1, (int)(limit * 100));
}

esp_err_t tef6686_set_st_hi_blend_offset(uint8_t start)
{
    uint8_t mode = (start == 0) ? 0 : 3;
    tef_send_cmd(MOD_FM, RADIO_SET_STHB_LEVEL, 3, mode, (int)(start * 10), 300);
    tef_send_cmd(MOD_FM, RADIO_SET_STHB_NOISE, 3, mode, 360, 300);
    return tef_send_cmd(MOD_FM, RADIO_SET_STHB_MPH, 3, mode, 360, 300);
}

esp_err_t tef6686_set_mph_suppression(bool on)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_MPH_SUP, 1, on ? 1 : 0);
}

esp_err_t tef6686_set_channel_eq(bool on)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_CHAN_EQ, 1, on ? 1 : 0);
}

esp_err_t tef6686_set_stereo_improvement(uint8_t mode)
{
    if (mode == 2) {
        return tef_send_cmd(MOD_FM, RADIO_SET_SI, 1, 1);
    }
    return tef_send_cmd(MOD_FM, RADIO_SET_SI, 1, 0);
}

esp_err_t tef6686_set_si_blend_time(uint16_t attack, uint16_t decay)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_STB_TIME, 2, attack, decay);
}

esp_err_t tef6686_set_si_blend_gain(uint16_t b1, uint16_t b2, uint16_t b3, uint16_t b4)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_STB_GAIN, 4, (int)(b1 * 10), (int)(b2 * 10), (int)(b3 * 10), (int)(b4 * 10));
}

esp_err_t tef6686_set_si_blend_bias(int16_t b1, int16_t b2, int16_t b3, int16_t b4)
{
    return tef_send_cmd(MOD_FM, RADIO_SET_STB_BIAS, 4, b1 - 250, b2 - 250, b3 - 250, b4 - 250);
}

esp_err_t tef6686_set_rds(bool full_search)
{
    if (full_search) {
        return tef_send_cmd(MOD_FM, RADIO_SET_RDS, 3, 3, 1, 0);
    }
    return tef_send_cmd(MOD_FM, RADIO_SET_RDS, 3, 1, 1, 0);
}

// --- Public API: AM signal processing ---

esp_err_t tef6686_set_bandwidth_am(uint16_t bandwidth_khz)
{
    return tef_send_cmd(MOD_AM, RADIO_SET_BANDWIDTH, 2, 0, (int)(bandwidth_khz * 10));
}

esp_err_t tef6686_set_am_agc(uint8_t agc)
{
    return tef_send_cmd(MOD_AM, RADIO_SET_RFAGC, 2, (int)(agc * 10), 0);
}

esp_err_t tef6686_set_am_offset(int8_t offset)
{
    return tef_send_cmd(MOD_AM, RADIO_SET_LEVEL_OFF, 1, (int)(offset * 10 - 70));
}

esp_err_t tef6686_set_softmute_am(bool on)
{
    return tef_send_cmd(MOD_AM, RADIO_SET_SOFTMUTE, 2, on ? 1 : 0, 250);
}

esp_err_t tef6686_set_am_noise_blanker(uint16_t start)
{
    if (start == 0) {
        tef_send_cmd(MOD_AM, RADIO_SET_NB, 2, 0, 1000);
        return tef_send_cmd(MOD_AM, RADIO_SET_NB_AUDIO, 2, 0, 1000);
    }
    tef_send_cmd(MOD_AM, RADIO_SET_NB, 2, 1, (int)(start * 10));
    return tef_send_cmd(MOD_AM, RADIO_SET_NB_AUDIO, 2, 1, 1000);
}

esp_err_t tef6686_set_am_cochannel(uint16_t start, uint8_t level)
{
    if (start == 0) {
        return tef_send_cmd(MOD_AM, RADIO_SET_COCHAN, 5, 0, 2, (int)(start * 10), 1000, level);
    }
    return tef_send_cmd(MOD_AM, RADIO_SET_COCHAN, 5, 1, 2, (int)(start * 10), 1000, level);
}

esp_err_t tef6686_set_am_attenuation(uint16_t start)
{
    return tef_send_cmd(MOD_AM, RADIO_SET_ANTENNA, 1, (int)(start * 10));
}

// --- Public API: Hardware ---

esp_err_t tef6686_set_gpio(uint8_t mode)
{
    switch (mode) {
        case 0: return tef_send_cmd(MOD_APPL, APPL_SET_GPIO, 3, 0, 33, 2);
        case 1: return tef_send_cmd(MOD_APPL, APPL_SET_GPIO, 3, 0, 33, 3);
        case 2: return tef_send_cmd(MOD_APPL, APPL_SET_GPIO, 3, 0, 32, 2);
        case 3: return tef_send_cmd(MOD_APPL, APPL_SET_GPIO, 3, 0, 32, 3);
        default: return ESP_ERR_INVALID_ARG;
    }
}

// --- Public API: Status reads ---

esp_err_t tef6686_get_quality(tef_quality_t *quality)
{
    if (!quality) return ESP_ERR_INVALID_ARG;

    uint16_t data[7] = {0};
    esp_err_t err = tef_read_cmd(MOD_FM, RADIO_GET_QUAL_STATUS, data, 7);
    if (err != ESP_OK) return err;

    quality->status = data[0];
    quality->level = (int16_t)data[1];
    quality->usn = data[2];
    quality->wam = data[3];
    quality->offset = (int16_t)data[4];
    quality->bandwidth = data[5] / 10;
    quality->modulation = data[6] / 10;

    if (quality->level < -200) quality->level = -200;
    if (quality->level > 1200) quality->level = 1200;

    quality->snr = (int8_t)(0.46222375f * (float)quality->level / 10.0f
                          - 0.082495118f * (float)quality->usn / 10.0f + 10.0f);

    return ESP_OK;
}

esp_err_t tef6686_get_quality_am(tef_quality_t *quality)
{
    if (!quality) return ESP_ERR_INVALID_ARG;

    uint16_t data[7] = {0};
    esp_err_t err = tef_read_cmd(MOD_AM, RADIO_GET_QUAL_STATUS, data, 7);
    if (err != ESP_OK) return err;

    quality->status = data[0];
    quality->level = (int16_t)data[1];
    quality->usn = data[2];
    quality->wam = data[3];
    quality->offset = (int16_t)data[4];
    quality->bandwidth = data[5] / 10;
    quality->modulation = data[6] / 10;

    if (quality->level < -200) quality->level = -200;
    if (quality->level > 1200) quality->level = 1200;

    quality->snr = (int8_t)(0.46222375f * (float)quality->level / 10.0f
                          - 0.082495118f * (float)(quality->usn / 50) / 10.0f + 10.0f);

    return ESP_OK;
}

esp_err_t tef6686_get_stereo_status(tef_stereo_status_t *st)
{
    if (!st) return ESP_ERR_INVALID_ARG;

    uint16_t data[1] = {0};
    esp_err_t err = tef_read_cmd(MOD_FM, RADIO_GET_SIG_STATUS, data, 1);
    if (err != ESP_OK) return err;

    st->stereo = (data[0] & 0x8000) != 0;

    // Read stereo blend from processing status
    tef_processing_t proc;
    if (tef6686_get_processing(&proc) == ESP_OK) {
        st->stereo_blend = proc.stereo;
    } else {
        st->stereo_blend = 0;
    }
    return ESP_OK;
}

esp_err_t tef6686_get_processing(tef_processing_t *proc)
{
    if (!proc) return ESP_ERR_INVALID_ARG;

    uint16_t data[6] = {0};
    esp_err_t err = tef_read_cmd(MOD_FM, RADIO_GET_PROC_STATUS, data, 6);
    if (err != ESP_OK) return err;

    proc->highcut = data[1] / 10;
    proc->stereo = data[2] / 10;
    proc->st_hi_blend = data[3] / 10;
    proc->st_band[0] = (uint8_t)(data[4] >> 8);
    proc->st_band[1] = (uint8_t)(data[4] & 0xFF);
    proc->st_band[2] = (uint8_t)(data[5] >> 8);
    proc->st_band[3] = (uint8_t)(data[5] & 0xFF);

    return ESP_OK;
}

esp_err_t tef6686_get_rds_status(tef_rds_raw_t *rds)
{
    if (!rds) return ESP_ERR_INVALID_ARG;

    uint16_t data[6] = {0};
    esp_err_t err = tef_read_cmd(MOD_FM, RADIO_GET_RDS_STATUS, data, 6);
    if (err != ESP_OK) return err;

    rds->status = data[0];
    rds->block_a = data[1];
    rds->block_b = data[2];
    rds->block_c = data[3];
    rds->block_d = data[4];
    rds->dec_error = data[5];
    return ESP_OK;
}

esp_err_t tef6686_get_rds_data(tef_rds_raw_t *rds)
{
    if (!rds) return ESP_ERR_INVALID_ARG;

    uint16_t data[6] = {0};
    esp_err_t err = tef_read_cmd(MOD_FM, RADIO_GET_RDS_DATA, data, 6);
    if (err != ESP_OK) return err;

    rds->status = data[0];
    rds->block_a = data[1];
    rds->block_b = data[2];
    rds->block_c = data[3];
    rds->block_d = data[4];
    rds->dec_error = data[5];
    return ESP_OK;
}

esp_err_t tef6686_get_status(tef_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;

    memset(status, 0, sizeof(*status));
    status->band = s_current_band;
    status->frequency = s_current_freq;

    if (s_current_band == TEF_BAND_FM) {
        tef_quality_t q;
        if (tef6686_get_quality(&q) == ESP_OK) {
            status->level = q.level;
            status->snr = q.snr;
            status->bandwidth = q.bandwidth;
            status->tuned = (q.status & 0x0001) != 0;
        }

        tef_stereo_status_t st;
        if (tef6686_get_stereo_status(&st) == ESP_OK) {
            status->stereo = st.stereo;
        }

        tef_rds_raw_t rds;
        if (tef6686_get_rds_status(&rds) == ESP_OK) {
            status->rds_sync = (rds.status & (1U << 9)) != 0;
        }
    } else {
        tef_quality_t q;
        if (tef6686_get_quality_am(&q) == ESP_OK) {
            status->level = q.level;
            status->snr = q.snr;
            status->bandwidth = q.bandwidth;
            status->tuned = (q.status & 0x0001) != 0;
        }
    }

    return ESP_OK;
}

esp_err_t tef6686_get_identification(uint16_t *device, uint16_t *hw_ver, uint16_t *sw_ver)
{
    uint16_t data[3] = {0};
    esp_err_t err = tef_read_cmd(MOD_APPL, APPL_GET_IDENT, data, 3);
    if (err != ESP_OK) return err;

    if (device) *device = data[0];
    if (hw_ver) *hw_ver = data[1];
    if (sw_ver) *sw_ver = data[2];
    return ESP_OK;
}
