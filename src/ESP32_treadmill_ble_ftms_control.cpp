// ============================================================================
// BLE FTMS Control Point - Command handling callbacks
// ============================================================================

#include "ESP32_treadmill_tacho_ble.h"
#include "ESP32_treadmill_tacho_workout.h"

// ============================================================================
// FTMS CONTROL POINT CALLBACKS
// ============================================================================

class FTMSControlPointCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& /*connInfo*/) override {
    bleData.clientConnected = true;

    std::string payload = pCharacteristic->getValue();
    const uint8_t* data = reinterpret_cast<const uint8_t*>(payload.data());
    size_t length = payload.size();

    if (!data || length == 0) {
      Serial.println("FTMS: Empty command received");
      return;
    }

    uint8_t opCode = data[0];
    if (opCode == 0x80) {
      Serial.println("FTMS: Ignoring response packet");
      Serial.print("FTMS: Client wrote a Response frame (unexpected): ");
      for (size_t i = 0; i < length; ++i) Serial.printf("%02X ", data[i]);
      Serial.println();
      return;
    }

    Serial.printf("FTMS Command: 0x%02X\r\n", opCode);

    uint8_t response[3] = {0x80, opCode, FTMS_SUCCESS};

    switch (opCode) {
      case FTMS_REQUEST_CONTROL:
        Serial.println("FTMS: Control granted");
        metrics.controlRequested = true;
        sendFTMSStatusUpdate(0x01);
        break;

      case FTMS_RESET:
        Serial.println("FTMS: Reset workout");
        resetWorkout();
        sendFTMSStatusUpdate(0x02);
        break;

      case FTMS_SET_TARGET_SPEED:
        if (length >= 3) {
          uint16_t speed = (uint16_t)(data[1] | (data[2] << 8));
          metrics.targetSpeed = speed * 0.01f;
          if (testdata) storedGlobals.TESTDATA_FREQ_MS = (uint32_t)(390.0f / metrics.targetSpeed);
          Serial.printf("FTMS: Target speed %.1f km/h\r\n", metrics.targetSpeed);
        } else {
          response[2] = FTMS_INVALID_PARAMETER;
        }
        break;

      case FTMS_SET_TARGET_INCLINATION:
        if (length >= 3) {
          int16_t incline = (int16_t)(data[1] | (data[2] << 8));
          metrics.targetInclination  = incline * 0.1f;
          metrics.currentInclination = metrics.targetInclination;
          Serial.printf("FTMS: Target incline %.1f%%\r\n", metrics.targetInclination);
        } else {
          response[2] = FTMS_INVALID_PARAMETER;
        }
        break;

      case FTMS_START_RESUME:
        Serial.println("FTMS: Start/Resume workout");
        if (!metrics.isRunning) {
          metrics.isRunning = true;
          metrics.isPaused = false;
          metrics.sessionStartTime = millis();
          sendFTMSStatusUpdate(0x04);
        } else if (metrics.isPaused) {
          metrics.isPaused = false;
          sendFTMSStatusUpdate(0x04);
        }
        break;

      case FTMS_STOP_PAUSE:
        if (length >= 2) {
          if (data[1] == 0x01) {
            Serial.println("FTMS: Stop workout");
            resetWorkout();
            sendFTMSStatusUpdate(0x05);
          } else if (data[1] == 0x02) {
            Serial.println("FTMS: Pause workout");
            metrics.isPaused = true;
            sendFTMSStatusUpdate(0x06);
          }
        }
        break;

      default:
        Serial.printf("FTMS: Unsupported command 0x%02X\r\n", opCode);
        response[2] = FTMS_NOT_SUPPORTED;
        break;
    }

    // Respond via Indicate (preferred) or Notify (fallback)
    pFTMSControlPoint->setValue(response, 3);
    if (!pFTMSControlPoint->indicate()) {
      pFTMSControlPoint->notify();
    }
  }

  void onRead(NimBLECharacteristic* /*chr*/, NimBLEConnInfo& /*connInfo*/) override {
    // optional
  }

  void onSubscribe(NimBLECharacteristic* /*chr*/, NimBLEConnInfo& /*ci*/, uint16_t subValue) override {
    Serial.printf("FTMS CP subscribe: 0x%04X (1=notify,2=indicate,3=both)\r\n", subValue);
  }

  void onStatus(NimBLECharacteristic* /*chr*/, int code) override {
    if (code) Serial.printf("FTMS CP indicate/notify status: %d\r\n", code);
  }
};

// Singleton instance
static FTMSControlPointCallbacks ftmsControlPointCallbacks;

NimBLECharacteristicCallbacks* getFTMSControlPointCallbacks() {
  return &ftmsControlPointCallbacks;
}
