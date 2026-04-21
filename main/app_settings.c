#include "app_settings.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#define SETTINGS_VERSION   1U

#define DEFAULT_FM_FREQ_KHZ 87500U
#define DEFAULT_LW_FREQ_KHZ   153U
#define DEFAULT_MW_FREQ_KHZ   999U
#define DEFAULT_SW_FREQ_KHZ  6000U

static SemaphoreHandle_t s_settings_mutex;
static app_settings_t s_settings;
static bool s_initialized;
static app_settings_busy_owner_t s_busy_owner;

static bool app_settings_ready(void)
{
    return s_initialized && s_settings_mutex != NULL;
}

static uint32_t default_freq_for_band(tef_band_t band)
{
    switch (band) {
    case TEF_BAND_FM:
        return DEFAULT_FM_FREQ_KHZ;
    case TEF_BAND_LW:
        return DEFAULT_LW_FREQ_KHZ;
    case TEF_BAND_MW:
        return DEFAULT_MW_FREQ_KHZ;
    case TEF_BAND_SW:
        return DEFAULT_SW_FREQ_KHZ;
    default:
        return DEFAULT_FM_FREQ_KHZ;
    }
}

static void app_settings_set_defaults(app_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->version = SETTINGS_VERSION;
    settings->deemphasis_us = 50;
    settings->fm_freq_khz = DEFAULT_FM_FREQ_KHZ;
    settings->lw_freq_khz = DEFAULT_LW_FREQ_KHZ;
    settings->mw_freq_khz = DEFAULT_MW_FREQ_KHZ;
    settings->sw_freq_khz = DEFAULT_SW_FREQ_KHZ;
    settings->squelch_threshold_db = -1;
    settings->volume = CONFIG_TUNER_DEFAULT_VOLUME;
    settings->agc_index = 0;
    settings->antenna_index = 0;
    settings->active_band = CONFIG_TUNER_DEFAULT_BAND_FM ? TEF_BAND_FM : TEF_BAND_MW;
    settings->audio_mode = TUNER_XDR_AUDIO_MODE_STEREO_AUTO;
}

static void app_settings_normalize(app_settings_t *settings)
{
    if (settings->version != SETTINGS_VERSION) {
        app_settings_set_defaults(settings);
        return;
    }

    if (settings->volume > 30) {
        settings->volume = CONFIG_TUNER_DEFAULT_VOLUME;
    }
    if (settings->agc_index > 3) {
        settings->agc_index = 0;
    }
    if (settings->antenna_index > 3) {
        settings->antenna_index = 0;
    }
    if (settings->audio_mode > TUNER_XDR_AUDIO_MODE_MPX) {
        settings->audio_mode = TUNER_XDR_AUDIO_MODE_STEREO_AUTO;
    }
    if (settings->active_band != TEF_BAND_FM
        && settings->active_band != TEF_BAND_LW
        && settings->active_band != TEF_BAND_MW
        && settings->active_band != TEF_BAND_SW) {
        settings->active_band = CONFIG_TUNER_DEFAULT_BAND_FM ? TEF_BAND_FM : TEF_BAND_MW;
    }
    if (settings->deemphasis_us != 0 && settings->deemphasis_us != 50 && settings->deemphasis_us != 75) {
        settings->deemphasis_us = 50;
    }
    if (settings->fm_freq_khz == 0) {
        settings->fm_freq_khz = DEFAULT_FM_FREQ_KHZ;
    }
    if (settings->lw_freq_khz == 0) {
        settings->lw_freq_khz = DEFAULT_LW_FREQ_KHZ;
    }
    if (settings->mw_freq_khz == 0) {
        settings->mw_freq_khz = DEFAULT_MW_FREQ_KHZ;
    }
    if (settings->sw_freq_khz == 0) {
        settings->sw_freq_khz = DEFAULT_SW_FREQ_KHZ;
    }
}

static esp_err_t persist_settings_locked(const char *reason)
{
    (void)reason;
    return ESP_OK;
}

static esp_err_t app_settings_check_busy(app_settings_busy_owner_t owner)
{
    esp_err_t err = ESP_OK;

    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    if (s_busy_owner != APP_SETTINGS_BUSY_OWNER_NONE && s_busy_owner != owner) {
        err = ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_settings_mutex);
    return err;
}

static void update_band_frequency_locked(tef_band_t band, uint32_t freq_khz)
{
    s_settings.active_band = band;

    switch (band) {
    case TEF_BAND_FM:
        s_settings.fm_freq_khz = freq_khz;
        break;
    case TEF_BAND_LW:
        s_settings.lw_freq_khz = freq_khz;
        break;
    case TEF_BAND_MW:
        s_settings.mw_freq_khz = freq_khz;
        break;
    case TEF_BAND_SW:
        s_settings.sw_freq_khz = freq_khz;
        break;
    default:
        break;
    }
}

static uint32_t frequency_for_band(const app_settings_t *settings, tef_band_t band)
{
    switch (band) {
    case TEF_BAND_FM:
        return settings->fm_freq_khz;
    case TEF_BAND_LW:
        return settings->lw_freq_khz;
    case TEF_BAND_MW:
        return settings->mw_freq_khz;
    case TEF_BAND_SW:
        return settings->sw_freq_khz;
    default:
        return default_freq_for_band(band);
    }
}

static void sync_tuned_frequency_from_state_locked(const tuner_state_t *state, uint32_t fallback_freq_khz)
{
    uint32_t applied_freq_khz = state->status.frequency > 0 ? state->status.frequency : fallback_freq_khz;
    update_band_frequency_locked(state->active_band, applied_freq_khz);
}

esp_err_t app_settings_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_settings_mutex = xSemaphoreCreateMutex();
    if (s_settings_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    app_settings_t loaded = {0};
    app_settings_set_defaults(&loaded);
    app_settings_normalize(&loaded);

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings = loaded;
    s_busy_owner = APP_SETTINGS_BUSY_OWNER_NONE;
    s_initialized = true;
    xSemaphoreGive(s_settings_mutex);

    return ESP_OK;
}

void app_settings_get(app_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    if (!s_initialized || s_settings_mutex == NULL) {
        app_settings_set_defaults(settings);
        return;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    *settings = s_settings;
    xSemaphoreGive(s_settings_mutex);
}

esp_err_t app_settings_apply(void)
{
    if (!app_settings_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    app_settings_t settings;
    app_settings_get(&settings);

    uint32_t target_freq_khz = frequency_for_band(&settings, settings.active_band);
    esp_err_t err;

    if (settings.active_band == TEF_BAND_FM) {
        err = tuner_controller_tune_fm(target_freq_khz);
    } else {
        err = tuner_controller_tune_am(target_freq_khz);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_volume(settings.volume);
    if (err != ESP_OK) {
        return err;
    }
    err = tuner_controller_set_deemphasis(settings.deemphasis_us);
    if (err != ESP_OK) {
        return err;
    }
    err = tuner_controller_set_softmute_fm(settings.softmute_fm);
    if (err != ESP_OK) {
        return err;
    }
    err = tuner_controller_set_softmute_am(settings.softmute_am);
    if (err != ESP_OK) {
        return err;
    }
    err = tuner_controller_set_bandwidth_fm(settings.fm_bandwidth_khz);
    if (err != ESP_OK) {
        return err;
    }
    err = tuner_controller_set_agc_index(settings.agc_index);
    if (err != ESP_OK) {
        return err;
    }
    err = tuner_controller_set_xdr_audio_mode(settings.audio_mode);
    if (err != ESP_OK) {
        return err;
    }
    err = tuner_controller_set_xdr_eq(settings.ims_enabled, settings.eq_enabled);
    if (err != ESP_OK) {
        return err;
    }
    err = tuner_controller_set_antenna(settings.antenna_index);
    if (err != ESP_OK) {
        return err;
    }
    err = tuner_controller_set_auto_squelch(settings.auto_squelch_enabled);
    if (err != ESP_OK) {
        return err;
    }
    err = tuner_controller_set_squelch(settings.squelch_threshold_db);
    if (err != ESP_OK) {
        return err;
    }
    return tuner_controller_set_mute(settings.muted);
}

esp_err_t app_settings_begin_busy(app_settings_busy_owner_t owner)
{
    esp_err_t err = ESP_OK;

    if (owner == APP_SETTINGS_BUSY_OWNER_NONE || !app_settings_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    if (s_busy_owner != APP_SETTINGS_BUSY_OWNER_NONE && s_busy_owner != owner) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_busy_owner = owner;
    }
    xSemaphoreGive(s_settings_mutex);

    return err;
}

void app_settings_end_busy(app_settings_busy_owner_t owner)
{
    if (owner == APP_SETTINGS_BUSY_OWNER_NONE || !app_settings_ready()) {
        return;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    if (s_busy_owner == owner) {
        s_busy_owner = APP_SETTINGS_BUSY_OWNER_NONE;
    }
    xSemaphoreGive(s_settings_mutex);
}

esp_err_t app_settings_tune_fm(uint32_t freq_khz)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_tune_fm(freq_khz);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    tuner_state_t state = tuner_controller_get_state();
    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    sync_tuned_frequency_from_state_locked(&state, freq_khz);
    (void)persist_settings_locked("tune_fm");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_tune_am(uint32_t freq_khz)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_tune_am(freq_khz);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    tuner_state_t state = tuner_controller_get_state();
    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    sync_tuned_frequency_from_state_locked(&state, freq_khz);
    (void)persist_settings_locked("tune_am");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_switch_band(tef_band_t band, uint32_t *applied_freq_khz)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t applied_freq = 0;
    err = tuner_controller_switch_band(band, &applied_freq);
    if (err != ESP_OK) {
        return err;
    }

    if (applied_freq_khz != NULL) {
        *applied_freq_khz = applied_freq;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    update_band_frequency_locked(band, applied_freq > 0 ? applied_freq : default_freq_for_band(band));
    (void)persist_settings_locked("switch_band");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_start_seek(bool up, bool am_mode)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    return tuner_controller_start_seek(up, am_mode);
}

esp_err_t app_settings_set_volume(uint8_t volume)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_volume(volume);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings.volume = volume;
    (void)persist_settings_locked("set_volume");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_set_mute(bool mute)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_mute(mute);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings.muted = mute;
    (void)persist_settings_locked("set_mute");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_set_deemphasis(uint16_t timeconstant)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_deemphasis(timeconstant);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings.deemphasis_us = timeconstant;
    (void)persist_settings_locked("set_deemphasis");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_set_softmute_fm(bool on)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_softmute_fm(on);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings.softmute_fm = on;
    (void)persist_settings_locked("set_softmute_fm");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_set_softmute_am(bool on)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_softmute_am(on);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings.softmute_am = on;
    (void)persist_settings_locked("set_softmute_am");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_set_bandwidth_fm(uint16_t bandwidth_khz)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_bandwidth_fm(bandwidth_khz);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings.fm_bandwidth_khz = bandwidth_khz;
    (void)persist_settings_locked("set_bandwidth_fm");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_set_agc_index(uint8_t index)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_agc_index(index);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings.agc_index = index;
    (void)persist_settings_locked("set_agc_index");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_set_xdr_audio_mode(tuner_xdr_audio_mode_t mode)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_xdr_audio_mode(mode);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings.audio_mode = mode;
    (void)persist_settings_locked("set_audio_mode");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_set_xdr_eq(bool ims_enabled, bool eq_enabled)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_xdr_eq(ims_enabled, eq_enabled);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings.ims_enabled = ims_enabled;
    s_settings.eq_enabled = eq_enabled;
    (void)persist_settings_locked("set_xdr_eq");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_set_antenna(uint8_t antenna_index)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_antenna(antenna_index);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings.antenna_index = antenna_index;
    (void)persist_settings_locked("set_antenna");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_set_auto_squelch(bool on)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_auto_squelch(on);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings.auto_squelch_enabled = on;
    (void)persist_settings_locked("set_auto_squelch");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}

esp_err_t app_settings_set_squelch(int16_t threshold_db)
{
    esp_err_t err = app_settings_check_busy(APP_SETTINGS_BUSY_OWNER_NONE);
    if (err != ESP_OK) {
        return err;
    }

    err = tuner_controller_set_squelch(threshold_db);
    if (err != ESP_OK) {
        return err;
    }
    if (!app_settings_ready()) {
        return ESP_OK;
    }

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings.squelch_threshold_db = threshold_db;
    (void)persist_settings_locked("set_squelch");
    xSemaphoreGive(s_settings_mutex);
    return ESP_OK;
}
