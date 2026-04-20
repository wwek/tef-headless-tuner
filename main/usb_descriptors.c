#include "usb_descriptors.h"
#include "tusb.h"

#define USB_CONFIG_LEN_CDC (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#define USB_CONFIG_LEN_UAC (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_AUDIO_MIC_TWO_CH_DESC_LEN)

#if !CFG_TUD_AUDIO
static const uint8_t desc_configuration_cdc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, USB_CONFIG_LEN_CDC, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, CDC_NOTIF_EP, 8, CDC_OUT_EP, CDC_IN_EP, 64),
};
#endif

#if CFG_TUD_AUDIO
static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, USB_CONFIG_LEN_UAC, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, CDC_NOTIF_EP, 8, CDC_OUT_EP, CDC_IN_EP, 64),
    TUD_AUDIO_MIC_TWO_CH_DESCRIPTOR(
        ITF_NUM_AUDIO_CTL,
        5,
        CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX,
        CFG_TUD_AUDIO_FUNC_1_RESOLUTION_TX,
        AUDIO_IN_EP,
        TUNER_AUDIO_EP_SIZE_BYTES
    ),
};
#endif

const uint8_t *usb_composite_get_config_desc(uint16_t *len)
{
#if CFG_TUD_AUDIO
    *len = sizeof(desc_configuration);
    return desc_configuration;
#else
    *len = sizeof(desc_configuration_cdc);
    return desc_configuration_cdc;
#endif
}

uint16_t usb_composite_get_config_desc_len(void)
{
#if CFG_TUD_AUDIO
    return sizeof(desc_configuration);
#else
    return sizeof(desc_configuration_cdc);
#endif
}
