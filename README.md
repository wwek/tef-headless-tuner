# TEF6686 Headless Tuner

[![CI](https://github.com/wwek/tef-headless-tuner/actions/workflows/ci.yml/badge.svg)](https://github.com/wwek/tef-headless-tuner/actions/workflows/ci.yml)
[![Release](https://github.com/wwek/tef-headless-tuner/actions/workflows/release.yml/badge.svg)](https://github.com/wwek/tef-headless-tuner/actions/workflows/release.yml)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.3.1-ff6a00)
![Target](https://img.shields.io/badge/Target-ESP32--S3-00979d)
[![License](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

[中文说明](README.zh-CN.md)

ESP32-S3 firmware for a headless TEF6686-based tuner with multiple control interfaces:

- **USB CDC ACM** — serial control, status, and command channel
- **USB Audio Class (UAC)** — audio input streaming to host
- **Wi-Fi Web UI** — browser-based control with REST API and real-time SSE updates
- **XDR-GTK TCP** — compatible with XDR-GTK desktop tuner software
- **I2C** — TEF6686 tuner control

Wi-Fi supports AP mode (direct connection) and STA mode (join existing network), with automatic fallback.

## Status

This project is still in active development.

Current repository scope:

- `ESP-IDF` firmware for `ESP32-S3`
- TEF6686 initialization, patch loading, and tuner control
- USB composite device descriptors for `CDC + UAC`
- Command handler for tune, seek, status, RDS, mute, volume, and power control
- Wi-Fi AP/STA with automatic mode switching and NVS credential persistence
- HTTP REST API + SSE real-time status streaming
- Embedded web control page (dark theme, responsive)
- XDR-GTK TCP protocol server (port 7373, SHA1 challenge auth)
- A small host-side Python CLI for USB CDC interaction

## Repository Layout

- `main/`
  Firmware sources
- `docs/`
  Design notes and reference guidance
- `host/`
  Host-side tools
- `.local/`
  Locally cloned reference projects, ignored by Git

## Main Components

- `components/tef6686/`
  TEF6686 driver component, patch loading, clock setup, and tuner-facing API
- `components/tuner_frontend/`
  Frontend service layer for audio input, RDS decoding, seek logic, and unified tuner control
- `main/usb_cdc.*`
  USB CDC transport
- `main/usb_descriptors.*`
  USB composite descriptor definitions
- `main/cmd_handler.*`
  USB CDC command parsing
- `main/wifi_manager.*`
  Wi-Fi AP/STA mode management, NVS credential storage
- `main/web_server.*`
  HTTP REST API, SSE streaming, embedded web UI
- `main/html/`
  Embedded HTML pages (index.html, wifi.html)
- `main/xdr_server.*`
  XDR-GTK TCP protocol server

## Requirements

- ESP-IDF 5.x or a compatible environment with `idf.py`
- An `ESP32-S3` board with native USB device support
- A TEF6686 or compatible TEF668X tuner module wired over I2C
- Audio wiring that matches the configured I2S input path if USB audio is enabled

## Configuration

Project options are defined in `menuconfig` under:

- `TEF6686 Configuration`
- `USB CDC Configuration`
- `Audio Configuration`
- `Tuner Defaults`
- `WiFi Manager Configuration`

Key settings include:

- I2C pins and bus frequency
- TEF chip patch version: `102` or `205`
- Crystal type: `4 MHz`, `9.216 MHz`, `12 MHz`, or `55 MHz`
- I2S pins and sample format
- Default startup band and volume
- Wi-Fi AP SSID prefix, channel, and STA connect timeout

## Wiring

Default GPIO assignments (configurable via `menuconfig`):

```
                       USB-C
                         |
    +--------------------+--------------------+
    |              ESP32-S3 DevKit           |
    |                                        |
    |  3V3  ------------------- VCC   模块   |
    |  GND  ------------------- GND  TEF6686 |
    |  GPIO  8 (SDA)  ------- SDI      (24) |
    |  GPIO  9 (SCL)  ------- SCL      (23) |
    |              4.7k pull-up to 3V3       |
    |  GPIO  4 (BCLK) ------- I2S_BCLK (13) |
    |  GPIO  5 (WS)   ------- I2S_WS   (12) |
    |  GPIO  6 (DATA) ------- I2S_SDO  (11) |
    |  GPIO 19 (D-)  ---+                    |
    |  GPIO 20 (D+)  ---+ USB-C             |
    +----------------------------------------+
                          |
                     ANT --- 天线口
```

### Pin Reference

| Function | ESP32-S3 GPIO | TEF6686 Pin | Notes |
|----------|--------------|-------------|-------|
| I2C SDA | 8 | 24 (SDI) | 4.7k pull-up to 3.3V |
| I2C SCL | 9 | 23 (SCL) | 4.7k pull-up to 3.3V |
| I2S BCLK | 4 | 13 (I2S_BCLK) | |
| I2S WS | 5 | 12 (I2S_WS) | |
| I2S Data | 6 | 11 (I2S_SDO_0) | Audio data to ESP32-S3 |
| USB D- | 19 | — | Native USB |
| USB D+ | 20 | — | Native USB |

### External Components

- **I2C pull-up resistors**: 4.7kΩ on both SDA and SCL to 3V3 (only if not already present)

I2C bus needs pull-up resistors, but only one set total — not one per device. Most TEF6686 modules and ESP32-S3 DevKits may or may not have them onboard. Check with a multimeter in resistance mode: measure between SDA/SCL and VCC on each module. If either side reads ~4.7kΩ, the pull-ups are already present and no external resistors are needed.

No LDO needed — the ESP32-S3 DevKit onboard 3V3 output powers the TEF6686 module directly. TEF6686 metal-shielded modules include the crystal and surrounding passives onboard.

## Build

After sourcing the ESP-IDF environment:

```bash
idf.py set-target esp32s3
idf.py build
```

Flash and monitor:

```bash
idf.py flash
idf.py monitor
```

## GitHub Automation

This repository includes GitHub Actions workflows under `.github/workflows/`:

- `ci.yml`
  Builds the ESP32-S3 firmware on every push and pull request, then uploads build artifacts.
- `release.yml`
  Builds release firmware on tags matching `v*`, packages binaries, and publishes GitHub Release assets.

Build artifacts produced by CI include:

- `tef-headless-tuner.bin`
- `bootloader.bin`
- `partition-table.bin`
- `flash_args`
- `flasher_args.json`
- `SHA256SUMS.txt`

Recommended release flow:

1. Push changes to a branch and let `ci.yml` validate the build.
2. Create and push a version tag such as `v0.1.0`.
3. Let `release.yml` generate the release binaries and publish the GitHub Release automatically.

## USB Interfaces

The firmware is intended to enumerate as a composite USB device with:

- `CDC ACM`
  Control, status, and command channel
- `UAC`
  Audio input device on the host

The default USB identifiers are defined in [main/usb_descriptors.h](main/usb_descriptors.h).

## CDC Commands

The command handler currently exposes commands such as:

- `HELP`
- `STATUS`
- `QUALITY`
- `STEREO`
- `RDS`
- `RDSDEC`
- `TUNE <freq_khz>`
- `TUNEAM <freq_khz>`
- `SEEK UP|DOWN`
- `SEEKAM UP|DOWN`
- `SEEKSTOP`
- `BAND FM|LW|MW|SW`
- `VOLUME <0-30>`
- `MUTE ON|OFF`
- `AUDIO ON|OFF`
- `EVENTS ON|OFF`
- `POWER ON|OFF`
- `IDENT`

The current help text lives in [main/cmd_handler.c](main/cmd_handler.c).

## Wi-Fi & Web Control

The firmware starts a Wi-Fi access point by default (`TEF6686-XXXX`, IP `192.168.4.1`). Open the IP in a browser for the control page. You can also join an existing network via the `/wifi` provisioning page.

### REST API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web control page |
| `/wifi` | GET | Wi-Fi provisioning page |
| `/api/status` | GET | Tuner status (band, freq, level, etc.) |
| `/api/quality` | GET | Signal quality metrics |
| `/api/rds` | GET | RDS decoded data |
| `/api/events` | GET | SSE stream (real-time status, quality, RDS) |
| `/api/tune` | POST | Tune to frequency (`{"freq":95000,"band":"FM"}`) |
| `/api/seek` | POST | Start seek (`{"direction":"UP","band":"FM"}`) |
| `/api/seekstop` | POST | Abort seek |
| `/api/volume` | POST | Set volume 0-30 |
| `/api/mute` | POST | Mute/unmute |
| `/api/wifi` | POST | Set STA credentials |

### SSE Events

Connect to `/api/events` for real-time updates:
- `event: status` — band, frequency, signal, seek state
- `event: quality` — level, USN, WAM, bandwidth, modulation, SNR
- `event: rds` — PI, PS, RT, PTY, TP/TA/MS flags

## XDR-GTK Server

TCP server on port 7373 with core XDR-GTK protocol compatibility. Supports SHA1 challenge authentication plus the main control commands `A`, `B`, `C`, `D`, `F`, `G`, `M`, `T`, `W`, `X`, `Y`, `Z`, and `x`. Status is pushed at ~15 Hz, including raw RDS frames and seek completion feedback.

## Host CLI

A small Python CLI is available at [host/tef_control.py](host/tef_control.py).

Install dependency:

```bash
pip install pyserial
```

Run it:

```bash
python host/tef_control.py /dev/ttyACM0
```

On Windows:

```bash
python host/tef_control.py COM3
```

## Design Notes

Project design notes and implementation guidance are collected in:

- [docs/项目设计.md](docs/项目设计.md)

That document also points to the local reference projects under `.local/`.

## Notes

- This repository prioritizes a headless user path over UI features.
- Reference projects were used for TEF patch data, tuner behavior, and overall headless product direction.
- USB and ESP32-S3 device behavior should be validated against official Espressif documentation when implementation details differ from community projects.
