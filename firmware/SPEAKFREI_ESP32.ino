/**
 * ============================================================
 *  SPEAKFREI — SPEAKFREI_ESP32.ino
 *  Main sketch — ties all modules together.
 *
 *  Dependency graph:
 *    config.h    ← (no deps, included by all)
 *    display.h   ← config.h, Wire, Adafruit_SSD1306
 *    ble_uart.h  ← config.h, display.h, BLE libs
 *    audio.h     ← config.h, ble_uart.h, display.h, I2S driver
 *
 *  Required Arduino libraries (install via Library Manager):
 *    - Adafruit SSD1306          (by Adafruit)
 *    - Adafruit GFX Library      (by Adafruit)
 *    - ESP32 BLE Arduino         (built-in with ESP32 board package)
 *
 *  Board: ESP32 Dev Module
 *  Partition scheme: Default (or "Huge APP" if you add OTA later)
 *  CPU frequency: 240MHz
 *  Flash size: 4MB
 *
 *  ── Pin Summary ──────────────────────────────────────
 *  INMP441 MEMS Mic:
 *    VDD  → 3.3V
 *    GND  → GND
 *    L/R  → GND           (select left channel)
 *    WS   → GPIO15
 *    SCK  → GPIO14
 *    SD   → GPIO32
 *
 *  SSD1306 OLED (I2C, 4-pin):
 *    VCC  → 3.3V
 *    GND  → GND
 *    SDA  → GPIO21
 *    SCL  → GPIO22
 *  ─────────────────────────────────────────────────────
 * ============================================================
 */

// Include order matters: config first, then hardware modules.
#include "config.h"
#include "display.h"
#include "ble_uart.h"
#include "audio.h"

// ─────────────────────────────────────────────────────────
//  APPLICATION STATE MACHINE
// ─────────────────────────────────────────────────────────
enum class AppState : uint8_t {
  BOOT,           // Splash screen, hardware init
  ADVERTISING,    // BLE advertising, waiting for web app
  CONNECTED,      // BLE connected, streaming audio
  DISCONNECTED,   // Lost connection, will return to ADVERTISING
  ERROR           // Fatal init failure
};

static AppState _appState = AppState::BOOT;

// ─────────────────────────────────────────────────────────
//  TIMING
// ─────────────────────────────────────────────────────────
static unsigned long _lastDisplayUpdate  = 0;
static unsigned long _lastStatusLog      = 0;
#define STATUS_LOG_INTERVAL_MS  5000

// ─────────────────────────────────────────────────────────
//  SUBTITLE RECEIVE BUFFER
// ─────────────────────────────────────────────────────────
static char _rxSubtitle[SUBTITLE_BUF_SIZE];

// ─────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────
void setup() {
#if DEBUG_SERIAL
  Serial.begin(BAUD_RATE);
  delay(500);  // let serial monitor connect
  Serial.println("\n\n=== SPEAKFREI v0.1 ===");
  Serial.println("Initialising hardware...");
#endif

  // ── 1. Display ──────────────────────────────────────────
  // FIX 8: Capture the init result and gate ALL display calls on it.
  // Previously displaySplash() was called unconditionally even when
  // displayBegin() returned false, which calls methods on an
  // uninitialised Adafruit_SSD1306 object and can cause a hard fault.
  bool _displayOk = displayBegin();
  if (!_displayOk) {
    DBGLN("[INIT] WARNING: Display failed — continuing without it.");
  }
  if (_displayOk) displaySplash();
  if (_displayOk) displaySetStatus("INIT...");
  if (_displayOk) displayUpdate();

  // ── 2. BLE ──────────────────────────────────────────────
  if (_displayOk) displaySetStatus("BLE INIT...");
  if (_displayOk) displayUpdate();

  if (!bleBegin()) {
    DBGLN("[INIT] FATAL: BLE failed to start.");
    if (_displayOk) displaySetStatus("BLE FAIL");
    if (_displayOk) displayUpdate();
    _appState = AppState::ERROR;
    return;
  }
  DBGLN("[INIT] BLE OK");

  // ── 3. Audio ─────────────────────────────────────────────
  if (_displayOk) displaySetStatus("AUDIO INIT...");
  if (_displayOk) displayUpdate();

  if (!audioBegin()) {
    DBGLN("[INIT] FATAL: Audio (I2S) failed to start.");
    if (_displayOk) displaySetStatus("I2S FAIL");
    if (_displayOk) displayUpdate();
    _appState = AppState::ERROR;
    return;
  }
  // Start in stopped state — begin streaming once BLE connects
  audioStop();
  DBGLN("[INIT] Audio OK");

  // ── 4. Ready ─────────────────────────────────────────────
  _appState = AppState::ADVERTISING;
  if (_displayOk) displaySetBLE(false);
  if (_displayOk) displaySetStatus("ADVERTISING");
  if (_displayOk) displayUpdate();
  DBGLN("[INIT] System ready — advertising as \"" BLE_DEVICE_NAME "\"");
}

// ─────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────
void loop() {
  // ── Handle fatal error state ───────────────────────────
  if (_appState == AppState::ERROR) {
    // Blink error on display, do nothing else
    static unsigned long lastBlink = 0;
    static bool blink = false;
    if (millis() - lastBlink > 500) {
      blink = !blink;
      displaySetStatus(blink ? "!! ERROR !!" : "CHECK WIRING");
      displayUpdate();
      lastBlink = millis();
    }
    return;
  }

  // ── BLE state transitions ─────────────────────────────
  bool nowConnected = bleIsConnected();

  if (nowConnected && _appState == AppState::ADVERTISING) {
    _appState = AppState::CONNECTED;
    audioStart();
    displaySetAudio(true, 0);
    DBGLN("[APP] State: CONNECTED — audio streaming started");
  }

  if (!nowConnected && _appState == AppState::CONNECTED) {
    _appState = AppState::DISCONNECTED;
    audioStop();
    displaySetAudio(false, 0);
    DBGLN("[APP] State: DISCONNECTED — audio stopped");
  }

  if (!nowConnected && _appState == AppState::DISCONNECTED) {
    // bleUpdate() handles re-advertising; we just track state
    _appState = AppState::ADVERTISING;
    DBGLN("[APP] State: ADVERTISING");
  }

  // ── BLE housekeeping ──────────────────────────────────
  bleUpdate();

  // ── Update VU meter on display ────────────────────────
  if (_appState == AppState::CONNECTED) {
    displaySetAudio(true, audioGetVU());
  }

  // ── Poll for incoming subtitle from web app ───────────
  if (bleGetSubtitle(_rxSubtitle, sizeof(_rxSubtitle))) {
    displayShowSubtitle(_rxSubtitle);
    DBGF("[APP] Showing subtitle: %s\n", _rxSubtitle);
  }

  // ── Periodic display refresh ──────────────────────────
  if (millis() - _lastDisplayUpdate >= STATUS_REFRESH_MS) {
    displayUpdate();
    _lastDisplayUpdate = millis();
  }

  // ── Periodic serial status log ────────────────────────
#if DEBUG_SERIAL
  if (millis() - _lastStatusLog >= STATUS_LOG_INTERVAL_MS) {
    _lastStatusLog = millis();
    DBGF("[STATUS] State=%d  BLE=%s  TXrdy=%s  VU=%u  "
         "ChunksSent=%u  BytesSent=%u  FreeMem=%u\n",
      (int)_appState,
      bleIsConnected() ? "Y" : "N",
      bleTXReady()     ? "Y" : "N",
      audioGetVU(),
      audioGetChunks(),
      audioGetBytesSent(),
      ESP.getFreeHeap()
    );
  }
#endif

  // ── Yield to FreeRTOS scheduler ───────────────────────
  // The audio tasks run on both cores — give them time.
  vTaskDelay(pdMS_TO_TICKS(10));
}
