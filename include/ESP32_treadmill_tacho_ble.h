// ============================================================================
// BLE (FTMS + RSC) Service/Characteristics  — ESP-IDF NimBLE version
// Implements treadmill data/control + cadence service.
// ============================================================================
#ifndef BLE_HANDLER_H
#define BLE_HANDLER_H

#include "ESP32_treadmill_tacho_config.h"
#include <Arduino.h>
#include "NimBLEDevice.h"
#include "NimBLEServer.h"
#include "NimBLECharacteristic.h"

// ============================================================================
// EXTERNAL DECLARATIONS
// ============================================================================

// BLE objects
extern NimBLEServer*         pServer;
extern NimBLECharacteristic* pRSCMeasurement;
extern NimBLECharacteristic* pFTMSTreadmillData;
extern NimBLECharacteristic* pFTMSControlPoint;
extern NimBLECharacteristic* pFTMSStatus;
extern NimBLECharacteristic* pFTMSFitnessFeature;
extern NimBLECharacteristic* pFTMSSupportedSpeedRange;
extern NimBLECharacteristic* pFTMSSupportedInclinationRange;
extern NimBLECharacteristic* pHRMeasurement;

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

// BLE initialization
void initFTMS_BLE();
void initBLE_RSC();
void initHR_BLE_Server();
void BLEAdvertising_init();

// BLE data transmission
void sendFTMS_BLE_Data();
void sendRSC_BLE_Data();
void sendHR_BLE_Data();

// FTMS status helper
void sendFTMSStatusUpdate(uint8_t statusCode);

// Notification helper
bool isNotificationEnabled(NimBLECharacteristic* c);

// Diagnostic
void FMTS_check();

#endif // BLE_HANDLER_H
