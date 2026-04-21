#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// TEF6686 I2C address (7-bit)
#define TEF6686_I2C_ADDR 0x64

// Chip versions for patch selection
typedef enum {
    TEF_VERSION_V102 = 102,
    TEF_VERSION_V205 = 205,
} tef_version_t;

// Tuner bands
typedef enum {
    TEF_BAND_FM,
    TEF_BAND_LW,
    TEF_BAND_MW,
    TEF_BAND_SW,
} tef_band_t;

// Signal quality data
typedef struct {
    uint16_t status;
    int16_t level;      // dBf * 10
    uint16_t usn;       // ultrasonic noise
    uint16_t wam;       // wideband AM noise
    int16_t offset;     // frequency offset
    uint16_t bandwidth; // kHz
    uint16_t modulation;// %
    int8_t snr;         // estimated SNR in dB
} tef_quality_t;

// Stereo/processing status
typedef struct {
    uint16_t stereo_blend; // stereo blend %
    bool stereo;           // stereo decoded
} tef_stereo_status_t;

// Audio processing status
typedef struct {
    uint16_t highcut;      // highcut blend %
    uint16_t stereo;       // stereo blend %
    uint16_t st_hi_blend;  // stereo-high blend %
    uint8_t st_band[4];    // band blend values per SI band
} tef_processing_t;

// RDS raw blocks
typedef struct {
    uint16_t status;
    uint16_t block_a;   // PI code
    uint16_t block_b;
    uint16_t block_c;
    uint16_t block_d;
    uint16_t dec_error; // error flags per block
} tef_rds_raw_t;

// Full tuner status (application-level)
typedef struct {
    tef_band_t band;
    uint32_t frequency;  // kHz
    bool tuned;
    bool stereo;
    bool rds_sync;
    int16_t level;       // dBf * 10
    int8_t snr;          // dB
    uint16_t bandwidth;  // kHz
} tef_status_t;

// Configuration
typedef struct {
    int i2c_port;
    int sda_pin;
    int scl_pin;
    uint32_t i2c_freq_hz;
    tef_version_t chip_version;
    uint8_t xtal_type;   // 0=4MHz, 1=9.216MHz, 2=12MHz, 3=55MHz
} tef_config_t;

// --- Initialization and power ---

esp_err_t tef6686_init(const tef_config_t *config);
esp_err_t tef6686_set_power(bool on);
esp_err_t tef6686_get_op_status(uint8_t *bootstatus);

// --- Tuning ---

esp_err_t tef6686_tune_fm(uint32_t freq_khz);
esp_err_t tef6686_tune_am(uint32_t freq_khz);
esp_err_t tef6686_set_band(tef_band_t band);

// --- Audio control ---

esp_err_t tef6686_set_volume(uint8_t volume);
esp_err_t tef6686_set_mute(bool mute);
esp_err_t tef6686_set_specials(uint16_t audio);
esp_err_t tef6686_set_wavegen(bool on, int16_t amplitude, uint16_t freq);
esp_err_t tef6686_set_i2s_input(bool on);
esp_err_t tef6686_set_i2s_output(uint32_t sample_rate_hz);

// --- FM signal processing ---

esp_err_t tef6686_set_bandwidth_fm(uint16_t bandwidth_khz);
esp_err_t tef6686_set_bandwidth_fm_auto(void);
esp_err_t tef6686_extend_bandwidth(bool on);
esp_err_t tef6686_set_deemphasis(uint16_t timeconstant);
esp_err_t tef6686_set_agc(uint8_t agc);
esp_err_t tef6686_set_softmute_fm(bool on);
esp_err_t tef6686_set_mono(bool mono);
esp_err_t tef6686_set_offset(int8_t offset);
esp_err_t tef6686_set_fm_noise_blanker(uint16_t start);
esp_err_t tef6686_set_high_cut_level(uint16_t limit);
esp_err_t tef6686_set_high_cut_offset(uint8_t start);
esp_err_t tef6686_set_stereo_level(uint8_t start);
esp_err_t tef6686_set_st_hi_blend_level(uint16_t limit);
esp_err_t tef6686_set_st_hi_blend_offset(uint8_t start);
esp_err_t tef6686_set_mph_suppression(bool on);
esp_err_t tef6686_set_channel_eq(bool on);
esp_err_t tef6686_set_stereo_improvement(uint8_t mode);
esp_err_t tef6686_set_si_blend_time(uint16_t attack, uint16_t decay);
esp_err_t tef6686_set_si_blend_gain(uint16_t b1, uint16_t b2, uint16_t b3, uint16_t b4);
esp_err_t tef6686_set_si_blend_bias(int16_t b1, int16_t b2, int16_t b3, int16_t b4);
esp_err_t tef6686_set_rds(bool full_search);

// --- AM signal processing ---

esp_err_t tef6686_set_bandwidth_am(uint16_t bandwidth_khz);
esp_err_t tef6686_set_am_agc(uint8_t agc);
esp_err_t tef6686_set_am_offset(int8_t offset);
esp_err_t tef6686_set_softmute_am(bool on);
esp_err_t tef6686_set_am_noise_blanker(uint16_t start);
esp_err_t tef6686_set_am_cochannel(uint16_t start, uint8_t level);
esp_err_t tef6686_set_am_attenuation(uint16_t start);

// --- Hardware ---

esp_err_t tef6686_set_gpio(uint8_t mode);

// --- Status reads ---

esp_err_t tef6686_get_quality(tef_quality_t *quality);
esp_err_t tef6686_get_quality_am(tef_quality_t *quality);
esp_err_t tef6686_get_stereo_status(tef_stereo_status_t *st);
esp_err_t tef6686_get_processing(tef_processing_t *proc);
esp_err_t tef6686_get_rds_status(tef_rds_raw_t *rds);
esp_err_t tef6686_get_rds_data(tef_rds_raw_t *rds);
esp_err_t tef6686_get_status(tef_status_t *status);
esp_err_t tef6686_get_identification(uint16_t *device, uint16_t *hw_ver, uint16_t *sw_ver);

const char *tef_band_name(tef_band_t band);
