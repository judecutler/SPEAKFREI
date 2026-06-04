/**
 * ============================================================
 *  SPEAKFREI — ble_uart.h
 *  BLE Nordic UART Service (NUS)
 *
 *  Role: PERIPHERAL (server)
 *    TX characteristic → ESP32 sends PCM audio chunks TO phone
 *    RX characteristic → phone sends subtitle text TO ESP32
 *
 *  Designed to pair with the SPEAKFREI web app running in
 *  Chrome (Web Bluetooth API).
 *
 *  Connection state machine:
 *    IDLE → ADVERTISING → CONNECTED → (DISCONNECTED → ADVERTISING)
 * ============================================================
 */

#pragma once
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "config.h"
#include "display.h"

// ── Forward declarations ───────────────────────────────────
static void bleStartAdvertising();

// ── BLE handles ───────────────────────────────────────────
static BLEServer*          _bleServer    = nullptr;
static BLECharacteristic*  _txChar       = nullptr;  // ESP32→phone (audio)
static BLECharacteristic*  _rxChar       = nullptr;  // phone→ESP32 (subtitles)

// ── Connection state ──────────────────────────────────────
static volatile bool _bleConnected     = false;
static volatile bool _bleWasConnected  = false;  // for re-advertising on disconnect

// ── Subtitle receive callback ─────────────────────────────
// Called from BLE task — keep it fast, just copy to buffer
#define SUBTITLE_BUF_SIZE  256
static char   _subtitleBuf[SUBTITLE_BUF_SIZE] = "";
static volatile bool _subtitleReady = false;

// ── TX notify available flag ──────────────────────────────
// True when client has subscribed to TX notifications.
static volatile bool _txNotifyEnabled = false;

// ─────────────────────────────────────────────────────────
//  SERVER CALLBACKS
// ─────────────────────────────────────────────────────────
class SPKServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    _bleConnected    = true;
    _bleWasConnected = true;

    // Request extended MTU so we can send larger audio packets
    server->updateConnParams(
      server->getConnId(),
      6,    // minInterval × 1.25ms = 7.5ms
      12,   // maxInterval × 1.25ms = 15ms
      0,    // latency
      500   // timeout × 10ms = 5s
    );

    DBGLN("[BLE] Client connected");
    displaySetBLE(true);
    displaySetStatus("CONNECTED");
  }

  void onDisconnect(BLEServer* server) override {
    _bleConnected      = false;
    _txNotifyEnabled   = false;
    DBGLN("[BLE] Client disconnected — restarting advertising");
    displaySetBLE(false);
    displaySetStatus("DISCONNECTED");
    // Re-advertising is handled in bleUpdate() to avoid calling
    // BLE stack functions from within a callback.
  }
};

// ─────────────────────────────────────────────────────────
//  TX DESCRIPTOR CALLBACKS  (subscription state)
// ─────────────────────────────────────────────────────────
class SPKTXDescCallbacks : public BLEDescriptorCallbacks {
  void onWrite(BLEDescriptor* desc) override {
    // FIX 5: Guard against zero-length or single-byte descriptor writes.
    // During BLE negotiation the stack can fire this callback with partial
    // data; reading data[1] without checking length is an out-of-bounds read.
    uint8_t* data = desc->getValue();
    size_t   len  = desc->getLength();
    if (!data || len < 2) {
      DBGLN("[BLE] TX desc write ignored — payload too short");
      return;
    }
    uint16_t val  = (uint16_t)((data[1] << 8) | data[0]);
    _txNotifyEnabled = (val == 0x0001);
    DBGF("[BLE] TX notify %s\n", _txNotifyEnabled ? "enabled" : "disabled");
  }
};

// ─────────────────────────────────────────────────────────
//  RX CHARACTERISTIC CALLBACKS  (subtitle receive)
// ─────────────────────────────────────────────────────────
class SPKRXCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    std::string val = characteristic->getValue();
    if (val.length() == 0) return;

    // Safely copy into buffer
    size_t copyLen = min(val.length(), (size_t)(SUBTITLE_BUF_SIZE - 1));
    memcpy(_subtitleBuf, val.c_str(), copyLen);
    _subtitleBuf[copyLen] = '\0';
    _subtitleReady = true;

    DBGF("[BLE] Subtitle RX: %s\n", _subtitleBuf);
  }
};

// ─────────────────────────────────────────────────────────
//  INIT
// ─────────────────────────────────────────────────────────
bool bleBegin() {
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(BLE_MTU_SIZE);

  _bleServer = BLEDevice::createServer();
  if (!_bleServer) {
    DBGLN("[BLE] Failed to create server");
    return false;
  }
  _bleServer->setCallbacks(new SPKServerCallbacks());

  // Create NUS service
  BLEService* nusSvc = _bleServer->createService(NUS_SERVICE_UUID);
  if (!nusSvc) {
    DBGLN("[BLE] Failed to create NUS service");
    return false;
  }

  // TX Characteristic: ESP32 → phone  (NOTIFY)
  _txChar = nusSvc->createCharacteristic(
    NUS_TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  if (!_txChar) { DBGLN("[BLE] TX char failed"); return false; }

  BLE2902* txDesc = new BLE2902();
  txDesc->setCallbacks(new SPKTXDescCallbacks());
  _txChar->addDescriptor(txDesc);

  // RX Characteristic: phone → ESP32  (WRITE, WRITE_NR)
  _rxChar = nusSvc->createCharacteristic(
    NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  if (!_rxChar) { DBGLN("[BLE] RX char failed"); return false; }
  _rxChar->setCallbacks(new SPKRXCallbacks());

  nusSvc->start();
  bleStartAdvertising();

  DBGLN("[BLE] NUS service started, advertising as \"" BLE_DEVICE_NAME "\"");
  return true;
}

// ─────────────────────────────────────────────────────────
//  ADVERTISING
// ─────────────────────────────────────────────────────────
static void bleStartAdvertising() {
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  // Advertise the full device name in scan response
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  displaySetStatus("ADVERTISING");
  DBGLN("[BLE] Advertising started");
}

// ─────────────────────────────────────────────────────────
//  SEND AUDIO PACKET
//  Returns true if packet was sent (notify was accepted).
//  data: pointer to raw int16 PCM bytes
//  len : number of BYTES (not samples) — max BLE_PACKET_BYTES
// ─────────────────────────────────────────────────────────
bool bleSendAudio(const uint8_t* data, size_t len) {
  if (!_bleConnected || !_txNotifyEnabled) return false;
  if (len == 0 || len > BLE_PACKET_BYTES)  return false;

  _txChar->setValue((uint8_t*)data, len);
  _txChar->notify();
  return true;
}

// ─────────────────────────────────────────────────────────
//  POLL for subtitle received from phone
//  Returns true once when a new subtitle is available.
//  Caller copies _subtitleBuf immediately.
// ─────────────────────────────────────────────────────────
bool bleGetSubtitle(char* outBuf, size_t outLen) {
  if (!_subtitleReady) return false;
  _subtitleReady = false;
  strncpy(outBuf, _subtitleBuf, outLen - 1);
  outBuf[outLen - 1] = '\0';
  return true;
}

// ─────────────────────────────────────────────────────────
//  UPDATE — call from main loop()
//  Handles re-advertising after disconnect.
// ─────────────────────────────────────────────────────────

// FIX 4: Tracks when the disconnect was first detected.
// Replaces the blocking delay(200) which froze the entire loop
// (display, subtitle polling, everything) for 200ms on every disconnect.
static unsigned long _bleDisconnectTime = 0;

void bleUpdate() {
  if (_bleWasConnected && !_bleConnected) {
    // Record the moment we first noticed the disconnect
    if (_bleDisconnectTime == 0) {
      _bleDisconnectTime = millis();
    }
    // Wait 200ms non-blocking before restarting advertising,
    // giving the BLE stack time to release its connection resources.
    if (millis() - _bleDisconnectTime >= 200) {
      bleStartAdvertising();
      _bleWasConnected   = false;
      _bleDisconnectTime = 0;  // reset for next disconnect
    }
  }
}

// ─────────────────────────────────────────────────────────
//  STATE ACCESSORS
// ─────────────────────────────────────────────────────────
bool bleIsConnected()     { return _bleConnected; }
bool bleTXReady()         { return _bleConnected && _txNotifyEnabled; }
