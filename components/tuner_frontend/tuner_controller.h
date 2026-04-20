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
esp_err_t tuner_controller_set_bandwidth_fm(uint16_t bandwidth_khz);

esp_err_t tuner_controller_get_stereo_status(tef_stereo_status_t *st);
esp_err_t tuner_controller_get_rds_data(tef_rds_raw_t *rds);
esp_err_t tuner_controller_get_identification(uint16_t *device, uint16_t *hw_ver, uint16_t *sw_ver);
