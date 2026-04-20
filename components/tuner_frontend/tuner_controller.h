#pragma once

#include "esp_err.h"
#include "rds_decode.h"
#include "tef6686.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    tef_status_t status;
    tef_quality_t quality;
    rds_decoded_t rds;
    tef_band_t active_band;
    bool seeking;
} tuner_state_t;

typedef enum {
    TUNER_XDR_AUDIO_MODE_STEREO_AUTO = 0,
    TUNER_XDR_AUDIO_MODE_FORCED_MONO,
    TUNER_XDR_AUDIO_MODE_MPX,
} tuner_xdr_audio_mode_t;

typedef struct {
    uint8_t agc_index;
    tuner_xdr_audio_mode_t audio_mode;
    bool ims_enabled;
    bool eq_enabled;
    uint16_t deemphasis_us;
    bool softmute_fm;
    bool softmute_am;
    uint16_t fm_bandwidth_khz;
    uint8_t antenna_index;
} tuner_xdr_settings_t;

typedef void (*status_cb_t)(const tuner_state_t *state, bool rds_changed, void *ctx);

esp_err_t tuner_controller_start(void);

int tuner_controller_register_cb(status_cb_t cb, void *ctx);
void tuner_controller_unregister_cb(status_cb_t cb);

tuner_state_t tuner_controller_get_state(void);

bool tuner_controller_is_seeking(void);
esp_err_t tuner_controller_start_seek(bool up, bool am_mode);
bool tuner_controller_abort_seek(void);

esp_err_t tuner_controller_tune_fm(uint32_t freq_khz);
esp_err_t tuner_controller_tune_am(uint32_t freq_khz);
esp_err_t tuner_controller_switch_band(tef_band_t band, uint32_t *applied_freq_khz);

esp_err_t tuner_controller_set_volume(uint8_t volume);
esp_err_t tuner_controller_set_mute(bool mute);
esp_err_t tuner_controller_set_audio(bool on);
esp_err_t tuner_controller_set_power(bool on);
esp_err_t tuner_controller_set_deemphasis(uint16_t timeconstant);
esp_err_t tuner_controller_set_softmute_fm(bool on);
esp_err_t tuner_controller_set_softmute_am(bool on);
esp_err_t tuner_controller_set_bandwidth_fm(uint16_t bandwidth_khz);
esp_err_t tuner_controller_set_agc_index(uint8_t index);
esp_err_t tuner_controller_set_xdr_audio_mode(tuner_xdr_audio_mode_t mode);
esp_err_t tuner_controller_set_xdr_eq(bool ims_enabled, bool eq_enabled);
esp_err_t tuner_controller_set_antenna(uint8_t antenna_index);
esp_err_t tuner_controller_set_auto_squelch(bool on);
esp_err_t tuner_controller_set_squelch(int16_t threshold_db);
esp_err_t tuner_controller_set_scan_mute(bool mute);
void tuner_controller_get_xdr_settings(tuner_xdr_settings_t *settings);

esp_err_t tuner_controller_get_stereo_status(tef_stereo_status_t *st);
esp_err_t tuner_controller_get_rds_data(tef_rds_raw_t *rds);
esp_err_t tuner_controller_get_identification(uint16_t *device, uint16_t *hw_ver, uint16_t *sw_ver);
