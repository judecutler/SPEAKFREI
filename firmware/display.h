/**
 * ============================================================
 *  SPEAKFREI — display.h
 *  Abstraction layer for OLED display.
 *
 *  Handles both 128×64 (0.96") and 64×32 (0.49") SSD1306.
 *  All rendering decisions are made here based on DISPLAY_TYPE.
 *
 *  Screens:
 *    - Boot splash
 *    - Status (BLE state, audio activity, VU bar)
 *    - Subtitle (received English text, word-wrapped)
 *    - Error
 * ============================================================
 */

#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "config.h"

// ── Internal display handle ────────────────────────────────
static Adafruit_SSD1306 _oled(DISP_WIDTH, DISP_HEIGHT, &Wire, -1);

// ── State shared with display routines ────────────────────
struct DisplayState {
  bool   bleConnected    = false;
  bool   audioActive     = false;
  uint8_t vuLevel        = 0;        // 0–100
  char   subtitleLine[128] = "";     // last received EN subtitle
  char   statusMsg[64]   = "BOOTING";
  bool   showSubtitle    = false;
  unsigned long subtitleShownAt = 0;
};

static DisplayState _ds;

// ─────────────────────────────────────────────────────────
//  INIT
// ─────────────────────────────────────────────────────────
bool displayBegin() {
  Wire.begin(PIN_SDA, PIN_SCL);
  if (!_oled.begin(SSD1306_SWITCHCAPVCC, DISP_I2C_ADDR)) {
    DBGLN("[DISP] SSD1306 not found — check wiring");
    return false;
  }
  _oled.clearDisplay();
  _oled.setTextWrap(false);
  _oled.dim(false);
  DBGLN("[DISP] " DISP_MODEL_STR " initialised");
  return true;
}

// ─────────────────────────────────────────────────────────
//  STATE SETTERS  (call these from other modules)
// ─────────────────────────────────────────────────────────
void displaySetBLE(bool connected) {
  _ds.bleConnected = connected;
}

void displaySetAudio(bool active, uint8_t vu = 0) {
  _ds.audioActive = active;
  _ds.vuLevel     = vu;
}

void displaySetStatus(const char* msg) {
  strncpy(_ds.statusMsg, msg, sizeof(_ds.statusMsg) - 1);
  _ds.statusMsg[sizeof(_ds.statusMsg) - 1] = '\0';
}

void displayShowSubtitle(const char* text) {
  strncpy(_ds.subtitleLine, text, sizeof(_ds.subtitleLine) - 1);
  _ds.subtitleLine[sizeof(_ds.subtitleLine) - 1] = '\0';
  _ds.showSubtitle   = true;
  _ds.subtitleShownAt = millis();
}

// ─────────────────────────────────────────────────────────
//  BOOT SPLASH
// ─────────────────────────────────────────────────────────
void displaySplash() {
  _oled.clearDisplay();

#if DISPLAY_TYPE == 0
  // 128×64: full splash
  _oled.setTextSize(2);
  _oled.setTextColor(SSD1306_WHITE);
  _oled.setCursor(8, 10);
  _oled.print("SPEAKFREI");

  _oled.setTextSize(1);
  _oled.setCursor(22, 34);
  _oled.print("JA -> EN  v0.1");

  _oled.setCursor(10, 50);
  _oled.print(DISP_MODEL_STR);

  // decorative border
  _oled.drawRect(0, 0, DISP_WIDTH, DISP_HEIGHT, SSD1306_WHITE);
  _oled.drawRect(2, 2, DISP_WIDTH - 4, DISP_HEIGHT - 4, SSD1306_WHITE);

#elif DISPLAY_TYPE == 1
  // 64×32: compact splash
  _oled.setTextSize(1);
  _oled.setTextColor(SSD1306_WHITE);
  _oled.setCursor(4, 4);
  _oled.print("SPEAKFREI");
  _oled.setCursor(4, 18);
  _oled.print("JA->EN 0.1");
  _oled.drawRect(0, 0, DISP_WIDTH, DISP_HEIGHT, SSD1306_WHITE);
#endif

  _oled.display();
  delay(2000);
}

// ─────────────────────────────────────────────────────────
//  INTERNAL: draw VU bar
//  barY = top y of bar, barH = height in pixels
// ─────────────────────────────────────────────────────────
static void _drawVU(int x, int y, int w, int h) {
  _oled.drawRect(x, y, w, h, SSD1306_WHITE);
  int filled = map(_ds.vuLevel, 0, 100, 0, w - 2);
  if (filled > 0) {
    _oled.fillRect(x + 1, y + 1, filled, h - 2, SSD1306_WHITE);
  }
}

// ─────────────────────────────────────────────────────────
//  MAIN RENDER LOOP — call from loop() on a timer
// ─────────────────────────────────────────────────────────
void displayUpdate() {
  // Auto-expire subtitle
  if (_ds.showSubtitle &&
      (millis() - _ds.subtitleShownAt > SUBTITLE_DISPLAY_MS)) {
    _ds.showSubtitle = false;
  }

  _oled.clearDisplay();
  _oled.setTextColor(SSD1306_WHITE);

#if DISPLAY_TYPE == 0
  // ── 128×64 layout ──────────────────────────────────────
  //
  //  ┌──────────────────────────────┐
  //  │ SPEAKFREI     ● BLE  ♪ AUDIO │  row 0  (h=10)
  //  ├──────────────────────────────┤
  //  │ [====VU bar===========     ] │  row 11 (h=6)
  //  ├──────────────────────────────┤
  //  │ status msg or subtitle text  │  rows 22–63
  //  └──────────────────────────────┘

  // Header bar background
  _oled.fillRect(0, 0, DISP_WIDTH, 10, SSD1306_WHITE);
  _oled.setTextColor(SSD1306_BLACK);
  _oled.setTextSize(1);
  _oled.setCursor(2, 1);
  _oled.print("SPEAKFREI");

  // BLE indicator (right side of header)
  _oled.setCursor(78, 1);
  if (_ds.bleConnected) {
    _oled.print("BLE:OK");
  } else {
    _oled.print("BLE:--");
  }

  // Audio indicator
  if (_ds.audioActive) {
    _oled.setCursor(116, 1);
    _oled.print("*");
  }

  // Reset text colour for body
  _oled.setTextColor(SSD1306_WHITE);

  // VU bar row
  _drawVU(2, 13, 124, 6);

  // Divider
  _oled.drawFastHLine(0, 21, DISP_WIDTH, SSD1306_WHITE);

  if (_ds.showSubtitle) {
    // Subtitle mode: larger text, word-wrapped manually
    _oled.setTextSize(1);
    // Simple word-wrap at ~21 chars per line (128px / 6px per char)
    const int MAX_CHARS_PER_LINE = 21;
    const int LINE_H = 10;
    int cursor_y = 24;
    const char* p = _ds.subtitleLine;
    int len = strlen(p);
    int pos = 0;

    while (pos < len && cursor_y < DISP_HEIGHT - LINE_H + 2) {
      int remaining = len - pos;
      int take = min(remaining, MAX_CHARS_PER_LINE);

      // Try to break on word boundary
      if (remaining > MAX_CHARS_PER_LINE) {
        for (int back = take; back > MAX_CHARS_PER_LINE / 2; back--) {
          if (p[pos + back] == ' ') { take = back; break; }
        }
      }

      char lineBuf[MAX_CHARS_PER_LINE + 1];
      strncpy(lineBuf, p + pos, take);
      lineBuf[take] = '\0';

      _oled.setCursor(2, cursor_y);
      _oled.print(lineBuf);
      cursor_y += LINE_H;

      pos += take;
      if (pos < len && p[pos] == ' ') pos++; // skip leading space
    }

    // Small tick countdown bar at bottom
    unsigned long elapsed = millis() - _ds.subtitleShownAt;
    int remaining_w = map(
      constrain(elapsed, 0, SUBTITLE_DISPLAY_MS),
      0, SUBTITLE_DISPLAY_MS,
      DISP_WIDTH - 4, 0
    );
    if (remaining_w > 0) {
      _oled.fillRect(2, DISP_HEIGHT - 3, remaining_w, 2, SSD1306_WHITE);
    }

  } else {
    // Status mode
    _oled.setTextSize(1);
    _oled.setCursor(2, 26);
    _oled.print(_ds.statusMsg);

    // Waiting animation: dots
    static uint8_t dotFrame = 0;
    static unsigned long lastDot = 0;
    if (millis() - lastDot > 400) { dotFrame = (dotFrame + 1) % 4; lastDot = millis(); }
    if (!_ds.bleConnected) {
      _oled.setCursor(2, 40);
      _oled.print("Waiting for app");
      for (uint8_t d = 0; d < dotFrame; d++) _oled.print('.');
    }
  }

#elif DISPLAY_TYPE == 1
  // ── 64×32 layout (0.49") ────────────────────────────────
  //
  //  ┌──────────────┐
  //  │ SPKF  BLE:OK │  row 0 (h=8)
  //  ├──────────────┤
  //  │ [==VU====  ] │  row 9 (h=4)
  //  ├──────────────┤
  //  │ subtitle/msg │  rows 15–31
  //  └──────────────┘

  // Header
  _oled.fillRect(0, 0, DISP_WIDTH, 8, SSD1306_WHITE);
  _oled.setTextColor(SSD1306_BLACK);
  _oled.setTextSize(1);
  _oled.setCursor(1, 0);
  _oled.print("SPKF");
  _oled.setCursor(32, 0);
  _oled.print(_ds.bleConnected ? "BLE:Y" : "BLE:N");

  _oled.setTextColor(SSD1306_WHITE);

  // VU bar
  _drawVU(1, 10, 62, 4);

  // Divider
  _oled.drawFastHLine(0, 16, DISP_WIDTH, SSD1306_WHITE);

  if (_ds.showSubtitle) {
    // Only one line fits: truncate to 10 chars
    _oled.setTextSize(1);
    char shortBuf[11];
    strncpy(shortBuf, _ds.subtitleLine, 10);
    shortBuf[10] = '\0';
    _oled.setCursor(1, 19);
    _oled.print(shortBuf);
    // If text overflows, show continuation marker
    if (strlen(_ds.subtitleLine) > 10) {
      _oled.setCursor(57, 19);
      _oled.print(">");
    }
  } else {
    _oled.setTextSize(1);
    char shortStatus[11];
    strncpy(shortStatus, _ds.statusMsg, 10);
    shortStatus[10] = '\0';
    _oled.setCursor(1, 19);
    _oled.print(shortStatus);
  }
#endif

  _oled.display();
}
