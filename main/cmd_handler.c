#include "cmd_handler.h"

#include "app_settings.h"
#include "tuner_controller.h"
#include "usb_cdc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool s_events_enabled;

static void serial_status_cb(const tuner_state_t *state, bool rds_changed, void *ctx)
{
    (void)ctx;

    if (!s_events_enabled) {
        return;
    }

    usb_cdc_write_fmt(
        "EV_STATUS band=%s freq=%" PRIu32 " tuned=%d stereo=%d rds=%d level=%.1f snr=%d\r\n",
        tef_band_name(state->status.band),
        state->status.frequency,
        state->status.tuned ? 1 : 0,
        state->status.stereo ? 1 : 0,
        state->status.rds_sync ? 1 : 0,
        state->status.level / 10.0f,
        state->status.snr
    );

    if (rds_changed && state->status.band == TEF_BAND_FM && state->rds.has_data) {
        usb_cdc_write_fmt(
            "EV_RDS pi=%04X pty=%d tp=%d ta=%d ps=\"%.8s\" rt=\"%.64s\"\r\n",
            state->rds.pi,
            state->rds.pty,
            state->rds.tp ? 1 : 0,
            state->rds.ta ? 1 : 0,
            state->rds.ps,
            state->rds.rt
        );
    }
}

static void cmd_unknown(void)
{
    usb_cdc_write_str("ERROR unknown command\r\n");
}

static void cmd_help(void)
{
    usb_cdc_write_str(
        "Commands:\r\n"
        "  TUNE <freq_khz>     Tune FM (e.g. 87500 = 87.5 MHz)\r\n"
        "  TUNEAM <freq_khz>   Tune AM and infer LW/MW/SW from frequency\r\n"
        "  SEEK UP|DOWN        Seek FM station\r\n"
        "  SEEKAM UP|DOWN      Seek AM station\r\n"
        "  SEEKSTOP            Abort seek\r\n"
        "  BAND FM|LW|MW|SW    Switch band\r\n"
        "  STATUS              Show current tuner status\r\n"
        "  QUALITY             Show band-aware signal quality\r\n"
        "  STEREO              Show stereo status (FM only)\r\n"
        "  RDS                 Show raw RDS blocks (FM only)\r\n"
        "  RDSDEC              Show decoded RDS (FM only)\r\n"
        "  VOLUME <0-30>       Set volume\r\n"
        "  MUTE ON|OFF         Mute/unmute\r\n"
        "  AUDIO ON|OFF        Start/stop USB audio\r\n"
        "  EVENTS ON|OFF       Auto-report RDS/status events\r\n"
        "  POWER ON|OFF        Power on/off TEF6686\r\n"
        "  IDENT               Show chip identification\r\n"
        "  HELP                Show this help\r\n"
    );
}

static void cmd_tune(const char *arg)
{
    if (!arg || !*arg) {
        usb_cdc_write_str("ERROR TUNE requires frequency\r\n");
        return;
    }

    long freq = strtol(arg, NULL, 10);
    if (freq < 64000 || freq > 108000) {
        usb_cdc_write_str("ERROR FM frequency out of range 64000-108000 kHz\r\n");
        return;
    }

    esp_err_t err = app_settings_tune_fm((uint32_t)freq);
    if (err != ESP_OK) {
        usb_cdc_write_fmt("ERROR tune: %s\r\n", esp_err_to_name(err));
        return;
    }

    usb_cdc_write_fmt("OK tuned FM %ld kHz\r\n", freq);
}

static void cmd_tuneam(const char *arg)
{
    if (!arg || !*arg) {
        usb_cdc_write_str("ERROR TUNEAM requires frequency\r\n");
        return;
    }

    long freq = strtol(arg, NULL, 10);
    if (freq < 144 || freq > 27000) {
        usb_cdc_write_str("ERROR AM frequency out of range 144-27000 kHz\r\n");
        return;
    }

    esp_err_t err = app_settings_tune_am((uint32_t)freq);
    if (err != ESP_OK) {
        usb_cdc_write_fmt("ERROR tune AM: %s\r\n", esp_err_to_name(err));
        return;
    }

    tuner_state_t state = tuner_controller_get_state();
    usb_cdc_write_fmt("OK tuned %s %ld kHz\r\n", tef_band_name(state.active_band), freq);
}

static bool parse_seek_direction(const char *arg, bool *up)
{
    if (!arg || !*arg || strcasecmp(arg, "UP") == 0) {
        *up = true;
        return true;
    }

    if (strcasecmp(arg, "DOWN") == 0) {
        *up = false;
        return true;
    }

    return false;
}

static void cmd_seek(const char *arg)
{
    bool up;

    if (!parse_seek_direction(arg, &up)) {
        usb_cdc_write_str("ERROR SEEK requires UP or DOWN\r\n");
        return;
    }

    esp_err_t err = app_settings_start_seek(up, false);
    if (err != ESP_OK) {
        usb_cdc_write_fmt("ERROR seek: %s\r\n", esp_err_to_name(err));
        return;
    }

    usb_cdc_write_fmt("OK seek=STARTED direction=%s band=FM\r\n", up ? "UP" : "DOWN");
}

static void cmd_seekam(const char *arg)
{
    bool up;

    if (!parse_seek_direction(arg, &up)) {
        usb_cdc_write_str("ERROR SEEKAM requires UP or DOWN\r\n");
        return;
    }

    esp_err_t err = app_settings_start_seek(up, true);
    if (err != ESP_OK) {
        usb_cdc_write_fmt("ERROR seek AM: %s\r\n", esp_err_to_name(err));
        return;
    }

    usb_cdc_write_fmt("OK seek=STARTED direction=%s band=AM\r\n", up ? "UP" : "DOWN");
}

static void cmd_seekstop(void)
{
    if (tuner_controller_abort_seek()) {
        usb_cdc_write_str("OK seek=STOPPED\r\n");
    } else {
        usb_cdc_write_str("ERROR seek stop timed out\r\n");
    }
}

static void cmd_band(const char *arg)
{
    if (!arg || !*arg) {
        usb_cdc_write_str("ERROR BAND requires FM|LW|MW|SW\r\n");
        return;
    }

    tef_band_t band;
    if (strcasecmp(arg, "FM") == 0) band = TEF_BAND_FM;
    else if (strcasecmp(arg, "LW") == 0) band = TEF_BAND_LW;
    else if (strcasecmp(arg, "MW") == 0) band = TEF_BAND_MW;
    else if (strcasecmp(arg, "SW") == 0) band = TEF_BAND_SW;
    else {
        usb_cdc_write_str("ERROR unknown band\r\n");
        return;
    }

    uint32_t applied_freq_khz = 0;
    esp_err_t err = app_settings_switch_band(band, &applied_freq_khz);
    if (err != ESP_OK) {
        usb_cdc_write_fmt("ERROR band: %s\r\n", esp_err_to_name(err));
        return;
    }

    usb_cdc_write_fmt("OK band=%s freq=%" PRIu32 "\r\n", tef_band_name(band), applied_freq_khz);
}

static void cmd_status(void)
{
    tuner_state_t state = tuner_controller_get_state();

    usb_cdc_write_fmt(
        "STATUS band=%s freq=%" PRIu32 " tuned=%d stereo=%d rds=%d level=%.1fdB snr=%ddB bw=%ukHz\r\n",
        tef_band_name(state.status.band),
        state.status.frequency,
        state.status.tuned ? 1 : 0,
        state.status.stereo ? 1 : 0,
        state.status.rds_sync ? 1 : 0,
        state.status.level / 10.0f,
        state.status.snr,
        state.status.bandwidth
    );
}

static void cmd_quality(void)
{
    tuner_state_t state = tuner_controller_get_state();

    usb_cdc_write_fmt(
        "QUALITY band=%s status=%04X level=%.1fdB usn=%u wam=%u offset=%d bw=%ukHz mod=%u%% snr=%ddB\r\n",
        tef_band_name(state.status.band),
        state.quality.status,
        state.quality.level / 10.0f,
        state.quality.usn,
        state.quality.wam,
        state.quality.offset,
        state.quality.bandwidth,
        state.quality.modulation,
        state.quality.snr
    );
}

static void cmd_stereo(void)
{
    tef_stereo_status_t st = {0};
    esp_err_t err = tuner_controller_get_stereo_status(&st);
    if (err != ESP_OK) {
        usb_cdc_write_fmt("ERROR stereo: %s\r\n", esp_err_to_name(err));
        return;
    }

    usb_cdc_write_fmt("STEREO decoded=%d blend=%u\r\n", st.stereo ? 1 : 0, st.stereo_blend);
}

static void cmd_rds(void)
{
    tef_rds_raw_t rds = {0};
    esp_err_t err = tuner_controller_get_rds_data(&rds);
    if (err != ESP_OK) {
        usb_cdc_write_fmt("ERROR RDS: %s\r\n", esp_err_to_name(err));
        return;
    }

    usb_cdc_write_fmt("RDS %04X %04X %04X %04X %04X %04X\r\n",
        rds.status, rds.block_a, rds.block_b, rds.block_c, rds.block_d, rds.dec_error);
}

static void cmd_rdsdec(void)
{
    tuner_state_t state = tuner_controller_get_state();

    if (state.status.band != TEF_BAND_FM) {
        usb_cdc_write_fmt("ERROR RDSDEC unavailable in %s\r\n", tef_band_name(state.status.band));
        return;
    }

    if (!state.rds.has_data) {
        usb_cdc_write_str("RDSDEC no data\r\n");
        return;
    }

    usb_cdc_write_fmt(
        "RDSDEC pi=%04X pty=%d tp=%d ta=%d ms=%d ps=\"%.8s\" rt=\"%.64s\"\r\n",
        state.rds.pi,
        state.rds.pty,
        state.rds.tp ? 1 : 0,
        state.rds.ta ? 1 : 0,
        state.rds.ms ? 1 : 0,
        state.rds.ps,
        state.rds.rt
    );
}

static void cmd_volume(const char *arg)
{
    if (!arg || !*arg) {
        usb_cdc_write_str("ERROR VOLUME requires 0-30\r\n");
        return;
    }

    long vol = strtol(arg, NULL, 10);
    if (vol < 0 || vol > 30) {
        usb_cdc_write_str("ERROR range 0-30\r\n");
        return;
    }

    esp_err_t err = app_settings_set_volume((uint8_t)vol);
    if (err != ESP_OK) {
        usb_cdc_write_fmt("ERROR volume: %s\r\n", esp_err_to_name(err));
        return;
    }

    usb_cdc_write_fmt("OK volume=%ld\r\n", vol);
}

static void cmd_mute(const char *arg)
{
    bool mute = true;
    if (arg && strcasecmp(arg, "OFF") == 0) {
        mute = false;
    }

    esp_err_t err = app_settings_set_mute(mute);
    if (err != ESP_OK) {
        usb_cdc_write_fmt("ERROR mute: %s\r\n", esp_err_to_name(err));
        return;
    }

    usb_cdc_write_fmt("OK mute=%s\r\n", mute ? "ON" : "OFF");
}

static void cmd_audio(const char *arg)
{
    bool on = true;
    if (arg && strcasecmp(arg, "OFF") == 0) {
        on = false;
    }

    esp_err_t err = tuner_controller_set_audio(on);
    if (err != ESP_OK) {
        usb_cdc_write_fmt("ERROR audio: %s\r\n", esp_err_to_name(err));
        return;
    }

    usb_cdc_write_fmt("OK audio=%s\r\n", on ? "ON" : "OFF");
}

static void cmd_events(const char *arg)
{
    s_events_enabled = !(arg && strcasecmp(arg, "OFF") == 0);
    usb_cdc_write_fmt("OK events=%s\r\n", s_events_enabled ? "ON" : "OFF");
}

static void cmd_power(const char *arg)
{
    bool on = true;
    if (arg && strcasecmp(arg, "OFF") == 0) {
        on = false;
    }

    esp_err_t err = tuner_controller_set_power(on);
    if (err != ESP_OK) {
        usb_cdc_write_fmt("ERROR power: %s\r\n", esp_err_to_name(err));
        return;
    }

    usb_cdc_write_fmt("OK power=%s\r\n", on ? "ON" : "OFF");
}

static void cmd_ident(void)
{
    uint16_t dev = 0;
    uint16_t hw = 0;
    uint16_t sw = 0;

    if (tuner_controller_get_identification(&dev, &hw, &sw) == ESP_OK) {
        usb_cdc_write_fmt("IDENT device=%04X hw=%04X sw=%04X\r\n", dev, hw, sw);
    } else {
        usb_cdc_write_str("ERROR ident read failed\r\n");
    }
}

static void process_line(const char *line)
{
    while (*line && isspace((unsigned char)*line)) line++;
    if (!*line) return;

    char cmd[16] = {0};
    int i = 0;
    while (*line && !isspace((unsigned char)*line) && i < 15) {
        cmd[i++] = toupper((unsigned char)*line++);
    }
    cmd[i] = '\0';
    while (*line && isspace((unsigned char)*line)) line++;

    if (strcmp(cmd, "TUNEAM") == 0) cmd_tuneam(line);
    else if (strcmp(cmd, "SEEKAM") == 0) cmd_seekam(line);
    else if (strcmp(cmd, "SEEKSTOP") == 0) cmd_seekstop();
    else if (strcmp(cmd, "SEEK") == 0) cmd_seek(line);
    else if (strcmp(cmd, "TUNE") == 0) cmd_tune(line);
    else if (strcmp(cmd, "BAND") == 0) cmd_band(line);
    else if (strcmp(cmd, "STATUS") == 0) cmd_status();
    else if (strcmp(cmd, "QUALITY") == 0) cmd_quality();
    else if (strcmp(cmd, "STEREO") == 0) cmd_stereo();
    else if (strcmp(cmd, "RDSDEC") == 0) cmd_rdsdec();
    else if (strcmp(cmd, "RDS") == 0) cmd_rds();
    else if (strcmp(cmd, "VOLUME") == 0) cmd_volume(line);
    else if (strcmp(cmd, "MUTE") == 0) cmd_mute(line);
    else if (strcmp(cmd, "AUDIO") == 0) cmd_audio(line);
    else if (strcmp(cmd, "EVENTS") == 0) cmd_events(line);
    else if (strcmp(cmd, "POWER") == 0) cmd_power(line);
    else if (strcmp(cmd, "IDENT") == 0) cmd_ident();
    else if (strcmp(cmd, "HELP") == 0) cmd_help();
    else cmd_unknown();
}

static void cmd_task(void *arg)
{
    (void)arg;

    char line[256];
    vTaskDelay(pdMS_TO_TICKS(2000));

    usb_cdc_write_str("\r\n=== TEF6686 Headless Tuner v3 ===\r\n");
    usb_cdc_write_str("Type HELP for commands\r\n");

    while (1) {
        int len = usb_cdc_read_line(line, sizeof(line));
        if (len > 0) {
            process_line(line);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void cmd_handler_start(void)
{
    tuner_controller_register_cb(serial_status_cb, NULL);
    xTaskCreate(cmd_task, "cmd", CONFIG_USB_CDC_CMD_TASK_STACK, NULL, CONFIG_USB_CDC_CMD_TASK_PRIO, NULL);
}
