# TEF6686 Headless Tuner

[![CI](https://github.com/wwek/tef-headless-tuner/actions/workflows/ci.yml/badge.svg)](https://github.com/wwek/tef-headless-tuner/actions/workflows/ci.yml)
[![Release](https://github.com/wwek/tef-headless-tuner/actions/workflows/release.yml/badge.svg)](https://github.com/wwek/tef-headless-tuner/actions/workflows/release.yml)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.3.1-ff6a00)
![Target](https://img.shields.io/badge/Target-ESP32--S3-00979d)
[![License](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

[English](README.md)

这是一个面向 `ESP32-S3` 的 `TEF6686` headless 调谐器固件项目，支持多种控制方式：

- **USB CDC ACM** — 串口控制、状态查询和命令通道
- **USB Audio Class (UAC)** — 音频输入设备
- **Wi-Fi Web UI** — 浏览器控制页面，REST API + SSE 实时推送
- **XDR-GTK TCP** — 兼容 XDR-GTK 桌面调谐器软件
- **I2C** — `TEF6686` 调谐器控制

Wi-Fi 支持 AP 模式（直连）和 STA 模式（连接现有网络），自动回退。

## 当前状态

项目仍处于持续开发阶段。

当前仓库范围包括：

- 面向 `ESP32-S3` 的 `ESP-IDF` 固件
- `TEF6686` 初始化、补丁加载和基础调谐控制
- `CDC + UAC` 复合 USB 设备描述符
- 用于调谐、搜台、状态、RDS、静音、音量、电源控制的命令处理器
- Wi-Fi AP/STA 双模式管理，支持 NVS 凭据持久化
- HTTP REST API + SSE 实时状态推送
- 内嵌 Web 控制页面（暗色主题，响应式布局）
- XDR-GTK TCP 协议服务（端口 7373，SHA1 挑战认证）
- 一个通过 USB CDC 交互的 Python 主机侧 CLI

## 目录结构

- `main/`
  固件源码
- `docs/`
  设计说明和参考指引
- `host/`
  主机侧工具
- `.local/`
  本地克隆的参考项目目录，已被 Git 忽略

## 主要模块

- `components/tef6686/`
  `TEF6686` 驱动组件，负责补丁加载、时钟初始化和调谐器接口
- `components/tuner_frontend/`
  收音前端服务层，负责音频输入、RDS 解码、搜台逻辑和统一调谐控制
- `main/usb_cdc.*`
  USB CDC 传输层
- `main/usb_descriptors.*`
  USB 复合设备描述符
- `main/cmd_handler.*`
  USB CDC 命令解析
- `main/wifi_manager.*`
  Wi-Fi AP/STA 模式管理、NVS 凭据存储
- `main/web_server.*`
  HTTP REST API、SSE 推送、内嵌 Web 控制页
- `main/html/`
  内嵌 HTML 页面（index.html、wifi.html）
- `main/xdr_server.*`
  XDR-GTK TCP 协议服务

## 环境要求

- `ESP-IDF 5.x` 或兼容的 `idf.py` 环境
- 支持原生 USB Device 的 `ESP32-S3` 开发板
- 通过 I2C 连接的 `TEF6686` 或兼容 `TEF668X` 模块
- 如果启用 USB 音频，需要与当前 I2S 配置一致的音频连接

## 配置项

项目配置位于 `menuconfig` 中的以下菜单：

- `TEF6686 Configuration`
- `USB CDC Configuration`
- `Audio Configuration`
- `Tuner Defaults`
- `WiFi Manager Configuration`

关键配置包括：

- I2C 引脚和总线频率
- TEF 芯片补丁版本：`102` 或 `205`
- 晶振类型：`4 MHz`、`9.216 MHz`、`12 MHz`、`55 MHz`
- I2S 引脚和采样格式
- 默认启动波段和音量
- Wi-Fi AP SSID 前缀、信道、STA 连接超时

## 接线图

默认 GPIO 分配（可通过 `menuconfig` 修改）：

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
    |              4.7k 上拉至 3V3 (两根)     |
    |  GPIO  4 (BCLK) ------- I2S_BCLK (13) |
    |  GPIO  5 (WS)   ------- I2S_WS   (12) |
    |  GPIO  6 (DATA) ------- I2S_SDO  (11) |
    |  GPIO 19 (D-)  ---+                    |
    |  GPIO 20 (D+)  ---+ USB-C             |
    +----------------------------------------+
                          |
                     ANT --- 天线口
```

### 引脚对照

| 功能 | ESP32-S3 GPIO | TEF6686 引脚 | 说明 |
|------|--------------|-------------|------|
| I2C SDA | 8 | 24 (SDI) | 4.7k 上拉至 3.3V |
| I2C SCL | 9 | 23 (SCL) | 4.7k 上拉至 3.3V |
| I2S BCLK | 4 | 13 (I2S_BCLK) | |
| I2S WS | 5 | 12 (I2S_WS) | |
| I2S Data | 6 | 11 (I2S_SDO_0) | 音频数据输出到 ESP32-S3 |
| USB D- | 19 | — | ESP32-S3 原生 USB |
| USB D+ | 20 | — | ESP32-S3 原生 USB |

### 外围电路

- **I2C 上拉电阻**：SDA 和 SCL 各 4.7kΩ 上拉至 3V3（如果模块已带则不需要）

I2C 总线只需要一组上拉电阻，不是每个设备各加一组。TEF6686 模块和 ESP32-S3 模组可能自带也可能不带。判断方法：万用表打到电阻档，分别量模块上 SDA/SCL 与 VCC 之间的电阻，如果任一侧量到约 4.7kΩ，说明已有上拉，无需额外添加。

无需外接 LDO — ESP32-S3 DevKit 板载 3V3 输出可直接给 TEF6686 模块供电。TEF6686 铁壳模块已内置晶振和周围阻容。

## 构建

在加载好 ESP-IDF 环境后执行：

```bash
idf.py set-target esp32s3
idf.py build
```

烧录和串口监视：

```bash
idf.py flash
idf.py monitor
```

## GitHub 自动化

仓库已经内置 GitHub Actions 工作流，位于 `.github/workflows/`：

- `ci.yml`
  在每次 `push` 和 `pull_request` 时自动构建 `ESP32-S3` 固件，并上传构建产物。
- `release.yml`
  当推送符合 `v*` 的版本 tag 时自动构建、打包，并发布 GitHub Release 资产。

CI / Release 产物包括：

- `tef-headless-tuner.bin`
- `bootloader.bin`
- `partition-table.bin`
- `flash_args`
- `flasher_args.json`
- `SHA256SUMS.txt`

推荐发布流程：

1. 先在分支上提交代码，由 `ci.yml` 自动验证构建。
2. 创建并推送版本 tag，例如 `v0.1.0`。
3. 由 `release.yml` 自动构建固件并发布 GitHub Release。

## USB 接口

固件目标是枚举为一个复合 USB 设备，包含：

- `CDC ACM`
  控制、状态和命令通道
- `UAC`
  主机侧音频输入设备

默认 USB 标识符定义在 [main/usb_descriptors.h](main/usb_descriptors.h)。

## CDC 命令

当前命令处理器支持的命令包括：

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

当前帮助文本位于 [main/cmd_handler.c](main/cmd_handler.c)。

## Wi-Fi 与 Web 控制

固件默认启动 Wi-Fi 热点（`TEF6686-XXXX`，IP `192.168.4.1`）。在浏览器中打开该 IP 即可进入控制页面。也可以通过 `/wifi` 配网页面将设备接入现有网络。

### REST API

| 端点 | 方法 | 说明 |
|------|------|------|
| `/` | GET | Web 控制页面 |
| `/wifi` | GET | Wi-Fi 配网页面 |
| `/api/status` | GET | 调谐器状态（波段、频率、信号等） |
| `/api/quality` | GET | 信号质量指标 |
| `/api/rds` | GET | RDS 解码数据 |
| `/api/events` | GET | SSE 实时推送（状态、质量、RDS） |
| `/api/tune` | POST | 调谐（`{"freq":95000,"band":"FM"}`） |
| `/api/seek` | POST | 搜台（`{"direction":"UP","band":"FM"}`） |
| `/api/seekstop` | POST | 停止搜台 |
| `/api/volume` | POST | 设置音量 0-30 |
| `/api/mute` | POST | 静音/取消静音 |
| `/api/wifi` | POST | 设置 STA 凭据 |

### SSE 事件

连接 `/api/events` 获取实时更新：
- `event: status` — 波段、频率、信号、搜台状态
- `event: quality` — 电平、USN、WAM、带宽、调制度、SNR
- `event: rds` — PI、PS、RT、PTY、TP/TA/MS 标志

## XDR-GTK 服务

TCP 端口 7373，提供核心 XDR-GTK 协议兼容。支持 SHA1 挑战认证，以及 `A`、`B`、`C`、`D`、`F`、`G`、`M`、`T`、`W`、`X`、`Y`、`Z`、`x` 等主要控制命令；状态约 15 Hz 推送，并包含原始 RDS 帧和搜台结束反馈。

## 主机侧 CLI

仓库提供了一个简单的 Python 主机侧控制脚本：

- [host/tef_control.py](host/tef_control.py)

安装依赖：

```bash
pip install pyserial
```

运行示例：

```bash
python host/tef_control.py /dev/ttyACM0
```

Windows 示例：

```bash
python host/tef_control.py COM3
```

## 设计文档

项目设计说明和关键参考指引见：

- [docs/项目设计.md](docs/项目设计.md)

该文档也说明了 `.local/` 中参考项目的阅读顺序和借鉴范围。

## 说明

- 本仓库优先关注 headless 使用路径，而不是本地 UI。
- 参考项目主要用于借鉴 TEF 补丁数据、调谐器行为和 headless 产品形态。
- 当社区实现与 Espressif 官方 USB 文档存在差异时，应以官方文档和 `ESP-IDF` 约束为准。
