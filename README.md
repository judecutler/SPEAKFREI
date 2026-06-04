# SPEAKFREI — ESP32 Firmware

Real-time Japanese → English AR subtitle glasses attachment.
Streams 16kHz mono PCM audio over BLE (Nordic UART) to the SPEAKFREI web app.

---

## File Structure

```
SPEAKFREI_ESP32/
├── SPEAKFREI_ESP32.ino   ← Main sketch (open this in Arduino IDE)
├── config.h              ← ALL pin/hardware/tuning config lives here
├── display.h             ← OLED abstraction (0.96" and 0.49" SSD1306)
├── ble_uart.h            ← BLE Nordic UART Service
├── audio.h               ← I2S capture + ring buffer + BLE send
└── README.md             ← This file
```

---

## Hardware

| Component | Model |
|---|---|
| MCU | ESP32 Dev Module (dual-core, 240 MHz) |
| Microphone | INMP441 I2S MEMS Mic Module |
| Display | 0.96" SSD1306 OLED — 128×64, I2C, 4-pin |
| Battery | 3.7V LiPo 500mAh + TP4056 charger module |

### Future display swap
When moving to the 0.49" SSD1306 (64×32), open `config.h` and change:
```cpp
#define DISPLAY_TYPE  0   // ← change to 1
```
That's it. All layout code adjusts automatically.

---

## Wiring

### INMP441 Microphone → ESP32
```
INMP441     ESP32
───────     ─────────────────
VDD    →   3.3V
GND    →   GND
L/R    →   GND         ← ties mic to LEFT channel
WS     →   GPIO 15     ← Word Select (LRCLK)
SCK    →   GPIO 14     ← Bit Clock   (BCLK)
SD     →   GPIO 32     ← Serial Data (DIN)
```

### SSD1306 OLED (4-pin I2C) → ESP32
```
OLED        ESP32
────        ─────────────────
VCC    →   3.3V
GND    →   GND
SDA    →   GPIO 21
SCL    →   GPIO 22
```

> **Note:** Most 0.96" SSD1306 modules with 4 pins are I2C (not SPI).
> The pin order on the module is typically: GND, VCC, SCL, SDA.
> Double-check your specific module — some have VCC and GND swapped.

---

## Arduino IDE Setup

### 1. Install ESP32 Board Package
- File → Preferences → Additional Boards Manager URLs:
  ```
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
  ```
- Tools → Board → Boards Manager → search "esp32" → install **esp32 by Espressif Systems** (v2.x)

### 2. Install Libraries
Tools → Manage Libraries → install:
- **Adafruit SSD1306** (by Adafruit) — v2.5+
- **Adafruit GFX Library** (by Adafruit) — v1.11+

The BLE and I2S libraries are bundled with the ESP32 board package — no extra install needed.

### 3. Board Settings
```
Board:            ESP32 Dev Module
Upload Speed:     921600
CPU Frequency:    240MHz
Flash Frequency:  80MHz
Flash Mode:       QIO
Flash Size:       4MB (32Mb)
Partition Scheme: Default 4MB with spiffs
  (or "Huge APP" if you later add OTA)
PSRAM:            Disabled
```

### 4. Open & Upload
- Open `SPEAKFREI_ESP32.ino` in Arduino IDE
- All `.h` files must be in the same folder as the `.ino`
- Select your COM port
- Click Upload

---

## Connecting to the Web App

1. Power the ESP32
2. On the OLED you will see **ADVERTISING**
3. Open the SPEAKFREI web app in **Chrome desktop** (Web Bluetooth requires Chrome ≥ 85, desktop only — not Safari, not Firefox, not Chrome on iOS)
4. Select **BLE** mode in the app
5. Click **Connect BLE**
6. Chrome will show a device picker — select **SPEAKFREI**
7. OLED changes to **CONNECTED**, audio streaming begins immediately

---

## Audio Format

The ESP32 sends raw **16kHz, mono, signed int16 little-endian PCM** over BLE notify packets of up to 500 bytes (250 samples) each.

The web app accumulates these packets into 3-second chunks and sends them to the Whisper STT API.

---

## Serial Debug Output

Open Serial Monitor at **115200 baud** to see:
```
=== SPEAKFREI v0.1 ===
[INIT] BLE OK
[INIT] Audio OK
[BLE]  Advertising started
[BLE]  Client connected
[AUDIO] Flushed 96000 bytes in chunk #1
[APP]  Showing subtitle: Hello, how are you?
[STATUS] State=2  BLE=Y  TXrdy=Y  VU=42  ChunksSent=3  BytesSent=288000  FreeMem=182340
```

To disable all serial output (production / battery saving), open `config.h` and set:
```cpp
#define DEBUG_SERIAL  0
```

---

## Tuning Parameters (all in `config.h`)

| Parameter | Default | Notes |
|---|---|---|
| `CHUNK_SAMPLES` | 48000 (3s) | Increase for better STT accuracy, adds latency |
| `SILENCE_THRESHOLD` | 200 | RMS below this = silence. Raise if mic is noisy |
| `BLE_PACKET_BYTES` | 500 | Reduce to 128 if BLE drops packets |
| `SUBTITLE_DISPLAY_MS` | 5000 | How long subtitle stays on screen |
| `STATUS_REFRESH_MS` | 200 | OLED refresh rate in ms |

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| OLED shows nothing | Wrong I2C address | Try `0x3D` in `config.h` → `DISP_I2C_ADDR` |
| OLED shows "SSD1306 not found" | Bad wiring or 5V on 3.3V pin | Check SDA/SCL and power |
| No audio / silent chunks | INMP441 not wired correctly | Check L/R pin is GND. Check SCK/WS/SD |
| Distorted audio | Bit shift wrong for your module | Try changing `>> 14` to `>> 11` in `audio.h` line ~115 |
| BLE not visible in Chrome | Device not advertising | Check serial for `[BLE] Advertising started` |
| Chrome can't connect | MTU negotiation | Try reducing `BLE_MTU_SIZE` to 247 in `config.h` |
| Subtitles not showing | RX char not writing | Ensure web app sends subtitle JSON to RX UUID |
| Frequent disconnects | BLE interference / range | Move closer; try setting `minInterval` to 12 in `ble_uart.h` |
| Heap panic / crash | Stack overflow | Increase task stack: change `4096` to `8192` in `audioBegin()` |

---

## Partition Note
The BLE + I2S + Adafruit GFX combo is large. If you get "Sketch too big":
- Tools → Partition Scheme → **Huge APP (3MB No OTA)**

---

## Roadmap Hooks

The codebase is structured for these planned additions:
- **OTA update**: add `ArduinoOTA` to `setup()` — BLE task is on Core 0, OTA can share it
- **Battery monitor**: add ADC read on GPIO34 (with voltage divider) and call `displaySetStatus()` with level
- **Wake word**: add a lightweight VAD (voice activity detect) before `audioStart()` to save battery
- **Multi-language**: `config.h` has room for a `LANG_MODE` define that the web app reads from the RX characteristic
