idf_component_register(
    SRCS
        "main.c"
        "usb_cdc.c"
        "usb_descriptors.c"
        "cmd_handler.c"
        "wifi_manager.c"
        "web_server.c"
        "xdr_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_tinyusb
        esp_wifi
        esp_netif
        esp_http_server
        tef6686
        tuner_frontend
        nvs_flash
        mbedtls
        json
        lwip
    EMBED_TXTFILES html/index.html html/wifi.html
)

idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(tinyusb_name tinyusb)
else()
    set(tinyusb_name espressif__tinyusb)
endif()

idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
target_include_directories(${tusb_lib} BEFORE PRIVATE "${PROJECT_DIR}/components/tuner_frontend")
