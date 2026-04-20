#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int i2s_port;
    int bclk_pin;
    int ws_pin;
    int data_pin;
    uint32_t sample_rate;
    int bits_per_sample;
} audio_config_t;

esp_err_t audio_init(const audio_config_t *config);
esp_err_t audio_start(void);
esp_err_t audio_stop(void);

typedef void (*audio_data_cb_t)(const uint8_t *data, size_t len, void *ctx);

void audio_register_data_cb(audio_data_cb_t cb, void *ctx);
void audio_unregister_data_cb(audio_data_cb_t cb);
