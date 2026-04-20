---
quick_id: 260420-syi
title: Fix /wifi header fields are too long
date: 2026-04-20
status: completed
---

# Quick Task

## Problem

The `/wifi` page could fail with `Header fields are too long` on the ESP32 HTTP server.

## Root Cause

The project defaulted `CONFIG_HTTPD_MAX_REQ_HDR_LEN` to `1024` in `sdkconfig.defaults`, but the active tracked `sdkconfig` still used `512`.

## Changes

- Raised the active HTTP request header limit in `sdkconfig` from `512` to `1024`
- Added startup logging in `main/web_server.c` to print the active HTTP request header and URI limits

## Verification

- `idf.py build` passed
- `idf.py -p /dev/cu.usbmodem5C4C2143521 flash` reached esptool but failed because the board was not in download mode (`Wrong boot mode detected (0x8)`)
