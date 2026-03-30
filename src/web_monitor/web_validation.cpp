// ============================================================================
// Web API - Settings validation
// ============================================================================

#include "ESP32_treadmill_tacho_web.h"
#include <ArduinoJson.h>
#include <set>

bool validateSettings(const String& wifiSSID, const String& wifiPassword, const String& bleDeviceName,
    int interruptPin, int motorInterruptPin, int speedUpPin, int speedDownPin,
    int inclineUpPin, int inclineDownPin,
    uint32_t testdataFreq, long beltDistance, long debounceThreshold,
    long maxRevolutionTime, long pulsesPerRev, long motorPulsesPerRev, float motorToBeltRatio)
{
  if (wifiSSID.length() == 0 || wifiSSID.length() > 32) return false;
  if (wifiPassword.length() > 64) return false;
  if (bleDeviceName.length() == 0 || bleDeviceName.length() > 20) return false;

  auto pinOk = [](int p){ return p >= 0 && p <= 39; };
  if (!pinOk(interruptPin) || !pinOk(motorInterruptPin) || !pinOk(speedUpPin) ||
      !pinOk(speedDownPin) || !pinOk(inclineUpPin) || !pinOk(inclineDownPin)) return false;

  // Check for duplicate pins using std::set
  std::set<int> pinSet = {interruptPin, motorInterruptPin, speedUpPin, speedDownPin, inclineUpPin, inclineDownPin};
  if (pinSet.size() != 6) return false; // Duplicates found

  if (testdataFreq   < 1  || testdataFreq   > 1000) return false;

  if (beltDistance < 100 || beltDistance > 500) return false;
  if (debounceThreshold < 1 || debounceThreshold > 13) return false;  // PCNT hardware limit: max 1023 APB cycles = 12.78 µs
  if (maxRevolutionTime < 100 || maxRevolutionTime > 500000) return false;

  if (pulsesPerRev < 1 || pulsesPerRev > 128) return false;
  if (motorPulsesPerRev < 1 || motorPulsesPerRev > 256) return false;
  if (!(motorToBeltRatio > 0.01f && motorToBeltRatio < 10.0f)) return false;

  return true;
}

String extractJsonValue(const String& json, const String& key) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    Serial.printf("[JSON] Parse error: %s\r\n", error.c_str());
    return "";
  }
  if (!doc[key].is<String>() && !doc[key].is<int>()) return "";
  return doc[key].as<String>();
}
