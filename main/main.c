#include "tef6686.h"
#include "usb_cdc.h"
#include "usb_descriptors.h"
#include "audio.h"
#include "cmd_handler.h"
#include "tuner_controller.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "xdr_server.h"
#include "version.h"
#include "tinyusb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static const char *wifi_mode_name(wifi_mgr_mode_t mode)
{
    switch (mode) {
    case WIFI_MGR_MODE_AP:
        return "AP";
    case WIFI_MGR_MODE_STA:
        return "STA";
    case WIFI_MGR_MODE_APSTA:
        return "APSTA";
    default:
        return "UNKNOWN";
    }
}

static tusb_desc_device_t const s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USBD_VID,
    .idProduct          = USBD_PID,
    .bcdDevice          = USBD_VER,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

static char const *s_string_desc[] = {
    (const char[]){ 0x09, 0x04 },  // 0: English
    "wwek",                         // 1: Manufacturer
    "TEF6686 Headless Tuner",       // 2: Product
    "12345678",                      // 3: Serial
    "TEF6686 CDC",                   // 4: CDC interface
    "TEF6686 Audio",                 // 5: Audio interface
};

void app_main(void)
{
    ESP_LOGI(TAG, "TEF6686 Headless Tuner v" FIRMWARE_VERSION " (" FIRMWARE_COMMIT "@" FIRMWARE_BRANCH " " FIRMWARE_BUILD_DATE ")");

    // Initialize TEF6686 via I2C
    esp_err_t err;
    tef_config_t tef_cfg = {
        .i2c_port = CONFIG_TEF_I2C_PORT,
        .sda_pin = CONFIG_TEF_I2C_SDA_PIN,
        .scl_pin = CONFIG_TEF_I2C_SCL_PIN,
        .i2c_freq_hz = CONFIG_TEF_I2C_FREQ_HZ,
        .chip_version = CONFIG_TEF_CHIP_VERSION,
        .xtal_type = CONFIG_TEF_XTAL_TYPE,
    };
    err = tef6686_init(&tef_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TEF6686 init failed: %s", esp_err_to_name(err));
    } else {
        tef6686_set_volume(CONFIG_TUNER_DEFAULT_VOLUME);
        tef6686_set_rds(true);
        tef6686_set_deemphasis(50);
#if CONFIG_TUNER_DEFAULT_BAND_FM
        tef6686_set_band(TEF_BAND_FM);
        tef6686_tune_fm(87500);
#else
        tef6686_set_band(TEF_BAND_MW);
        tef6686_tune_am(1008);
#endif
    }

    // Initialize USB composite device (CDC + UAC)
    uint16_t config_desc_len = 0;
    const uint8_t *config_desc = usb_composite_get_config_desc(&config_desc_len);

    tinyusb_config_t tusb_cfg = {
        .device_descriptor = &s_device_desc,
        .string_descriptor = s_string_desc,
        .external_phy = false,
        .configuration_descriptor = config_desc,
    };
    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(err);
    }

    // Initialize CDC ACM interface
    err = usb_cdc_interface_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CDC interface init failed: %s", esp_err_to_name(err));
    }

    // Initialize audio pipeline (I2S from TEF6686 -> USB UAC)
    audio_config_t audio_cfg = {
        .i2s_port = CONFIG_AUDIO_I2S_PORT,
        .bclk_pin = CONFIG_AUDIO_BCLK_PIN,
        .ws_pin = CONFIG_AUDIO_WS_PIN,
        .data_pin = CONFIG_AUDIO_DATA_PIN,
        .sample_rate = CONFIG_AUDIO_SAMPLE_RATE,
        .bits_per_sample = CONFIG_AUDIO_BITS_PER_SAMPLE,
    };
    err = audio_init(&audio_cfg);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "USB audio class is not enabled in this build; running CDC-only");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio init failed: %s", esp_err_to_name(err));
    } else {
        audio_start();
    }

    // Start controller, then protocol adapters
    err = tuner_controller_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Tuner controller start failed: %s", esp_err_to_name(err));
    }

    cmd_handler_start();

    // Initialize Wi-Fi (non-fatal if it fails)
    err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
    } else {
        char ap_ssid[32] = {0};

        wifi_manager_get_ap_ssid(ap_ssid, sizeof(ap_ssid));
        ESP_LOGI(TAG,
                 "Startup config: AP SSID=%s, Wi-Fi mode=%s, I2C SDA=%d, SCL=%d",
                 ap_ssid[0] ? ap_ssid : "<unset>",
                 wifi_mode_name(wifi_manager_get_mode()),
                 CONFIG_TEF_I2C_SDA_PIN,
                 CONFIG_TEF_I2C_SCL_PIN);
    }

    // Start web server and XDR-GTK TCP server
    err = web_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web server start failed: %s", esp_err_to_name(err));
    }

    err = xdr_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "XDR server start failed: %s", esp_err_to_name(err));
    }

#if CFG_TUD_AUDIO
    ESP_LOGI(TAG, "System ready - CDC + UAC + WiFi");
#else
    ESP_LOGI(TAG, "System ready - CDC + WiFi");
#endif
}
