/**
 * ============================================================
 *  SPEAKFREI — config.h
 *  Central hardware & tuning configuration.
 *
 *  SWAP DISPLAY:  Change DISPLAY_TYPE below.
 *  SWAP MIC PINS: Change I2S_* defines below.
 *  All other files #include this — never hard-code pins elsewhere.
 * ============================================================
 */

#pragma once

// ─────────────────────────────────────────────
//  DISPLAY SELECTION
//  0 = SSD1306 0.96" 128×64   (current hardware)
//  1 = SSD1306 0.49" 64×32    (future hardware)
// ─────────────────────────────────────────────
#define DISPLAY_TYPE  0

#if   DISPLAY_TYPE == 0
  #define DISP_WIDTH      128
  #define DISP_HEIGHT      64
  #define DISP_I2C_ADDR  0x3C
  #define DISP_MODEL_STR "SSD1306 0.96\""
#elif DISPLAY_TYPE == 1
  #define DISP_WIDTH       64
  #define DISP_HEIGHT      32
  #define DISP_I2C_ADDR  0x3C
  #define DISP_MODEL_STR "SSD1306 0.49\""
#endif

// ─────────────────────────────────────────────
//  I2C PINS  (OLED)
// ─────────────────────────────────────────────
#define PIN_SDA   21
#define PIN_SCL   22

// ─────────────────────────────────────────────
//  I2S PINS  (INMP441 MEMS Microphone)
//
//  INMP441 wiring:
//    VDD  → 3.3V
//    GND  → GND
//    SD   → PIN_I2S_DIN   (serial data)
//    WS   → PIN_I2S_WS    (word select / LRCLK)
//    SCK  → PIN_I2S_SCK   (bit clock)
//    L/R  → GND           (selects LEFT channel)
// ─────────────────────────────────────────────
#define PIN_I2S_WS    15   // Word Select  (LRCLK)
#define PIN_I2S_SCK   14   // Bit Clock    (BCLK)
#define PIN_I2S_DIN   32   // Data In      (SD)

// ─────────────────────────────────────────────
//  AUDIO PARAMETERS
// ─────────────────────────────────────────────
#define SAMPLE_RATE        16000   // Hz  — Whisper optimal
#define SAMPLE_BITS           16   // bits per sample
#define I2S_CHANNEL_NUM        1   // mono
#define I2S_PORT      I2S_NUM_0

// DMA buffer: 512 samples × 8 buffers ≈ 256ms of audio in DMA
#define DMA_BUF_COUNT          8
#define DMA_BUF_LEN          512

// How many samples to accumulate before sending a BLE chunk.
// 16000 samples/s × 3s = 48000 samples = 96000 bytes raw.
// We send in smaller BLE packets; this controls how often we
// flush the ring buffer regardless of packet cadence.
#define CHUNK_SAMPLES      (SAMPLE_RATE * 3)   // 3-second chunks

// Volume threshold — samples below this RMS are treated as silence
// and not transmitted (saves BLE bandwidth). Range 0–32767.
#define SILENCE_THRESHOLD   200

// ─────────────────────────────────────────────
//  BLE  (Nordic UART Service — matches web app)
// ─────────────────────────────────────────────
#define BLE_DEVICE_NAME       "SPEAKFREI"
#define BLE_MTU_SIZE           517    // request extended MTU

// Nordic UART UUIDs (must match web app constants exactly)
#define NUS_SERVICE_UUID   "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID        "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // phone→ESP32
#define NUS_TX_UUID        "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // ESP32→phone

// BLE audio packet size in bytes.
// After MTU negotiation we can push up to ~512 bytes/notify.
// Using 500 bytes = 250 int16 samples per BLE notify.
#define BLE_PACKET_BYTES     500

// ─────────────────────────────────────────────
//  RING BUFFER
// ─────────────────────────────────────────────
// Must hold at least one full chunk. Round up to next power of 2.
// 3s × 16000 × 2 bytes = 96000 bytes → use 131072 (128 KB)
#define RING_BUF_SIZE     131072   // bytes

// ─────────────────────────────────────────────
//  DISPLAY TIMING
// ─────────────────────────────────────────────
#define SUBTITLE_DISPLAY_MS   5000  // how long to show a subtitle line
#define STATUS_REFRESH_MS      200  // OLED redraw interval

// ─────────────────────────────────────────────
//  DEBUG
//  Set to 1 to enable Serial.print logging.
// ─────────────────────────────────────────────
#define DEBUG_SERIAL   1
#define BAUD_RATE   115200

#if DEBUG_SERIAL
  #define DBG(x)     Serial.print(x)
  #define DBGLN(x)   Serial.println(x)
  #define DBGF(...)  Serial.printf(__VA_ARGS__)
#else
  #define DBG(x)
  #define DBGLN(x)
  #define DBGF(...)
#endif
