#pragma once

#include "esp_err.h"
#include "tuner_controller.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t version;
    uint16_t deemphasis_us;
    uint16_t fm_bandwidth_khz;
    uint32_t fm_freq_khz;
    uint32_t lw_freq_khz;
    uint32_t mw_freq_khz;
    uint32_t sw_freq_khz;
    int16_t squelch_threshold_db;
    uint8_t volume;
    uint8_t agc_index;
    uint8_t antenna_index;
    tef_band_t active_band;
    tuner_xdr_audio_mode_t audio_mode;
    bool muted;
    bool softmute_fm;
    bool softmute_am;
    bool ims_enabled;
    bool eq_enabled;
    bool auto_squelch_enabled;
} app_settings_t;

typedef enum {
    APP_SETTINGS_BUSY_OWNER_NONE = 0,
    APP_SETTINGS_BUSY_OWNER_WEB_SPECTRUM,
    APP_SETTINGS_BUSY_OWNER_XDR_SCAN,
} app_settings_busy_owner_t;

esp_err_t app_settings_init(void);
esp_err_t app_settings_apply(void);
void app_settings_get(app_settings_t *settings);
esp_err_t app_settings_begin_busy(app_settings_busy_owner_t owner);
void app_settings_end_busy(app_settings_busy_owner_t owner);

esp_err_t app_settings_tune_fm(uint32_t freq_khz);
esp_err_t app_settings_tune_am(uint32_t freq_khz);
esp_err_t app_settings_switch_band(tef_band_t band, uint32_t *applied_freq_khz);
esp_err_t app_settings_start_seek(bool up, bool am_mode);
esp_err_t app_settings_set_volume(uint8_t volume);
esp_err_t app_settings_set_mute(bool mute);
esp_err_t app_settings_set_deemphasis(uint16_t timeconstant);
esp_err_t app_settings_set_softmute_fm(bool on);
esp_err_t app_settings_set_softmute_am(bool on);
esp_err_t app_settings_set_bandwidth_fm(uint16_t bandwidth_khz);
esp_err_t app_settings_set_agc_index(uint8_t index);
esp_err_t app_settings_set_xdr_audio_mode(tuner_xdr_audio_mode_t mode);
esp_err_t app_settings_set_xdr_eq(bool ims_enabled, bool eq_enabled);
esp_err_t app_settings_set_antenna(uint8_t antenna_index);
esp_err_t app_settings_set_auto_squelch(bool on);
esp_err_t app_settings_set_squelch(int16_t threshold_db);
