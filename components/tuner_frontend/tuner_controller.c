#include "tuner_controller.h"

#include "audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stddef.h>

#define MAX_STATUS_CBS 4

#define FM_MIN_FREQ_KHZ          64000U
#define FM_MAX_FREQ_KHZ         108000U
#define FM_STEP_KHZ               100U
#define FM_DEFAULT_FREQ_KHZ     87500U
#define LW_MIN_FREQ_KHZ           144U
#define LW_MAX_FREQ_KHZ           519U
#define LW_STEP_KHZ                 9U
#define LW_DEFAULT_FREQ_KHZ       153U
#define MW_MIN_FREQ_KHZ           520U
#define MW_MAX_FREQ_KHZ          1710U
#define MW_STEP_KHZ                 9U
#define MW_DEFAULT_FREQ_KHZ       999U
#define SW_MIN_FREQ_KHZ          1711U
#define SW_MAX_FREQ_KHZ         27000U
#define SW_STEP_KHZ                 5U
#define SW_DEFAULT_FREQ_KHZ      6000U

#define SEEK_WAIT_SLICE_MS         10U
#define SEEK_WAIT_TIMEOUT_MS      500U
#define SEEK_SETTLE_DELAY_MS       80U
#define EVENT_POLL_INTERVAL_MS    100U
#define EVENT_REPORT_INTERVAL_MS 2000U

typedef struct {
    status_cb_t cb;
    void *ctx;
} status_cb_entry_t;

typedef struct {
    tef_band_t band;
    uint32_t min_freq_khz;
    uint32_t max_freq_khz;
    uint32_t step_khz;
    uint32_t default_freq_khz;
} band_profile_t;

static const band_profile_t s_band_profiles[] = {
    {TEF_BAND_FM, FM_MIN_FREQ_KHZ, FM_MAX_FREQ_KHZ, FM_STEP_KHZ, FM_DEFAULT_FREQ_KHZ},
    {TEF_BAND_LW, LW_MIN_FREQ_KHZ, LW_MAX_FREQ_KHZ, LW_STEP_KHZ, LW_DEFAULT_FREQ_KHZ},
    {TEF_BAND_MW, MW_MIN_FREQ_KHZ, MW_MAX_FREQ_KHZ, MW_STEP_KHZ, MW_DEFAULT_FREQ_KHZ},
    {TEF_BAND_SW, SW_MIN_FREQ_KHZ, SW_MAX_FREQ_KHZ, SW_STEP_KHZ, SW_DEFAULT_FREQ_KHZ},
};

static SemaphoreHandle_t s_state_mutex;
static status_cb_entry_t s_status_cbs[MAX_STATUS_CBS];
static TaskHandle_t s_seek_task;
static TaskHandle_t s_event_task;
static volatile bool s_started;
static volatile bool s_seeking;
static volatile bool s_seek_abort;
static tef_band_t s_active_band = TEF_BAND_FM;
static bool s_seek_up;
static bool s_seek_am_mode;
static rds_decoded_t s_rds;
static bool s_user_muted;
static bool s_squelch_muted;
static bool s_scan_muted;
static bool s_effective_muted;
static bool s_auto_squelch_enabled;
static int16_t s_manual_squelch_threshold_db = -1;
static tuner_xdr_settings_t s_xdr_settings = {
    .agc_index = 0,
    .audio_mode = TUNER_XDR_AUDIO_MODE_STEREO_AUTO,
    .ims_enabled = false,
    .eq_enabled = false,
    .softmute_fm = false,
    .softmute_am = false,
    .fm_bandwidth_khz = 0,
    .antenna_index = 0,
};

static const uint8_t s_xdr_fm_agc_values[] = {92, 90, 87, 84};
static const uint8_t s_xdr_am_agc_values[] = {102, 99, 96, 94};

static bool band_is_am(tef_band_t band);
static bool band_is_fm(tef_band_t band);
static esp_err_t read_live_status(tef_status_t *status);
static esp_err_t read_quality_for_band(tef_band_t band, tef_quality_t *quality);

static bool squelch_enabled(void)
{
    return s_auto_squelch_enabled || s_manual_squelch_threshold_db >= 0;
}

static int16_t squelch_close_threshold_for_band(tef_band_t band)
{
    if (s_auto_squelch_enabled) {
        return band_is_fm(band) ? 120 : 60;
    }

    if (s_manual_squelch_threshold_db >= 0) {
        return (int16_t)(s_manual_squelch_threshold_db * 10);
    }

    return 0;
}

static int16_t squelch_open_threshold_for_band(tef_band_t band)
{
    return (int16_t)(squelch_close_threshold_for_band(band) + (band_is_fm(band) ? 30 : 20));
}

static esp_err_t apply_effective_mute(void)
{
    const bool muted = s_user_muted || s_squelch_muted || s_scan_muted;

    if (muted == s_effective_muted) {
        return ESP_OK;
    }

    esp_err_t err = tef6686_set_mute(muted);
    if (err == ESP_OK) {
        s_effective_muted = muted;
    }

    return err;
}

// Fix #6: squelch during seek is unconditional, not gated by squelch_enabled()
static void update_squelch_state(tef_band_t band, bool tuned, const tef_quality_t *quality)
{
    bool squelched = false;

    if (s_seeking) {
        squelched = true;
    } else if (squelch_enabled() && quality != NULL) {
        const int16_t close_threshold = squelch_close_threshold_for_band(band);
        const int16_t open_threshold = squelch_open_threshold_for_band(band);

        if (!tuned) {
            squelched = true;
        } else if (s_squelch_muted) {
            squelched = quality->level < open_threshold;
        } else {
            squelched = quality->level < close_threshold;
        }
    }

    s_squelch_muted = squelched;
    (void)apply_effective_mute();
}

static esp_err_t refresh_squelch_state_now(void)
{
    tef_quality_t quality = {0};
    esp_err_t err;

    if (!squelch_enabled()) {
        s_squelch_muted = false;
        return apply_effective_mute();
    }

    err = read_quality_for_band(s_active_band, &quality);
    if (err != ESP_OK) {
        return err;
    }

    update_squelch_state(s_active_band, (quality.status & 0x0001) != 0, &quality);
    return ESP_OK;
}

static const band_profile_t *band_profile_for(tef_band_t band)
{
    for (size_t i = 0; i < sizeof(s_band_profiles) / sizeof(s_band_profiles[0]); i++) {
        if (s_band_profiles[i].band == band) {
            return &s_band_profiles[i];
        }
    }

    return NULL;
}

static bool band_is_am(tef_band_t band)
{
    return band == TEF_BAND_LW || band == TEF_BAND_MW || band == TEF_BAND_SW;
}

static bool band_is_fm(tef_band_t band)
{
    return band == TEF_BAND_FM;
}

static bool freq_in_band(uint32_t freq_khz, const band_profile_t *profile)
{
    return profile != NULL
        && freq_khz >= profile->min_freq_khz
        && freq_khz <= profile->max_freq_khz;
}

static tef_band_t infer_am_band_from_freq(uint32_t freq_khz)
{
    if (freq_khz < MW_MIN_FREQ_KHZ) {
        return TEF_BAND_LW;
    }

    if (freq_khz <= MW_MAX_FREQ_KHZ) {
        return TEF_BAND_MW;
    }

    return TEF_BAND_SW;
}

static void rds_state_reset_locked(void)
{
    rds_decode_init(&s_rds);
}

static void active_band_sync(tef_band_t band)
{
    s_active_band = band;

    if (!band_is_fm(band)) {
        rds_state_reset_locked();
    }
}

static esp_err_t read_live_status(tef_status_t *status)
{
    esp_err_t err = tef6686_get_status(status);
    if (err == ESP_OK) {
        active_band_sync(status->band);
    }

    return err;
}

static esp_err_t read_quality_for_band(tef_band_t band, tef_quality_t *quality)
{
    if (band_is_fm(band)) {
        return tef6686_get_quality(quality);
    }

    return tef6686_get_quality_am(quality);
}

static uint32_t next_seek_freq(uint32_t current_freq_khz, bool up, const band_profile_t *profile)
{
    int64_t next_freq_khz = up
        ? (int64_t)current_freq_khz + (int64_t)profile->step_khz
        : (int64_t)current_freq_khz - (int64_t)profile->step_khz;

    if (next_freq_khz > (int64_t)profile->max_freq_khz) {
        return profile->min_freq_khz;
    }

    if (next_freq_khz < (int64_t)profile->min_freq_khz) {
        return profile->max_freq_khz;
    }

    return (uint32_t)next_freq_khz;
}

static bool wait_for_seek_idle(void)
{
    uint32_t elapsed_ms = 0;

    while (s_seeking && elapsed_ms < SEEK_WAIT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(SEEK_WAIT_SLICE_MS));
        elapsed_ms += SEEK_WAIT_SLICE_MS;
    }

    return !s_seeking;
}

static uint32_t resolve_seek_start_freq(const band_profile_t *profile, uint32_t current_freq_khz)
{
    if (freq_in_band(current_freq_khz, profile)) {
        return current_freq_khz;
    }

    return profile->default_freq_khz;
}

static esp_err_t switch_to_band_internal(tef_band_t band, uint32_t *applied_freq_khz)
{
    const band_profile_t *profile = band_profile_for(band);
    uint32_t current_freq_khz = 0;
    tef_status_t status = {0};
    esp_err_t err;

    if (!profile) {
        return ESP_ERR_INVALID_ARG;
    }

    if (read_live_status(&status) == ESP_OK) {
        current_freq_khz = status.frequency;
    }

    current_freq_khz = resolve_seek_start_freq(profile, current_freq_khz);

    if (band_is_fm(band)) {
        err = tef6686_tune_fm(current_freq_khz);
        if (err == ESP_OK) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            rds_state_reset_locked();
            xSemaphoreGive(s_state_mutex);
        }
    } else {
        err = tef6686_tune_am(current_freq_khz);
    }

    if (err != ESP_OK) {
        return err;
    }

    active_band_sync(band);
    if (applied_freq_khz) {
        *applied_freq_khz = current_freq_khz;
    }

    return ESP_OK;
}

// Fix #3: snapshot callbacks under mutex before dispatching
static void dispatch_status_update(void)
{
    status_cb_t cbs[MAX_STATUS_CBS];
    void *ctxs[MAX_STATUS_CBS];
    int cb_count = 0;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_STATUS_CBS; i++) {
        if (s_status_cbs[i].cb) {
            cbs[cb_count] = s_status_cbs[i].cb;
            ctxs[cb_count] = s_status_cbs[i].ctx;
            cb_count++;
        }
    }

    if (cb_count == 0) {
        s_rds.ps_complete = false;
        s_rds.rt_changed = false;
        xSemaphoreGive(s_state_mutex);
        return;
    }

    bool rds_changed = s_rds.ps_complete || s_rds.rt_changed;
    s_rds.ps_complete = false;
    s_rds.rt_changed = false;
    xSemaphoreGive(s_state_mutex);

    tuner_state_t snapshot = {0};
    if (read_live_status(&snapshot.status) != ESP_OK) {
        return;
    }

    (void)read_quality_for_band(snapshot.status.band, &snapshot.quality);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snapshot.rds = s_rds;
    xSemaphoreGive(s_state_mutex);

    snapshot.active_band = s_active_band;
    snapshot.seeking = s_seeking;

    for (int i = 0; i < cb_count; i++) {
        cbs[i](&snapshot, rds_changed, ctxs[i]);
    }
}

// Fix #4: avoid redundant quality read — use quality directly for squelch
static void event_task(void *arg)
{
    (void)arg;
    uint32_t elapsed_ms = 0;

    while (1) {
        // RDS decoding under mutex
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (band_is_fm(s_active_band)) {
            tef_rds_raw_t raw = {0};
            if (tef6686_get_rds_data(&raw) == ESP_OK && (raw.status & (1U << 9)) != 0) {
                rds_decode_block(&s_rds, raw.block_a, raw.block_b, raw.block_c, raw.block_d, raw.dec_error);
            }
        } else if (s_rds.has_data) {
            rds_state_reset_locked();
        }
        xSemaphoreGive(s_state_mutex);

        // Quality for squelch — single I2C read instead of read_live_status + read_quality
        tef_quality_t quality = {0};
        tef_band_t band = s_active_band;
        if (read_quality_for_band(band, &quality) == ESP_OK) {
            update_squelch_state(band, (quality.status & 0x0001) != 0, &quality);
        }

        elapsed_ms += EVENT_POLL_INTERVAL_MS;
        if (!s_seeking && elapsed_ms >= EVENT_REPORT_INTERVAL_MS) {
            elapsed_ms = 0;
            dispatch_status_update();
        }

        vTaskDelay(pdMS_TO_TICKS(EVENT_POLL_INTERVAL_MS));
    }
}

static void do_seek_fm(bool up)
{
    const int16_t seek_threshold = 300;
    const band_profile_t *profile = band_profile_for(TEF_BAND_FM);
    tef_status_t st = {0};
    uint32_t freq = profile->default_freq_khz;
    uint32_t attempts = 0;
    uint32_t max_attempts = ((profile->max_freq_khz - profile->min_freq_khz) / profile->step_khz) + 1;

    (void)switch_to_band_internal(TEF_BAND_FM, NULL);
    if (read_live_status(&st) == ESP_OK) {
        freq = resolve_seek_start_freq(profile, st.frequency);
    }

    while (!s_seek_abort && attempts < max_attempts) {
        freq = next_seek_freq(freq, up, profile);
        attempts++;

        if (tef6686_tune_fm(freq) != ESP_OK) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(SEEK_SETTLE_DELAY_MS));

        tef_quality_t quality = {0};
        if (tef6686_get_quality(&quality) == ESP_OK
            && quality.level > seek_threshold
            && (quality.status & 0x0001) != 0) {
            active_band_sync(TEF_BAND_FM);
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            rds_state_reset_locked();
            xSemaphoreGive(s_state_mutex);
            break;
        }
    }

    s_seeking = false;
}

static void do_seek_am(bool up)
{
    const int16_t seek_threshold = 200;
    tef_status_t st = {0};
    tef_band_t band = s_active_band;
    const band_profile_t *profile;
    uint32_t freq;
    uint32_t attempts = 0;
    uint32_t max_attempts;

    if (read_live_status(&st) == ESP_OK && band_is_am(st.band)) {
        band = st.band;
    } else if (!band_is_am(band)) {
        band = infer_am_band_from_freq(st.frequency);
    }

    profile = band_profile_for(band);
    if (profile == NULL) {
        profile = band_profile_for(TEF_BAND_MW);
        band = TEF_BAND_MW;
    }

    freq = resolve_seek_start_freq(profile, st.frequency);
    max_attempts = ((profile->max_freq_khz - profile->min_freq_khz) / profile->step_khz) + 1;

    (void)switch_to_band_internal(band, NULL);

    while (!s_seek_abort && attempts < max_attempts) {
        freq = next_seek_freq(freq, up, profile);
        attempts++;

        if (tef6686_tune_am(freq) != ESP_OK) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(SEEK_SETTLE_DELAY_MS));

        tef_quality_t quality = {0};
        if (tef6686_get_quality_am(&quality) == ESP_OK
            && quality.level > seek_threshold
            && (quality.status & 0x0001) != 0) {
            active_band_sync(band);
            break;
        }
    }

    s_seeking = false;
}

static void seek_task(void *arg)
{
    (void)arg;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_seek_am_mode) {
            do_seek_am(s_seek_up);
        } else {
            do_seek_fm(s_seek_up);
        }
    }
}

esp_err_t tuner_controller_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        return ESP_ERR_NO_MEM;
    }

    tef_status_t status = {0};
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    rds_state_reset_locked();
    xSemaphoreGive(s_state_mutex);
    if (read_live_status(&status) != ESP_OK) {
        active_band_sync(TEF_BAND_FM);
    }

    if (xTaskCreate(event_task, "tuner_events", 4096, NULL, 4, &s_event_task) != pdPASS) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(seek_task, "tuner_seek", 4096, NULL, 5, &s_seek_task) != pdPASS) {
        vTaskDelete(s_event_task);
        s_event_task = NULL;
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    return ESP_OK;
}

// Fix #3: register/unregister under mutex
int tuner_controller_register_cb(status_cb_t cb, void *ctx)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MAX_STATUS_CBS; i++) {
        if (s_status_cbs[i].cb == NULL) {
            s_status_cbs[i].cb = cb;
            s_status_cbs[i].ctx = ctx;
            xSemaphoreGive(s_state_mutex);
            return 0;
        }
    }
    xSemaphoreGive(s_state_mutex);
    return -1;
}

void tuner_controller_unregister_cb(status_cb_t cb)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MAX_STATUS_CBS; i++) {
        if (s_status_cbs[i].cb == cb) {
            s_status_cbs[i].cb = NULL;
            s_status_cbs[i].ctx = NULL;
        }
    }
    xSemaphoreGive(s_state_mutex);
}

// Fix #1: copy s_rds under mutex
tuner_state_t tuner_controller_get_state(void)
{
    tuner_state_t state = {0};
    if (read_live_status(&state.status) == ESP_OK) {
        (void)read_quality_for_band(state.status.band, &state.quality);
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    state.rds = s_rds;
    xSemaphoreGive(s_state_mutex);
    state.active_band = s_active_band;
    state.seeking = s_seeking;
    return state;
}

bool tuner_controller_is_seeking(void)
{
    return s_seeking;
}

esp_err_t tuner_controller_start_seek(bool up, bool am_mode)
{
    if (s_seeking) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!am_mode) {
        esp_err_t err = switch_to_band_internal(TEF_BAND_FM, NULL);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        tef_status_t st = {0};
        tef_band_t band = s_active_band;

        if (read_live_status(&st) == ESP_OK && band_is_am(st.band)) {
            band = st.band;
        } else if (!band_is_am(band)) {
            band = infer_am_band_from_freq(st.frequency);
        }

        esp_err_t err = switch_to_band_internal(band, NULL);
        if (err != ESP_OK) {
            return err;
        }
    }

    s_seek_up = up;
    s_seek_am_mode = am_mode;
    s_seek_abort = false;
    s_seeking = true;
    // Fix #6: always mute during seek, not just when squelch is enabled
    s_squelch_muted = true;
    (void)apply_effective_mute();
    xTaskNotifyGive(s_seek_task);
    return ESP_OK;
}

bool tuner_controller_abort_seek(void)
{
    if (!s_seeking) {
        return true;
    }

    s_seek_abort = true;
    return wait_for_seek_idle();
}

esp_err_t tuner_controller_tune_fm(uint32_t freq_khz)
{
    if (!tuner_controller_abort_seek()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = tef6686_tune_fm(freq_khz);
    if (err == ESP_OK) {
        active_band_sync(TEF_BAND_FM);
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        rds_state_reset_locked();
        xSemaphoreGive(s_state_mutex);
    }

    return err;
}

esp_err_t tuner_controller_tune_am(uint32_t freq_khz)
{
    if (!tuner_controller_abort_seek()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = tef6686_tune_am(freq_khz);
    if (err == ESP_OK) {
        active_band_sync(infer_am_band_from_freq(freq_khz));
    }

    return err;
}

esp_err_t tuner_controller_switch_band(tef_band_t band, uint32_t *applied_freq_khz)
{
    if (!tuner_controller_abort_seek()) {
        return ESP_ERR_TIMEOUT;
    }

    return switch_to_band_internal(band, applied_freq_khz);
}

esp_err_t tuner_controller_set_volume(uint8_t volume)
{
    return tef6686_set_volume(volume);
}

esp_err_t tuner_controller_set_mute(bool mute)
{
    const bool previous = s_user_muted;

    s_user_muted = mute;
    esp_err_t err = apply_effective_mute();
    if (err != ESP_OK) {
        s_user_muted = previous;
    }
    return err;
}

esp_err_t tuner_controller_set_audio(bool on)
{
    return on ? audio_start() : audio_stop();
}

esp_err_t tuner_controller_set_power(bool on)
{
    if (!tuner_controller_abort_seek()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = tef6686_set_power(on);
    if (err == ESP_OK && !on) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        rds_state_reset_locked();
        xSemaphoreGive(s_state_mutex);
    }

    return err;
}

esp_err_t tuner_controller_set_deemphasis(uint16_t timeconstant)
{
    return tef6686_set_deemphasis(timeconstant);
}

// Fix #2: s_xdr_settings writes under mutex
esp_err_t tuner_controller_set_softmute_fm(bool on)
{
    esp_err_t err = tef6686_set_softmute_fm(on);
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_xdr_settings.softmute_fm = on;
        xSemaphoreGive(s_state_mutex);
    }
    return err;
}

esp_err_t tuner_controller_set_softmute_am(bool on)
{
    esp_err_t err = tef6686_set_softmute_am(on);
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_xdr_settings.softmute_am = on;
        xSemaphoreGive(s_state_mutex);
    }
    return err;
}

esp_err_t tuner_controller_set_bandwidth_fm(uint16_t bandwidth_khz)
{
    esp_err_t err;

    if (bandwidth_khz == 0) {
        err = tef6686_set_bandwidth_fm_auto();
    } else {
        err = tef6686_set_bandwidth_fm(bandwidth_khz);
    }

    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_xdr_settings.fm_bandwidth_khz = bandwidth_khz;
        xSemaphoreGive(s_state_mutex);
    }
    return err;
}

esp_err_t tuner_controller_set_agc_index(uint8_t index)
{
    tef_band_t band = s_active_band;

    if (index >= (sizeof(s_xdr_fm_agc_values) / sizeof(s_xdr_fm_agc_values[0]))) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;
    if (band_is_fm(band)) {
        err = tef6686_set_agc(s_xdr_fm_agc_values[index]);
    } else {
        err = tef6686_set_am_agc(s_xdr_am_agc_values[index]);
    }

    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_xdr_settings.agc_index = index;
        xSemaphoreGive(s_state_mutex);
    }

    return err;
}

esp_err_t tuner_controller_set_xdr_audio_mode(tuner_xdr_audio_mode_t mode)
{
    esp_err_t err;

    switch (mode) {
    case TUNER_XDR_AUDIO_MODE_STEREO_AUTO:
        err = tef6686_set_specials(0);
        if (err != ESP_OK) {
            return err;
        }
        err = tef6686_set_mono(false);
        break;
    case TUNER_XDR_AUDIO_MODE_FORCED_MONO:
        err = tef6686_set_specials(0);
        if (err != ESP_OK) {
            return err;
        }
        err = tef6686_set_mono(true);
        break;
    case TUNER_XDR_AUDIO_MODE_MPX:
        err = tef6686_set_mono(true);
        if (err != ESP_OK) {
            return err;
        }
        err = tef6686_set_specials(1);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_xdr_settings.audio_mode = mode;
        xSemaphoreGive(s_state_mutex);
    }

    return err;
}

esp_err_t tuner_controller_set_xdr_eq(bool ims_enabled, bool eq_enabled)
{
    esp_err_t err = tef6686_set_mph_suppression(ims_enabled);
    if (err != ESP_OK) {
        return err;
    }

    err = tef6686_set_channel_eq(eq_enabled);
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_xdr_settings.ims_enabled = ims_enabled;
        s_xdr_settings.eq_enabled = eq_enabled;
        xSemaphoreGive(s_state_mutex);
    }

    return err;
}

esp_err_t tuner_controller_set_antenna(uint8_t antenna_index)
{
    esp_err_t err = tef6686_set_gpio(antenna_index);
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_xdr_settings.antenna_index = antenna_index;
        xSemaphoreGive(s_state_mutex);
    }
    return err;
}

esp_err_t tuner_controller_set_auto_squelch(bool on)
{
    const bool previous = s_auto_squelch_enabled;

    s_auto_squelch_enabled = on;
    esp_err_t err = refresh_squelch_state_now();
    if (err != ESP_OK) {
        s_auto_squelch_enabled = previous;
        (void)refresh_squelch_state_now();
    }
    return err;
}

esp_err_t tuner_controller_set_squelch(int16_t threshold_db)
{
    const int16_t previous = s_manual_squelch_threshold_db;

    s_manual_squelch_threshold_db = threshold_db;
    esp_err_t err = refresh_squelch_state_now();
    if (err != ESP_OK) {
        s_manual_squelch_threshold_db = previous;
        (void)refresh_squelch_state_now();
    }
    return err;
}

esp_err_t tuner_controller_set_scan_mute(bool mute)
{
    const bool previous = s_scan_muted;

    s_scan_muted = mute;
    esp_err_t err = apply_effective_mute();
    if (err != ESP_OK) {
        s_scan_muted = previous;
    }
    return err;
}

// Fix #2: s_xdr_settings read under mutex
void tuner_controller_get_xdr_settings(tuner_xdr_settings_t *settings)
{
    if (settings != NULL) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        *settings = s_xdr_settings;
        xSemaphoreGive(s_state_mutex);
    }
}

// Fix #5: use s_active_band instead of heavy read_live_status
esp_err_t tuner_controller_get_stereo_status(tef_stereo_status_t *st)
{
    if (!band_is_fm(s_active_band)) {
        return ESP_ERR_INVALID_STATE;
    }

    return tef6686_get_stereo_status(st);
}

esp_err_t tuner_controller_get_rds_data(tef_rds_raw_t *rds)
{
    if (!band_is_fm(s_active_band)) {
        return ESP_ERR_INVALID_STATE;
    }

    return tef6686_get_rds_data(rds);
}

esp_err_t tuner_controller_get_identification(uint16_t *device, uint16_t *hw_ver, uint16_t *sw_ver)
{
    return tef6686_get_identification(device, hw_ver, sw_ver);
}
