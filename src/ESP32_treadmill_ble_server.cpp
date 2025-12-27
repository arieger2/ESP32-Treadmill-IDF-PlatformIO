// ============================================================================
// BLE Server - Server callbacks and global BLE objects
// ============================================================================

#include "ESP32_treadmill_tacho_ble.h"

// ============================================================================
// GLOBAL BLE OBJECTS
// ============================================================================

NimBLEServer*         pServer                          = nullptr;
NimBLECharacteristic* pRSCMeasurement                  = nullptr;
NimBLECharacteristic* pFTMSTreadmillData               = nullptr;
NimBLECharacteristic* pFTMSControlPoint                = nullptr;
NimBLECharacteristic* pFTMSStatus                      = nullptr;
NimBLECharacteristic* pFTMSFitnessFeature              = nullptr;
NimBLECharacteristic* pFTMSSupportedSpeedRange         = nullptr;
NimBLECharacteristic* pFTMSSupportedInclinationRange   = nullptr;
NimBLECharacteristic* pHRMeasurement                   = nullptr;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

bool isNotificationEnabled(NimBLECharacteristic* c) {
  return (c != nullptr && bleData.clientConnected);
}

void sendFTMSStatusUpdate(uint8_t statusCode) {
  if (!pFTMSStatus || !isNotificationEnabled(pFTMSStatus)) return;

  uint8_t statusData[2] = {0x01, statusCode};
  pFTMSStatus->setValue(statusData, 2);
  pFTMSStatus->notify();

  Serial.printf("FTMS Status: 0x%02X ", statusCode);
  switch (statusCode) {
    case 0x01: Serial.println("(Control Granted)"); break;
    case 0x02: Serial.println("(Reset Complete)"); break;
    case 0x04: Serial.println("(Started/Resumed)"); break;
    case 0x05: Serial.println("(Stopped)"); break;
    case 0x06: Serial.println("(Paused)"); break;
    default:   Serial.println("(Unknown)"); break;
  }
}

// ============================================================================
// SERVER CALLBACKS
// ============================================================================

class TachoBLEServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& /*info*/) override {
    (void)s;
    bleData.clientConnected   = true;
    metrics.controlRequested  = true;
    Serial.println("=== BLE CLIENT CONNECTED ===");
    Serial.println("Ready for cadence/data transmission");
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& /*info*/, int /*reason*/) override {
    (void)s;
    bleData.clientConnected   = false;
    metrics.controlRequested  = false;
    Serial.println("=== BLE CLIENT DISCONNECTED ===");
    if (pServer && pServer->getAdvertising()) pServer->getAdvertising()->start();
  }
};

// Singleton instance
static TachoBLEServerCallbacks serverCallbacks;

NimBLEServerCallbacks* getServerCallbacks() {
  return &serverCallbacks;
}
