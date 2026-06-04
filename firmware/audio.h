/**
 * ============================================================
 *  SPEAKFREI — audio.h
 *  I2S Audio Capture — INMP441 MEMS Microphone
 *
 *  Pipeline:
 *    INMP441 → I2S DMA → read task → silence gate →
 *    ring buffer → BLE send task → web app
 *
 *  Two FreeRTOS tasks:
 *    audioReadTask  — reads DMA, writes to ring buffer
 *    audioSendTask  — reads ring buffer, sends over BLE
 *
 *  Ring buffer is lock-free for single-producer / single-
 *  consumer (one writer task, one reader task).
 * ============================================================
 */

#pragma once
#include <Arduino.h>
#include <driver/i2s.h>
#include "config.h"
#include "ble_uart.h"
#include "display.h"

// ─────────────────────────────────────────────────────────
//  RING BUFFER
//  Simple lock-free SPSC byte ring buffer.
// ─────────────────────────────────────────────────────────
static uint8_t  _ringBuf[RING_BUF_SIZE];
static volatile uint32_t _ringHead = 0;  // write pointer
static volatile uint32_t _ringTail = 0;  // read  pointer

static inline uint32_t _ringAvailable() {
  uint32_t h = _ringHead, t = _ringTail;
  return (h >= t) ? (h - t) : (RING_BUF_SIZE - t + h);
}

static inline uint32_t _ringFree() {
  return RING_BUF_SIZE - _ringAvailable() - 1;
}

// Write len bytes into ring; returns bytes actually written.
static uint32_t _ringWrite(const uint8_t* src, uint32_t len) {
  uint32_t free = _ringFree();
  if (len > free) len = free;
  for (uint32_t i = 0; i < len; i++) {
    _ringBuf[_ringHead] = src[i];
    _ringHead = (_ringHead + 1) % RING_BUF_SIZE;
  }
  return len;
}

// Read up to len bytes from ring; returns bytes actually read.
static uint32_t _ringRead(uint8_t* dst, uint32_t len) {
  uint32_t avail = _ringAvailable();
  if (len > avail) len = avail;
  for (uint32_t i = 0; i < len; i++) {
    dst[i] = _ringBuf[_ringTail];
    _ringTail = (_ringTail + 1) % RING_BUF_SIZE;
  }
  return len;
}

// ─────────────────────────────────────────────────────────
//  AUDIO STATE
// ─────────────────────────────────────────────────────────
static volatile bool    _audioRunning   = false;
static volatile uint8_t _vuLevel        = 0;   // 0–100 for display
static volatile uint32_t _bytesSent     = 0;
static volatile uint32_t _chunksSent    = 0;

// FIX 3: Guard flag set during audioStart() ring buffer reset.
// Prevents the read task writing to the ring while head/tail are
// being zeroed, which would corrupt the SPSC invariant.
static volatile bool    _audioResetting = false;

// Silence tracking
static volatile bool    _isSilent       = true;
static volatile uint32_t _silenceConsec = 0;   // consecutive silent DMA reads
#define SILENCE_CONSEC_MAX  5                  // flush after this many silent reads

// FIX 7: Tracks samples accumulated since last flush.
// Used as a hard 5-second safety-cap flush — ensures audio is
// never silently held in the buffer indefinitely.
static volatile uint32_t _samplesSinceLastFlush = 0;

// ─────────────────────────────────────────────────────────
//  I2S INIT
// ─────────────────────────────────────────────────────────
static bool _i2sInit() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,  // INMP441 outputs 32-bit frames
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,  // L/R pin tied to GND → left
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = DMA_BUF_COUNT,
    .dma_buf_len          = DMA_BUF_LEN,
    .use_apll             = false,                       // APLL can cause instability
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };

  i2s_pin_config_t pins = {
    .bck_io_num   = PIN_I2S_SCK,
    .ws_io_num    = PIN_I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = PIN_I2S_DIN
  };

  esp_err_t err;
  err = i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);
  if (err != ESP_OK) {
    DBGF("[AUDIO] i2s_driver_install failed: %d\n", err);
    return false;
  }
  err = i2s_set_pin(I2S_PORT, &pins);
  if (err != ESP_OK) {
    DBGF("[AUDIO] i2s_set_pin failed: %d\n", err);
    return false;
  }
  i2s_zero_dma_buffer(I2S_PORT);
  DBGLN("[AUDIO] I2S driver installed");
  return true;
}

// ─────────────────────────────────────────────────────────
//  COMPUTE RMS of int16 samples, returns 0–32767
// ─────────────────────────────────────────────────────────
static uint16_t _computeRMS(const int16_t* samples, uint32_t count) {
  if (count == 0) return 0;
  int64_t sum = 0;
  for (uint32_t i = 0; i < count; i++) {
    int32_t s = samples[i];
    sum += s * s;
  }
  return (uint16_t)sqrt((double)sum / count);
}

// ─────────────────────────────────────────────────────────
//  AUDIO READ TASK
//  Runs on Core 1. Reads I2S DMA, converts 32→16 bit,
//  computes RMS for VU, writes int16 PCM to ring buffer.
// ─────────────────────────────────────────────────────────
#define READ_BUF_SAMPLES  512
static int32_t  _i2sRaw[READ_BUF_SAMPLES];   // 32-bit from DMA
static int16_t  _pcm16[READ_BUF_SAMPLES];    // converted 16-bit PCM

static void audioReadTask(void* param) {
  DBGLN("[AUDIO] Read task started on Core 1");

  while (true) {
    // FIX 3: Pause writes during audioStart() ring buffer reset.
    // Without this, resetting _ringHead/_ringTail while this task
    // is mid-write would corrupt the SPSC ring buffer.
    if (_audioResetting) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    if (!_audioRunning) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    size_t bytesRead = 0;
    esp_err_t err = i2s_read(
      I2S_PORT,
      _i2sRaw,
      READ_BUF_SAMPLES * sizeof(int32_t),
      &bytesRead,
      pdMS_TO_TICKS(100)
    );

    if (err != ESP_OK || bytesRead == 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    uint32_t samplesRead = bytesRead / sizeof(int32_t);

    // FIX 2: INMP441 outputs a 24-bit value left-justified in a 32-bit I2S frame.
    // The data occupies bits 31–8 (top 24 bits). Shifting right by 11 yields a
    // correctly-scaled signed 16-bit value. The previous value of >>14 under-
    // scaled by 8× and would cause near-silence readings on most modules.
    //
    // If audio is still very quiet after this fix, try >>8.
    // If it hard-clips, try >>14.
    // The one-shot log below prints raw sample values so you can verify on
    // first boot — remove it once you've confirmed the right shift for your board.
#if DEBUG_SERIAL
    static bool _i2sDebugPrinted = false;
    if (!_i2sDebugPrinted) {
      _i2sDebugPrinted = true;
      DBGF("[AUDIO] First raw I2S samples (32-bit): %ld  %ld  %ld  %ld\n",
        (long)_i2sRaw[0], (long)_i2sRaw[1],
        (long)_i2sRaw[2], (long)_i2sRaw[3]);
      DBGLN("[AUDIO] If these are all 0, check wiring. If >0, shift is correct.");
    }
#endif

    for (uint32_t i = 0; i < samplesRead; i++) {
      _pcm16[i] = (int16_t)(_i2sRaw[i] >> 11);  // FIX 2: was >>14
    }

    // Compute RMS for silence detection and VU meter
    uint16_t rms = _computeRMS(_pcm16, samplesRead);

    // Update VU (map RMS 0–4000 to 0–100; clip at 100)
    _vuLevel = (uint8_t)min(100UL, (unsigned long)(rms * 100UL / 4000UL));

    if (rms < SILENCE_THRESHOLD) {
      _silenceConsec++;
      _isSilent = (_silenceConsec > SILENCE_CONSEC_MAX);
    } else {
      _silenceConsec = 0;
      _isSilent      = false;
    }

    // Write to ring buffer regardless of silence — the send task
    // decides whether to flush (so short silent gaps stay in the chunk)
    uint32_t written = _ringWrite(
      (const uint8_t*)_pcm16,
      samplesRead * sizeof(int16_t)
    );

    if (written < samplesRead * sizeof(int16_t)) {
      DBGLN("[AUDIO] Ring buffer overflow — increase RING_BUF_SIZE");
    }
  }
}

// ─────────────────────────────────────────────────────────
//  AUDIO SEND TASK
//  Runs on Core 0 (same as BLE stack).
//  Reads ring buffer, sends BLE_PACKET_BYTES at a time.
//  Flushes when:
//    (a) CHUNK_BYTES of audio has accumulated (3s)
//    (b) Sustained silence detected
//    (c) FIX 7: Hard 5s cap — never hold audio longer than this
// ─────────────────────────────────────────────────────────
static uint8_t  _sendBuf[BLE_PACKET_BYTES];
static const uint32_t CHUNK_BYTES = CHUNK_SAMPLES * sizeof(int16_t);
// Hard cap: flush after 5 seconds of samples regardless of silence state.
// Prevents audio being stuck in the buffer if silence detection misfires.
static const uint32_t FLUSH_CAP_SAMPLES = SAMPLE_RATE * 5;

static void audioSendTask(void* param) {
  DBGLN("[AUDIO] Send task started on Core 0");

  while (true) {
    if (!_audioRunning || !bleTXReady()) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    uint32_t avail = _ringAvailable();

    // FIX 7: Track samples waiting in the buffer for the hard-cap flush.
    // _samplesSinceLastFlush is incremented here (send task is sole reader).
    _samplesSinceLastFlush += (avail / sizeof(int16_t));

    // Decide whether to flush:
    bool shouldFlush = (avail >= CHUNK_BYTES)
      || (_isSilent && avail > 0 && _silenceConsec > SILENCE_CONSEC_MAX)
      || (_samplesSinceLastFlush >= FLUSH_CAP_SAMPLES);  // FIX 7: hard cap

    if (!shouldFlush) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Drain the ring buffer in BLE-sized packets
    uint32_t totalSent = 0;
    while (_ringAvailable() >= BLE_PACKET_BYTES) {
      uint32_t rd = _ringRead(_sendBuf, BLE_PACKET_BYTES);
      if (rd == 0) break;

      bool ok = bleSendAudio(_sendBuf, rd);
      if (!ok) {
        // BLE busy or disconnected — back off
        vTaskDelay(pdMS_TO_TICKS(15));
        break;
      }

      totalSent += rd;
      _bytesSent += rd;
      // Small yield so BLE stack can process notify
      vTaskDelay(pdMS_TO_TICKS(5));
    }

    // Send any remaining partial packet
    uint32_t leftover = _ringAvailable();
    if (leftover > 0 && leftover < BLE_PACKET_BYTES) {
      uint32_t rd = _ringRead(_sendBuf, leftover);
      if (rd > 0 && bleSendAudio(_sendBuf, rd)) {
        totalSent += rd;
        _bytesSent += rd;
      }
    }

    if (totalSent > 0) {
      _chunksSent++;
      _samplesSinceLastFlush = 0;  // FIX 7: reset hard-cap counter after flush
      DBGF("[AUDIO] Flushed %u bytes in chunk #%u\n", totalSent, _chunksSent);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ─────────────────────────────────────────────────────────
//  PUBLIC API
// ─────────────────────────────────────────────────────────
bool audioBegin() {
  if (!_i2sInit()) return false;

  // FIX 1: Read task stack increased from 4096 → 6144 bytes.
  // _i2sRaw (2048 bytes) + _pcm16 (1024 bytes) are file-scope statics so they
  // don't live on the stack, but the task still needs headroom for i2s_read()
  // internal buffers, the FreeRTOS context frame, and debug printf formatting.
  // 4096 was borderline; 6144 gives safe margin without wasting heap.
  xTaskCreatePinnedToCore(
    audioReadTask, "audioRead",
    6144, nullptr, 5, nullptr, 1
  );

  // Send task: Core 0, priority 4, 4KB stack
  xTaskCreatePinnedToCore(
    audioSendTask, "audioSend",
    4096, nullptr, 4, nullptr, 0
  );

  _audioRunning = true;
  DBGLN("[AUDIO] Tasks started");
  return true;
}

void audioStop() {
  _audioRunning = false;
}

void audioStart() {
  // FIX 3: Guard the ring buffer reset against the read task.
  // Set _audioResetting first — the read task checks this flag at the
  // top of its loop and pauses, ensuring no write is in-flight when
  // we zero the head/tail pointers.
  _audioResetting = true;
  vTaskDelay(pdMS_TO_TICKS(20));  // wait one read-task tick to be certain
  _ringHead = _ringTail = 0;
  _silenceConsec        = 0;
  _samplesSinceLastFlush = 0;
  _audioResetting = false;
  _audioRunning   = true;
}

uint8_t  audioGetVU()         { return _vuLevel; }
bool     audioIsSilent()      { return _isSilent; }
uint32_t audioGetBytesSent()  { return _bytesSent; }
uint32_t audioGetChunks()     { return _chunksSent; }
