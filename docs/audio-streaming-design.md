# Audio Streaming Design: I2S to USB + WebSocket Concurrent Output

## 1. System Topology

```
TEF6686 (I2S Master, 48kHz/16bit/stereo)
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  ESP32-S3 (N16R8: 16MB Flash, 8MB PSRAM)            │
│                                                      │
│  Task_I2S (prio 6)                                   │
│    I2S RX (slave) ──► Ring Buffer (32KB, SRAM)       │
│       │                     │                        │
│       │              feed_usb_audio()                 │
│       │                     ▼                        │
│       │              USB UAC2 ──► Host PC             │
│       │                                              │
│       │              consumer callback                │
│       │                     ▼                        │
│       │              WS Ring Buffer (32KB, PSRAM)     │
│       │                     │                        │
│       │              Task_WS (prio 4, independent)    │
│       │                     ▼                        │
│       │              httpd_ws_send_frame_async()      │
│       │                     ▼                        │
│       │              Browser (Web Audio API)          │
└─────────────────────────────────────────────────────┘
```

## 2. Hardware Facts

| Parameter | Value | Notes |
|-----------|-------|-------|
| I2S role | **Slave** | TEF6686 is currently configured as I2S clock master |
| Sample rate | **48000 Hz** | Explicitly configured on both TEF6686 and USB UAC |
| Bit depth | 16-bit | Configurable (also supports 24-bit) |
| Channels | Stereo (2) | Standard I2S Philips format |
| Bitrate | 48000 x 2 x 16 = **1.54 Mbps** (192 KB/s) | |
| CPU | Dual-core Xtensa LX7, 240 MHz | Currently 160 MHz, can increase |
| PSRAM | 8MB (Octal, 80 MHz) | Used for WS ring buffer |
| Flash | 16MB | Dual OTA partitions (4MB each) |
| USB | Full-Speed (12 Mbps) | TinyUSB UAC2 |

## 3. Task Architecture

### 3.1 Task Allocation

| Task | Priority | Stack | Role |
|------|----------|-------|------|
| audio_task | 6 | 4096 | I2S read → USB ring buffer → consumer dispatch (pinned to CPU1 by default) |
| tusb_task | (TinyUSB internal) | 4096 | USB UAC2 endpoint (runs on CPU1) |
| ws_audio | 4 | 6144 | Read WS ring buffer → send via WebSocket |
| sse_worker | 5 | 4096 | SSE status push to web clients |
| httpd | (system) | 8192 | HTTP request handling |
| wifi_transition | 2 | 4096 | WiFi mode switching |

### 3.2 Design Principle: Independent Tasks, Zero Cross-Blocking

The core design principle is that **no task blocks another task's critical path**:

1. `audio_task` runs at the highest application priority (6). It reads I2S, writes to the USB ring buffer, and dispatches data to registered consumers — all non-blocking.
2. The WS audio consumer callback only does a ring buffer write (0 timeout). If the buffer is full, oldest data is dropped. This never blocks the audio task.
3. `ws_audio_sender_fn` is a completely independent task. It polls the WS ring buffer every 10ms and sends data via `httpd_ws_send_frame_async()`. This function is thread-safe and does not block the httpd worker threads.
4. httpd worker threads are never involved in audio streaming. The WS endpoint handler only handles handshake and close frames — returns immediately.

### 3.3 Why Not Reference Counting

A zero-copy reference counting approach was considered but rejected for this project:

- With only 2 consumers (USB + WS), the memcpy overhead is 192 bytes/frame at 48kHz — negligible on a 240 MHz CPU.
- Per-frame malloc/free introduces heap fragmentation risk in a long-running embedded system.
- The ring buffer approach has predictable memory usage and simple overflow behavior (drop oldest).
- Reference counting adds atomic operation overhead and complex free-timing bugs that are hard to debug.

### 3.4 Clock Domain Constraint

This project has two independent timing sources:

1. `TEF6686` drives `BCLK/WS` as the `I2S master`
2. The `USB host` schedules isochronous transfers from its own `SOF` clock

Even when both sides are nominally configured for the same sample rate, they are not guaranteed to produce and consume samples at exactly the same long-term average rate. A small ppm error is enough to slowly drain or fill the USB ring buffer.

This is the main reason some community USB digital tuner builds are described as sounding worse than the analog path. The failure mode is usually not "digital audio is bad" and not "the MCU brand is bad". The failure mode is that the bridge between the tuner clock domain and the USB host clock domain is implemented with a coarse buffer hack instead of real synchronization.

Design requirements for this repository:

- The USB ring buffer absorbs short-term jitter and scheduling variance. It must not be the primary long-term clock compensation mechanism.
- Normal steady-state playback must not depend on periodic PCM chunk drops or silence insertion.
- The advertised UAC sample rate must match the actual tuner output configuration for the selected audio path.
- If the tuner clock cannot be locked closely enough to the USB side, the design must add an explicit synchronization strategy such as UAC feedback or a controlled resampling stage.

Anti-patterns to avoid:

- Dropping a PCM block whenever the buffer gets too full
- Inserting silence whenever the buffer gets too empty
- Treating rare buffer recovery behavior as acceptable steady-state behavior
- Assuming that matching nominal sample-rate numbers alone solves drift

## 4. Memory Layout

```
Internal SRAM (~300KB available)
├── USB ring buffer: 32KB (FreeRTOS ringbuf, RINGBUF_TYPE_BYTEBUF)
├── Task stacks: ~50KB total
├── HTTP/TCP buffers: ~30KB
└── System overhead: remaining

PSRAM (8MB, enabled via CONFIG_SPIRAM=y, Octal 80MHz)
├── WS audio ring buffer: 32KB (RINGBUF_TYPE_BYTEBUF)
├── Available for future use: ~7.9MB
└── (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384)
    Allocations > 16KB automatically go to PSRAM
```

### 4.1 Ring Buffer Flow Control

Both ring buffers use the same overflow strategy:

```
write(data, timeout=0):
  if ring_buffer_full:
    // Drop oldest data to make room
    discard = receive_up_to(data_length)
    return_item(discard)
    write(data, timeout=0)  // guaranteed to succeed
```

This ensures the I2S reader never blocks, and data loss is graceful (oldest dropped first).

## 5. WebSocket Audio Protocol

### 5.1 Endpoint

- URI: `/api/audio`
- Protocol: WebSocket (binary frames)
- Max concurrent clients: 2

### 5.2 Data Format

Raw PCM binary frames, same format as I2S output:
- 48000 Hz sample rate
- 16-bit signed little-endian
- Stereo, interleaved (L R L R ...)
- No header or framing — each WebSocket binary message is a contiguous PCM chunk

### 5.3 Frame Size

The sender task polls every 10ms. At 176 KB/s, average frame size is ~1.7 KB. The sender reads up to 4 KB per iteration from the WS ring buffer.

### 5.4 Client Lifecycle

1. Browser connects via WebSocket handshake (`GET /api/audio` with `Upgrade: websocket`)
2. Server assigns a client slot, resets ring buffer to avoid stale data
3. `ws_audio_sender_fn` begins sending PCM data to all active clients
4. On disconnect (close frame, TCP reset, or send failure), the client slot is released
5. `httpd` session close callback (`config.close_fn`) provides reliable cleanup even for abrupt disconnects

## 6. WiFi Configuration for Real-Time Audio

### 6.1 Power Save Must Be Disabled

WiFi power save mode (Minimum Retention, `WIFI_PS_MIN_MODEM`) causes the radio to sleep periodically, introducing **20-70ms latency spikes**. For real-time audio streaming:

```c
esp_wifi_set_ps(WIFI_PS_NONE);
```

This must be called after `esp_wifi_start()`. Power consumption increases by ~30mA but latency becomes predictable.

### 6.2 TCP/LWIP Tuning

| Parameter | Current Value | Notes |
|-----------|--------------|-------|
| TCP send buffer | 5760 bytes | Default, sufficient for ~32ms of audio |
| TCP write timeout | Default | `httpd_ws_send_frame_async` handles internally |

## 7. Browser-Side Playback

### 7.1 Architecture

```
WebSocket onmessage (binary ArrayBuffer)
    │
    ▼
AudioWorklet (via Blob URL)
    │  PCMPlayer processor:
    │  - Circular Float32 buffer (48000*2 samples = 1 second)
    │  - Int16 → Float32 conversion on receive
    │  - Dequeues 128-sample frames at 48kHz rate
    ▼
AudioContext (48000 Hz, stereo)
    │
    ▼
Audio Output (speakers)
```

### 7.2 Jitter Buffer

The AudioWorklet maintains a circular buffer of 48000*2 samples (1 second at stereo). This absorbs WiFi jitter up to ~1 second. Under normal conditions, the buffer stays at ~100-200ms fill level.

### 7.3 Latency Budget

| Stage | Typical | Worst Case |
|-------|---------|-----------|
| I2S read → ring buffer | < 1ms | 1ms |
| WS ring buffer → sender task | 10ms (poll) | 20ms |
| TCP send (WiFi) | 1-5ms | 20ms (no PS) |
| Browser jitter buffer | 100ms | 200ms |
| AudioWorklet output | 3ms | 3ms |
| **Total** | **~115ms** | **~244ms** |

## 8. CPU Load Estimate

| Component | CPU Load | Core |
|-----------|----------|------|
| I2S read (DMA → buffer) | 2-3% | Either |
| USB UAC2 (TinyUSB) | 2-3% | CPU1 |
| PCM volume processing | 1-2% | Either |
| WS ring buffer write | < 1% | Either |
| WS send (TCP/WiFi) | 5-10% | Either |
| WiFi/LWIP stack | 5-8% | System |
| HTTP/SSE/XDR | 2-5% | Either |
| **Total** | **~18-32%** | **Both cores** |

Sufficient headroom remains for tuner control, I2C communication, and future features.

## 9. Configuration Requirements

### sdkconfig

```
# PSRAM (ESP32-S3 N16R8 has 8MB)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768

# HTTP Server WebSocket support
CONFIG_HTTPD_WS_SUPPORT=y

# HTTP Server
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_MAX_URI_LEN=256

# CPU frequency (optional, increase from 160 to 240 for headroom)
# CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y
```

### sdkconfig.defaults

```
CONFIG_SPIRAM=y
```

## 10. File Organization

| File | Responsibility |
|------|---------------|
| `components/tuner_frontend/audio.h` | Audio init/start/stop + consumer callback API |
| `components/tuner_frontend/audio.c` | I2S driver, USB ring buffer, consumer dispatch |
| `main/web_server.c` | HTTP server, SSE, WebSocket audio endpoint + sender task |
| `main/wifi_manager.c` | WiFi init, STA/AP management, power save control |
| `main/html/index.html` | Web UI with AudioWorklet PCM player |

## 11. Implementation Checklist

- [x] PSRAM enabled in sdkconfig
- [x] HTTPD WebSocket support enabled
- [x] Audio consumer callback API (audio.h/audio.c)
- [x] Consumer dispatch from audio_task
- [x] WebSocket `/api/audio` endpoint
- [x] Independent ws_audio_sender task (non-blocking)
- [x] Ring buffer overflow handling (drop oldest)
- [x] Client lifecycle management (connect/disconnect/cleanup)
- [x] Browser AudioWorklet PCM player
- [x] WiFi power save disabled
- [ ] UAC clock-domain synchronization strategy verified on real hardware
- [ ] Long-run USB audio capture shows no periodic underflow/overflow artifacts
- [ ] Board-level verification (audio quality, latency, stability)
