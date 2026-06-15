// ============================================================================
// BLE FTMS/RSC/HR Initialization - Service setup and configuration
// ============================================================================

#include "ESP32_treadmill_tacho_ble.h"

// Forward declarations for callbacks
extern NimBLEServerCallbacks* getServerCallbacks();
extern NimBLECharacteristicCallbacks* getFTMSControlPointCallbacks();

// ============================================================================
// INIT: FTMS
// ============================================================================

void initFTMS_BLE() {
  Serial.println("Initializing FTMS BLE service...");

  try {
    NimBLEDevice::init(storedGlobals.BLE_DEVICE_NAME.c_str());
    NimBLEDevice::setMTU(247);

    pServer = NimBLEDevice::createServer();
    if (!pServer) { Serial.println("❌ Failed to create BLE server"); return; }
    pServer->setCallbacks(getServerCallbacks());

    // FTMS Service
    NimBLEService* pFTMSService = pServer->createService(FTMS_SERVICE_UUID);
    if (!pFTMSService) { Serial.println("❌ Failed to create FTMS service"); return; }

    // Characteristics
    pFTMSTreadmillData = pFTMSService->createCharacteristic(
      TREADMILL_DATA_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pFTMSFitnessFeature = pFTMSService->createCharacteristic(
      FTMS_FEATURE_UUID,
      NIMBLE_PROPERTY::READ
    );

    pFTMSControlPoint = pFTMSService->createCharacteristic(
      FTMS_CONTROL_POINT_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::INDICATE | NIMBLE_PROPERTY::WRITE_NR,
      20
    );

    pFTMSStatus = pFTMSService->createCharacteristic(
      FTMS_STATUS_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pFTMSSupportedSpeedRange = pFTMSService->createCharacteristic(
      SUPPORTED_SPEED_RANGE_UUID,
      NIMBLE_PROPERTY::READ
    );

    pFTMSSupportedInclinationRange = pFTMSService->createCharacteristic(
      SUPPORTED_INCLINATION_RANGE_UUID,
      NIMBLE_PROPERTY::READ
    );

    // Control Point callbacks
    if (pFTMSControlPoint) {
      pFTMSControlPoint->setCallbacks(getFTMSControlPointCallbacks());
      Serial.println("✅ FTMS Control Point callback set");
    }

    // FTMS Features value (little-endian, 32-bit set; sent as 8 bytes)
    uint32_t ftmsFeatures =
        (1u << 0) |   // Average Speed Supported
        (1u << 1) |   // Total Distance Supported
        (1u << 3) |   // Inclination Supported
        (1u << 8) |   // Target Speed Supported
        (1u << 9) |   // Target Inclination Supported
        (1u << 18);   // Heart Rate Target Supported

    uint8_t featuresArray[8] = {0};
    featuresArray[0] = (ftmsFeatures >>  0) & 0xFF;
    featuresArray[1] = (ftmsFeatures >>  8) & 0xFF;
    featuresArray[2] = (ftmsFeatures >> 16) & 0xFF;
    featuresArray[3] = (ftmsFeatures >> 24) & 0xFF;
    pFTMSFitnessFeature->setValue(featuresArray, 8);

    // Speed range: 1.0–30.0 km/h in 0.1 km/h
    uint8_t speedRange[6] = {10, 0, 44, 1, 1, 0};
    pFTMSSupportedSpeedRange->setValue(speedRange, 6);

    // Incline range: -15.0–40.0% in 0.5%
    uint8_t inclineRange[6] = {106, 255, 144, 1, 5, 0};
    pFTMSSupportedInclinationRange->setValue(inclineRange, 6);

    // Initial status
    uint8_t status = 0x00;
    pFTMSStatus->setValue(&status, 1);

    // Sanity check CCCD presence
    NimBLEDescriptor* cccdNow =
      pFTMSControlPoint->getDescriptorByUUID(NimBLEUUID((uint16_t)0x2902));
    Serial.println(cccdNow ? "FTMS: CP CCCD present" : "FTMS: CP CCCD MISSING");

    Serial.println("✅ FTMS Service started successfully");
  } catch (...) {
    Serial.println("❌ FTMS BLE initialization failed");
  }
}

// ============================================================================
// INIT: RSC
// ============================================================================

void initBLE_RSC() {
  Serial.println("Initializing RSC BLE service...");

  try {
    if (!pServer) return;

    NimBLEService* pRSCService = pServer->createService(RSC_SERVICE_UUID);

    // RSC Feature: bit1=Total Distance supported, bit2=Walking/Running Status supported
    NimBLECharacteristic* pRSCFeature = pRSCService->createCharacteristic(
      RSC_FEATURE_UUID, NIMBLE_PROPERTY::READ
    );
    uint16_t rscFeatures = 0x0006;
    pRSCFeature->setValue((uint8_t*)&rscFeatures, 2);
    Serial.printf("✅ RSC Features: 0x%04X (Total Distance: %s, Running Status: %s)\r\n",
                  rscFeatures,
                  (rscFeatures & 0x0002) ? "YES" : "NO",
                  (rscFeatures & 0x0004) ? "YES" : "NO");
    Serial.println();

    // Measurement (notify)
    pRSCMeasurement = pRSCService->createCharacteristic(
      RSC_MEASUREMENT_UUID, NIMBLE_PROPERTY::NOTIFY
    );

    Serial.println("✅ RSC Service created - Speed, Cadence, Distance transmission enabled");
  } catch (...) {
    Serial.println("❌ RSC initialization failed");
  }
}

// ============================================================================
// INIT: HEART RATE SERVER
// ============================================================================

void initHR_BLE_Server() {
  Serial.println("Initializing Heart Rate Server (broadcast mode)...");

  try {
    if (!pServer) {
      Serial.println("❌ Server not initialized");
      return;
    }

    // Create Heart Rate Service (0x180D)
    NimBLEService* pHRService = pServer->createService(NimBLEUUID((uint16_t)0x180D));
    if (!pHRService) {
      Serial.println("❌ Failed to create HR service");
      return;
    }

    // Heart Rate Measurement Characteristic (0x2A37) - Notify only
    pHRMeasurement = pHRService->createCharacteristic(
      NimBLEUUID((uint16_t)0x2A37),
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    if (!pHRMeasurement) {
      Serial.println("❌ Failed to create HR measurement characteristic");
      return;
    }

    // Body Sensor Location (optional but recommended) - Chest = 0x01
    NimBLECharacteristic* pBodySensorLocation = pHRService->createCharacteristic(
      NimBLEUUID((uint16_t)0x2A38),
      NIMBLE_PROPERTY::READ
    );
    if (pBodySensorLocation) {
      uint8_t location = 0x01; // Chest
      pBodySensorLocation->setValue(&location, 1);
    }

    Serial.println("✅ Heart Rate Server created - Ready to broadcast HR data");
  } catch (...) {
    Serial.println("❌ HR Server initialization failed");
  }
}

// ============================================================================
// ADVERTISING
// ============================================================================

void BLEAdvertising_init() {
  Serial.println("Starting BLE advertising...");

  try {
    NimBLEAdvertising* adv = pServer->getAdvertising();
    adv->stop();

    // Primary advertising data
    NimBLEAdvertisementData advData;
    advData.setName(storedGlobals.BLE_DEVICE_NAME.c_str());
    advData.addServiceUUID(FTMS_SERVICE_UUID);
    advData.setFlags(0x06);
    advData.setAppearance(0x0441); // Running Walking Sensor: In-Shoe (Garmin Foot Pod)
    adv->setAdvertisementData(advData);

    // Scan response data
    NimBLEAdvertisementData scanData;
    scanData.addServiceUUID(RSC_SERVICE_UUID);
    if (pHRMeasurement) {
      scanData.addServiceUUID(NimBLEUUID((uint16_t)0x180D));
    }
    adv->setScanResponseData(scanData);

    adv->start();

    Serial.println("🚀 BLE Services Started:");
    Serial.println("   FTMS (0x1826) - Training control & data");
    Serial.println("   RSC (0x1814) - Cadence transmission");
    if (pHRMeasurement) {
      Serial.println("   Heart Rate (0x180D) - HR broadcast from belt");
    }
    Serial.println("✅ Ready for MyWoosh/Zwift connection!");
  } catch (...) {
    Serial.println("❌ BLE advertising failed with unknown error");
  }
}
