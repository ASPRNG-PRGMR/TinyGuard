# TinyGuard Devlog

## Milestone 0 — Environment Setup

**Completed.**

### What was set up
- Repository structure created matching planned layout from README
- ESP32-CAM: Arduino IDE project under `firmware/esp32cam/`
  - Board: AI Thinker ESP32-CAM
  - Required board package: `esp32` by Espressif (via Arduino Board Manager)
- ESP32 DevKit: ESP-IDF v5.x project under `firmware/monitor/`
  - `CMakeLists.txt` at project root and `main/` component level
  - `sdkconfig.defaults` sets WiFi, LWIP, and FreeRTOS options
  - Stub `main.c` confirms toolchain builds successfully; logic starts at Milestone 2

### Verification steps
1. Open `firmware/esp32cam/` in Arduino IDE, select AI Thinker ESP32-CAM, compile — should produce no errors
2. `cd firmware/monitor && idf.py set-target esp32 && idf.py build` — should complete without errors
3. Flash both; monitor serial output — both should print startup messages

---

## Milestone 1 — Camera Node 

**Completed.**

### What was implemented

Four modules, each with its own `.h`/`.cpp`:

**wifi_manager** (`wifi_manager.cpp`)
- Connects to laptop hotspot using credentials in `SSID` / `PASSWORD` constants
- Configures static IP `192.168.137.10`, gateway `192.168.137.1`, subnet `255.255.255.0`
- Registers WiFi event handler: logs disconnect, increments `reconnect_count`, calls `WiFi.reconnect()`
- Blocks in `wifi_manager_init()` until connected (20s timeout → reboot)
- Exposes: `get_ip()`, `get_rssi()`, `get_reconnect_count()`

**camera_server** (`camera_server.cpp`)
- Initialises OV2640 with JPEG output; uses PSRAM if available (VGA @ quality 10), else QVGA @ quality 12
- HTTP server on port 80, two routes:
  - `GET /` — plain text status
  - `GET /stream` — MJPEG multipart stream
- Viewer count incremented on connect, decremented on disconnect
- HTTP server runs in a pinned FreeRTOS task on core 1 so `loop()` is not blocked

**telemetry_collector** (`telemetry_collector.cpp`)
- Thin aggregator: collects RSSI, uptime, stream state, viewer count, reconnect count into `telemetry_t`

**heartbeat_service** (`heartbeat_service.cpp`)
- Called from `loop()` via `heartbeat_service_tick()` — non-blocking timer check
- Sends UDP JSON packet to `192.168.137.20:5000` every 10,000 ms
- JSON fields: `device`, `uptime`, `rssi`, `stream_active`, `viewer_count`, `reconnects`, `timestamp`
- Logs send result to serial

### Verification steps
1. Flash camera firmware; open Serial Monitor at 115200 baud
2. Confirm: `[WiFi] Connected. IP: 192.168.137.10`
3. Open browser → `http://192.168.137.10/stream` — should display live MJPEG
4. On a separate machine, run `nc -ul 5000` or Wireshark on the hotspot interface
5. Confirm heartbeat JSON arrives every ~10 seconds

### Known limitations
- `timestamp` is uptime seconds, not wall-clock time. NTP sync deferred to a later milestone.
- No retry logic if UDP send fails repeatedly (acceptable for local LAN).
- Stream handler is synchronous per-client; only one viewer at a time. Sufficient for MVP.

---

## Milestone 2 — Monitor Node

**Completed.**

### What was implemented

**wifi_manager** (`wifi_manager.c` — ESP-IDF)
- Initialises NVS, netif, event loop
- Sets static IP `192.168.137.20` by stopping DHCP and calling `esp_netif_set_ip_info()`
- Event handler: reconnects on disconnect, sets event group bit on IP acquisition
- `wifi_manager_init()` blocks until connected (20s timeout → restart)

**device_state** (`device_state.c`)
- Mutex-protected global struct: `valid`, `uptime_ms`, `rssi`, `stream_active`, `viewer_count`, `reconnects`, `timestamp`, `last_rx_tick`, `packet_count`
- `device_state_update()` / `device_state_get()` — safe to call from any task

**udp_receiver** (`udp_receiver.c`)
- Binds `SOCK_DGRAM` on `0.0.0.0:5000`
- `recvfrom()` loop in a dedicated FreeRTOS task (4KB stack, priority 5)
- JSON parsing: `strstr()` + `strtol()` per field — no dynamic allocation, no JSON library
- After each packet: updates device_state, feeds stats engine, logs structured summary to serial
- Blinks GPIO2 LED for 80ms per heartbeat — visual confirmation without a second UART

**main.c**
- Calls `wifi_manager_init()` → `device_state_init()` → `stats_engine_init()` → `udp_receiver_start()`
- Main task logs a health summary every 30s

### Verification steps
1. Set `WIFI_SSID` / `WIFI_PASSWORD` in `firmware/monitor/main/wifi_manager.c`
2. Flash monitor; connect UART to monitor DevKit
3. Confirm `[WiFi-Monitor] Connected. IP: 192.168.137.20`
4. Power camera from separate supply
5. Serial shows heartbeat summaries every 10 seconds
6. LED on GPIO2 blinks once per heartbeat

### Errors encountered

#### Error 1 — `IPSTR` inside `ESP_LOGI` causes parse error

**Symptom:** `expected ')' before 'IPSTR'`

**Root cause:** `IPSTR` expands to the string literal `"%d.%d.%d.%d"`. When concatenated into an `ESP_LOGI` format string, the logging macro's internal `LOG_FORMAT` sees extra `%d` specifiers with no matching arguments, producing a malformed variadic call the preprocessor cannot parse.

**Fix:** Format IP into a `char[16]` buffer with `inet_ntoa()`, pass as `%s`.

```c
char src_ip[16];
strncpy(src_ip, inet_ntoa(src_addr.sin_addr), sizeof(src_ip) - 1);
src_ip[sizeof(src_ip) - 1] = '\0';
ESP_LOGI(TAG, "Source IP: %s", src_ip);
```

#### Error 2 — `%lu` format mismatch under `-Werror=format=`

**Symptom:** `format '%lu' expects 'unsigned long' but argument is 'uint32_t'`

**Root cause:** ESP-IDF v5 compiles with `-Werror=format=`. On Xtensa/ESP32, `uint32_t` is `unsigned int`, not `unsigned long`. `%lu` is a hard error.

**Fix:** Use `PRIu32` from `<inttypes.h>` throughout.

```c
ESP_LOGI(TAG, "Uptime: %" PRIu32 " ms", state.uptime_ms);
```

---

## Milestone 3 — Statistics Engine 

**Completed. Builds successfully.**

### What was implemented

**stats_engine** (`stats_engine.h` / `stats_engine.c`)

Four metrics tracked, each with its own rolling window:
- RSSI (dBm, one sample per heartbeat)
- Heartbeat interval (ms between consecutive heartbeat arrivals)
- Reconnect rate (derived from the cumulative counter delta, expressed as reconnects/hour)
- Stream viewer count

**Algorithm — Welford's online method:**
- Mean and variance updated incrementally on each new sample
- Numerically stable; avoids catastrophic cancellation that affects naive sum/count approaches
- When the circular buffer is full (60 samples = 10 minutes at 10s heartbeat rate), the oldest sample is evicted and Welford accumulators are recomputed from the window in O(60) — cheap at this sample rate

**Baseline learning phase:**
- Detection suppressed for the first 300 seconds (5 minutes, per spec)
- Timer starts on the first received heartbeat
- On completion, logs mean and stddev for each metric as the established baseline

**Reconnect rate derivation:**
- Camera reports a cumulative reconnect counter
- Monitor tracks the delta between consecutive readings and the elapsed time
- Converts to reconnects/hour: `rate = (delta / elapsed_s) * 3600`

**Integration:**
- `udp_receiver.c` calls `stats_engine_update()` on every heartbeat after updating device state
- `main.c` calls `stats_engine_get_snapshot()` every 30s and logs the full stats table
- `CMakeLists.txt` links `libm` for `sqrtf()`

### Serial output after learning completes
```
I (xxxxx) Stats: Learning complete after 30 samples. Detection enabled.
I (xxxxx) Stats:   RSSI baseline    : mean=-42.1 stddev=1.2
I (xxxxx) Stats:   HB interval base : mean=10012 ms stddev=45 ms
```

### Verification steps
1. Flash monitor with Milestone 3 firmware
2. Let run for 5+ minutes
3. Confirm `[LEARNING]` countdown in 30s summary during learning phase
4. Confirm `Learning complete` log and baseline values after 5 minutes
5. Confirm `[ACTIVE]` summary with populated mean/stddev values

---

## Milestone 4+ — Not yet implemented

See TASKLIST.md for remaining work.
