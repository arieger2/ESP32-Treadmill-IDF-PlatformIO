#include <Arduino.h>
#include <driver/gpio.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"

extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;

// Forward declaration from setup module
extern String saveSettings();

// ============================================================================
// CALIBRATION: Measure treadmill speed response
// ============================================================================
enum CalibrationState {
  CAL_IDLE,
  CAL_STARTING_UP,      // Initial delay before speed up test
  CAL_PRESSING_UP,      // Pressing speed up button
  CAL_WAITING_UP,       // Waiting for stabilization after speed up
  CAL_STARTING_DOWN,    // Delay before speed down test
  CAL_PRESSING_DOWN,    // Pressing speed down button
  CAL_WAITING_DOWN,     // Waiting for stabilization after speed down
  CAL_DONE,
  CAL_ERROR
};

static CalibrationState calState = CAL_IDLE;
static float calStartSpeed = 0;      // Speed before UP test
static float calMidSpeed = 0;        // Speed at UP release (for rate calc)
static float calDownStartSpeed = 0;  // Speed before DOWN test (stabilized after UP)
static float calEndSpeed = 0;        // Speed at DOWN release (for rate calc)
static uint32_t calStateChangeTime = 0;
static uint32_t calUpPressTime_ms = 0;   // Actual UP press duration
static uint32_t calDownPressTime_ms = 0; // Actual DOWN press duration
static String calMessage = "";

void startSpeedCalibration() {
  // Prevent starting calibration if one is already running
  if (calState != CAL_IDLE && calState != CAL_DONE && calState != CAL_ERROR) {
    calMessage = "Error: Calibration already in progress";
    Serial.println("Calibration failed: Already running");
    return;
  }

  if (!metrics.isRunning) {
    calState = CAL_ERROR;
    calMessage = "Error: Treadmill must be running";
    Serial.println("Calibration failed: No workout running");
    return;
  }

  // Reset state for new calibration
  calState = CAL_STARTING_UP;
  calStartSpeed = metrics.mps * 3.6f;  // Convert to km/h
  calMidSpeed = 0;  // Speed at release for UP test
  calEndSpeed = 0;  // Speed at release for DOWN test
  calUpPressTime_ms = 0;
  calDownPressTime_ms = 0;
  calStateChangeTime = millis();
  calMessage = "Calibration started - testing speed UP";

  Serial.printf("Starting calibration: initial speed = %.2f km/h\n", calStartSpeed);
}

// Non-blocking calibration state machine - call from main loop
void updateCalibration() {
  if (calState == CAL_IDLE || calState == CAL_DONE || calState == CAL_ERROR) {
    return;
  }

  // Abort if workout stops during calibration
  if (!metrics.isRunning) {
    // Cleanup: release pins if we were pressing
    if (calState == CAL_PRESSING_UP) {
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 1);  // Release relay
    }
    if (calState == CAL_PRESSING_DOWN) {
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 1);  // Release relay
    }
    calState = CAL_ERROR;
    calMessage = "Error: Workout stopped during calibration";
    Serial.println("Calibration aborted: Workout stopped");
    return;
  }

  uint32_t now = millis();
  uint32_t elapsed = now - calStateChangeTime;

  // ===== SPEED UP TEST =====
  if (calState == CAL_STARTING_UP) {
    // Wait 1 second for initial speed measurement to stabilize
    if (elapsed > 1000) {
      calState = CAL_PRESSING_UP;
      calStateChangeTime = now;
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 0);  // Start pressing (relay active)
      Serial.println("Calibration UP: Pressing speed up (max 5s or 10 km/h)...");
    }
  }
  else if (calState == CAL_PRESSING_UP) {
    float currentSpeed = metrics.mps * 3.6f;
    const float CAL_MAX_SPEED_KMH = 10.0f;  // Safety: stop at 10 km/h
    const uint32_t CAL_MAX_PRESS_MS = 5000; // Max 5 seconds

    // Stop if reached max speed OR max time
    if (elapsed >= CAL_MAX_PRESS_MS || currentSpeed >= CAL_MAX_SPEED_KMH) {
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 1);  // Stop pressing (relay inactive)
      calUpPressTime_ms = elapsed;  // Store actual press time
      calMidSpeed = currentSpeed;   // Store speed AT RELEASE (not after waiting!)
      calState = CAL_WAITING_UP;
      calStateChangeTime = now;
      Serial.printf("Calibration UP: Stopped after %u ms at %.1f km/h, waiting for stabilization...\n",
                    elapsed, currentSpeed);
    }
  }
  else if (calState == CAL_WAITING_UP) {
    // Wait 5 seconds for speed to stabilize, then calculate rate
    if (elapsed > 5000) {
      // Use speed at release moment (calMidSpeed), not current speed
      float speedIncrease = calMidSpeed - calStartSpeed;
      // Use actual press time for rate calculation
      float pressSec = calUpPressTime_ms / 1000.0f;
      float upRate = (pressSec > 0.1f) ? (speedIncrease / pressSec) : 0;

      Serial.printf("Calibration UP complete: speed %.2f -> %.2f km/h (at release), increase = %.2f km/h in %.1f s, rate = %.3f km/h/s\n",
                    calStartSpeed, calMidSpeed, speedIncrease, pressSec, upRate);

      if (upRate > 0.05f && upRate < 2.0f) {  // Sanity check
        storedGlobals.SPEED_UP_RATE = upRate;
        Serial.printf("[CAL] SPEED_UP_RATE set to: %.3f km/h/s\n", storedGlobals.SPEED_UP_RATE);

        // Now start DOWN test - record the speed AFTER UP test stabilization
        calState = CAL_STARTING_DOWN;
        calStateChangeTime = now;
        calMessage = "Speed UP test done - starting DOWN test";
        Serial.println("Starting speed DOWN test...");
      } else {
        calState = CAL_ERROR;
        calMessage = String("Error: Invalid UP rate ") + String(upRate, 3) + " km/h/s";
      }
    }
  }

  // ===== SPEED DOWN TEST =====
  else if (calState == CAL_STARTING_DOWN) {
    // Wait 1 second before pressing down, record current stable speed as DOWN start
    if (elapsed > 1000) {
      calDownStartSpeed = metrics.mps * 3.6f;  // Speed before pressing DOWN
      calState = CAL_PRESSING_DOWN;
      calStateChangeTime = now;
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 0);  // Start pressing (relay active)
      Serial.printf("Calibration DOWN: Starting at %.1f km/h, pressing speed down (max 5s or min 2 km/h)...\n", calDownStartSpeed);
    }
  }
  else if (calState == CAL_PRESSING_DOWN) {
    float currentSpeed = metrics.mps * 3.6f;
    const float CAL_MIN_SPEED_KMH = 2.0f;   // Safety: stop at ~2 km/h (above minimum)
    const uint32_t CAL_MAX_PRESS_MS = 5000; // Max 5 seconds

    // Stop if reached min speed OR max time
    if (elapsed >= CAL_MAX_PRESS_MS || currentSpeed <= CAL_MIN_SPEED_KMH) {
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 1);  // Stop pressing (relay inactive)
      calDownPressTime_ms = elapsed;  // Store actual press time
      calEndSpeed = currentSpeed;     // Store speed AT RELEASE (not after waiting!)
      calState = CAL_WAITING_DOWN;
      calStateChangeTime = now;
      Serial.printf("Calibration DOWN: Stopped after %u ms at %.1f km/h, waiting for stabilization...\n",
                    elapsed, currentSpeed);
    }
  }
  else if (calState == CAL_WAITING_DOWN) {
    // Wait 5 seconds for speed to stabilize, then calculate rate
    if (elapsed > 5000) {
      // Use calDownStartSpeed (recorded before pressing DOWN) and calEndSpeed (at release)
      float speedDecrease = calDownStartSpeed - calEndSpeed;
      // Use actual press time for rate calculation
      float pressSec = calDownPressTime_ms / 1000.0f;
      float downRate = (pressSec > 0.1f) ? (speedDecrease / pressSec) : 0;

      Serial.printf("Calibration DOWN complete: speed %.2f -> %.2f km/h (at release), decrease = %.2f km/h in %.1f s, rate = %.3f km/h/s\n",
                    calDownStartSpeed, calEndSpeed, speedDecrease, pressSec, downRate);

      if (downRate > 0.05f && downRate < 2.0f) {  // Sanity check
        storedGlobals.SPEED_DOWN_RATE = downRate;
        Serial.printf("[CAL] SPEED_DOWN_RATE set to: %.3f km/h/s\n", storedGlobals.SPEED_DOWN_RATE);
        Serial.printf("[CAL] Saving to NVS: UP=%.3f, DOWN=%.3f\n", storedGlobals.SPEED_UP_RATE, storedGlobals.SPEED_DOWN_RATE);
        String result = saveSettings();

        if (result == "") {
          calState = CAL_DONE;
          calMessage = String("Success! UP: ") + String(storedGlobals.SPEED_UP_RATE, 3) +
                      " km/h/s, DOWN: " + String(storedGlobals.SPEED_DOWN_RATE, 3) + " km/h/s";
          Serial.println("[CAL] Calibration saved successfully to NVS!");
        } else {
          calState = CAL_ERROR;
          calMessage = String("Error saving: ") + result;
        }
      } else {
        calState = CAL_ERROR;
        calMessage = String("Error: Invalid DOWN rate ") + String(downRate, 3) + " km/h/s";
      }
    }
  }
}

String getCalibrationStatus() {
  String stateStr = "";
  switch (calState) {
    case CAL_IDLE:           stateStr = "idle"; break;
    case CAL_STARTING_UP:    stateStr = "starting_up"; break;
    case CAL_PRESSING_UP:    stateStr = "pressing_up"; break;
    case CAL_WAITING_UP:     stateStr = "waiting_up"; break;
    case CAL_STARTING_DOWN:  stateStr = "starting_down"; break;
    case CAL_PRESSING_DOWN:  stateStr = "pressing_down"; break;
    case CAL_WAITING_DOWN:   stateStr = "waiting_down"; break;
    case CAL_DONE:           stateStr = "done"; break;
    case CAL_ERROR:          stateStr = "error"; break;
    default:                 stateStr = "unknown"; break;
  }

  String json = "{";
  json += "\"state\":\"" + stateStr + "\"";
  json += ",\"message\":\"" + calMessage + "\"";
  json += ",\"startSpeed\":" + String(calStartSpeed, 2);
  json += ",\"midSpeed\":" + String(calMidSpeed, 2);
  json += ",\"endSpeed\":" + String(calEndSpeed, 2);
  json += ",\"speedUpRate\":" + String(storedGlobals.SPEED_UP_RATE, 3);
  json += ",\"speedDownRate\":" + String(storedGlobals.SPEED_DOWN_RATE, 3);
  json += "}";
  return json;
}
