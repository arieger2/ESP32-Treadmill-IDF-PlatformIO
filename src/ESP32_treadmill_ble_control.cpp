#include <Arduino.h>  
#include "ESP32_treadmill_tacho_config.h"  // brings in TreadmillMetrics, storedGlobals, etc.

// Globals come from your project
extern TreadmillMetrics metrics;

// Clamp helpers (match your FTMS feature ranges: 1.0–30.0 km/h, 0–15 %)
static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline float round1(float v) {            // round to 0.1 steps like FTMS
  return floorf(v * 10.0f + 0.5f) / 10.0f;
}

// Called by the Workout Executor (or anything else)
void bleSetTreadmillSpeedKph(float kph) {
  // Safety + FTMS-compatible step
  kph = round1(clampf(kph, 1.0f, 30.0f));
  Serial.printf("speed change kmh: %.1f\r\n", kph);
  metrics.targetSpeed = kph;          // your loop already chases this with SPEED_UP/DOWN pulses
  metrics.controlRequested = true;    // enables FTMS treadmill data block in sendFTMS_BLE_Data()
  metrics.isRunning = (kph > 0.0f);
  if (metrics.sessionStartTime == 0) {
    metrics.sessionStartTime = millis();
  }
}

void bleSetTreadmillInclinePct(float percent) {
  // Safety: 0–15 % typical, rounded to 0.1
  percent = round1(clampf(percent, 0.0f, 15.0f));
  Serial.printf("incline change : %f.1\r\n", percent);
  metrics.targetInclination = percent;   // (Optional) add a chase loop similar to speed if you want HW control
  metrics.controlRequested = true;
  if (metrics.sessionStartTime == 0) {
    metrics.sessionStartTime = millis();
  }
}