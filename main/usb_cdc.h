#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

// Init CDC ACM interface (call after tinyusb_driver_install)
esp_err_t usb_cdc_interface_init(void);
esp_err_t usb_cdc_write(const char *data, size_t len);
esp_err_t usb_cdc_write_str(const char *str);
esp_err_t usb_cdc_write_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int usb_cdc_read_line(char *buf, int buf_size);
bool usb_cdc_connected(void);
