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
        app_update
        tef6686
        tuner_frontend
        nvs_flash
        mbedtls
        json
        lwip
    EMBED_TXTFILES html/index.html html/wifi.html html/system.html
)

# --- Build version from git ---
set(FIRMWARE_VERSION "0.1.0")

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${PROJECT_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_SHORT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE git_ok
)
if(NOT git_ok EQUAL 0)
    set(GIT_COMMIT_SHORT "unknown")
endif()

execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${PROJECT_DIR}
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE git_ok
)
if(NOT git_ok EQUAL 0)
    set(GIT_BRANCH "unknown")
endif()

string(TIMESTAMP BUILD_DATE "%Y-%m-%d")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/version.h
    @ONLY
)

target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(tinyusb_name tinyusb)
else()
    set(tinyusb_name espressif__tinyusb)
endif()

idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
target_include_directories(${tusb_lib} BEFORE PRIVATE "${PROJECT_DIR}/components/tuner_frontend")
