# Wi-Fi / Web 控制设计

## 目标

在现有 USB CDC + UAC 固件基础上，增加 Wi-Fi 和 Web 控制能力：
- 手机/电脑浏览器控制调谐器
- 兼容 XDR-GTK 桌面软件（TCP 协议）
- Wi-Fi 只做控制，音频仍走 USB UAC

## 新增模块

```
main/
  wifi_manager.*     Wi-Fi AP+STA 双模式管理、NVS 配网
  web_server.*       HTTP REST API + SSE 状态推送 + 嵌入式 Web 页面
  xdr_server.*       XDR-GTK TCP 协议服务 (端口 7373)
```

三个模块均调用现有 `tef6686.*` API，不重复实现调谐器逻辑。
`cmd_handler.*` 保持不变，继续服务 USB CDC。

## 模块职责

### wifi_manager

Wi-Fi 连接生命周期管理。

- **AP 模式**：上电默认开 AP，SSID `TEF6686-XXXX`（后四位取自 MAC），无密码或可配置密码
- **STA 模式**：通过配网页面输入路由器 SSID/密码，保存到 NVS，重启后自动连接
- **切换逻辑**：上电先尝试 STA（从 NVS 读配置），失败则回退 AP；AP 模式下配网成功后切到 STA
- **对外接口**：提供 IP 地址查询、连接状态回调、配网页面路由注册

配网页面注册在 web_server 上，路径 `/wifi`。

### web_server

HTTP 服务，基于 ESP-IDF `esp_http_server` 组件。

**REST API：**

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/status` | 调谐器完整状态（频点、band、stereo、RDS、信号） |
| POST | `/api/tune` | 调频 `{"freq": 87500, "band": "FM"}` |
| POST | `/api/seek` | 寻台 `{"direction": "UP", "band": "FM"}` |
| POST | `/api/seekstop` | 终止寻台 |
| POST | `/api/volume` | 设置音量 `{"volume": 20}` |
| POST | `/api/mute` | 静音 `{"mute": true}` |
| GET | `/api/quality` | 信号质量详情 |
| GET | `/api/rds` | 解码后 RDS 数据 |
| GET | `/api/events` | SSE 端点，推送实时状态变化 |

**SSE 事件格式：**

```
event: status
data: {"band":"FM","freq":87500,"tuned":true,"stereo":true,"rds":true,"level":42.5,"snr":35}

event: rds
data: {"pi":"C123","ps":"RADIO FM","rt":"Now playing...","pty":2,"tp":true,"ta":false}

event: quality
data: {"level":42.5,"snr":35,"usn":12,"wam":5,"bw":180,"mod":45}
```

推送频率：状态 ~1Hz，RDS 仅在数据变化时推送，信号质量 ~2Hz。

**Web 页面：**

嵌入式 HTML+JS+CSS，功能包括：
- 频率显示和调谐（数字键盘 + 上下步进）
- Seek 按钮（上/下）
- 信号强度条（level + SNR）
- RDS 信息显示（PI、PS、RadioText）
- 音量滑块、静音按钮
- Band 切换
- Wi-Fi 配网入口

页面通过 SSE 接收实时更新，通过 REST API 发送控制命令。
静态文件以 C 字符串数组形式嵌入固件（不依赖 SPIFFS），减少 flash 分区复杂度。

### xdr_server

XDR-GTK TCP 协议服务，兼容 XDR-GTK 桌面软件。

- 监听端口 7373，支持多客户端同时连接
- 文本协议，`\n` 结尾

**核心命令集：**

| 命令 | 格式 | 说明 |
|------|------|------|
| T | `T<freq>` | 调频（单位 10Hz，如 T087500000 = 87.5MHz） |
| M | `M<mode>` | 模式切换（0=FM, 1=AM） |
| Y | `Y<vol>` | 音量 0-100 |
| D | `D<val>` | 去加重（0=50us, 1=75us, 2=off） |
| A | `A<val>` | AGC（0=off, 1=on, 2=auto） |
| W | `W<hz>` | IF 带宽 |
| C | `C<dir>` | 寻台（0=down, 1=up） |
| x | `x` | 初始化握手 |

**状态推送 (~15Hz)：**

```
S<stereo><level>,<wam>,<usn>,<bw>\n\n
```

stereo 字段：`Sm`=mono, `Ss`=stereo, `SM`=stereo capable

**RDS 推送：**

```
R<bbbb><cccc><dddd><ee>\n
P<pi_hex>\n
```

**连接流程：**
1. 客户端连接
2. 服务端发送 16 字符随机 salt
3. 客户端回复 SHA1(salt+key) 40 字符 hex
4. 认证成功：`o1,0\n` + 完整状态同步
5. 认证失败：`a0\n`
6. Guest 模式（只读）：`a1\n`

密钥配置通过 Wi-Fi 配网页面或 CDC 命令设置，存储在 NVS。
默认密钥为空（不认证）。

## 状态广播架构

现有 `cmd_handler.c` 的 `event_task` 已周期性读取调谐器状态。
扩展为统一状态广播源，向所有通道推送：

```
event_task (100ms 轮询)
    ├── usb_cdc (现有 EVENTS 命令)
    ├── web_server SSE 客户端列表
    └── xdr_server TCP 客户端列表
```

具体实现：event_task 读取状态后，调用注册的回调函数列表。
web_server 和 xdr_server 各注册自己的回调。
回调在 event_task 上下文中执行，需快速返回（只拷贝数据到队列/缓冲区）。

## Wi-Fi 配网流程

1. 设备上电 → 尝试 STA 连接（NVS 中的配置）
2. STA 连接成功 → 启动 web_server + xdr_server
3. STA 连接超时（~5s） → 切换到 AP 模式
4. AP 模式 → 启动 web_server + 配网页面 + xdr_server
5. 用户通过浏览器连到 AP → 打开配网页面 → 输入 SSID/密码
6. 保存到 NVS → 尝试 STA 连接
7. STA 连接成功 → 关闭 AP，切换到 STA 模式

AP 模式下设备 IP 固定为 192.168.4.1。

## 新增依赖

- `esp_wifi` — Wi-Fi 驱动
- `esp_netif` — 网络接口
- `esp_http_server` — HTTP 服务
- `nvs_flash` — Wi-Fi 凭据存储
- `mbedtls` — SHA1 认证（XDR-GTK）

无需引入第三方库。

## 内存估算

- Wi-Fi 栈：~50KB
- HTTP server：~8KB + 连接缓冲区
- TCP server：~4KB/客户端，最多 4 客户端
- Web 页面 HTML+JS+CSS：~15KB（嵌入 flash，不占 RAM）
- 总新增 RAM：~80-100KB

ESP32-S3 有 512KB SRAM + 8MB PSRAM，资源充足。

## 与现有模块的交互

| 调用方 | 调用目标 | 说明 |
|--------|----------|------|
| web_server | tef6686.* | 调谐器控制 API |
| xdr_server | tef6686.* | 调谐器控制 API |
| web_server | wifi_manager | 配网页面路由 |
| cmd_handler event_task | web_server | SSE 状态广播 |
| cmd_handler event_task | xdr_server | TCP 状态广播 |

web_server 和 xdr_server 不互相调用。
两者独立工作，共享调谐器状态，互不依赖。

## 不做的事

- Wi-Fi 音频流：音频只走 USB UAC
- SPIFFS 文件系统：Web 页面以 C 数组嵌入，不新增 flash 分区
- mDNS：简化初始实现，用 IP 直连
- HTTPS：嵌入式设备不需要 TLS 开销
- WebSocket：SSE 足够，实现更简单
- OTA 升级：不在本次范围
- RDSSPY 协议：只做 XDR-GTK
