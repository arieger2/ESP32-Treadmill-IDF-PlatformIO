#include <Arduino.h>
#include <driver/gpio.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"

extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;

// Forward declaration from setup module
extern String saveSettings();

// ============================================================================
// MULTI-POINT CALIBRATION: Measure non-linear treadmill speed response
// ============================================================================

#define MAX_CALIBRATION_SAMPLES 8

struct CalibrationSample {
  float startSpeed_kmh;
  float endSpeed_kmh;
  float avgRate_kmh_per_s;
  uint32_t pressDuration_ms;
};

struct CalibrationData {
  bool isCalibrated;
  uint8_t upSampleCount;
  uint8_t downSampleCount;
  CalibrationSample upSamples[MAX_CALIBRATION_SAMPLES];
  CalibrationSample downSamples[MAX_CALIBRATION_SAMPLES];
};

// Global calibration data
static CalibrationData calData = {false, 0, 0, {}, {}};

// Checkpoints for continuous calibration (km/h)
static const float UP_CHECKPOINTS[] = {3.0f, 4.5f, 6.0f, 7.5f, 9.0f, 10.5f, 12.0f};
static const uint8_t UP_CHECKPOINT_COUNT = sizeof(UP_CHECKPOINTS) / sizeof(UP_CHECKPOINTS[0]);

static const float DOWN_CHECKPOINTS[] = {10.5f, 9.0f, 7.5f, 6.0f, 4.5f, 3.0f, 2.0f};
static const uint8_t DOWN_CHECKPOINT_COUNT = sizeof(DOWN_CHECKPOINTS) / sizeof(DOWN_CHECKPOINTS[0]);

enum CalibrationState {
  CAL_IDLE,
  CAL_STARTING_UP,             // Initial delay before speed up test
  CAL_PRESSING_UP_CONTINUOUS,  // Pressing UP continuously
  CAL_UP_AT_CHECKPOINT,        // Paused at checkpoint to measure
  CAL_UP_FINISHING,            // Reached end of UP checkpoints
  CAL_STARTING_DOWN,           // Delay before speed down test
  CAL_PRESSING_DOWN_CONTINUOUS,// Pressing DOWN continuously
  CAL_DOWN_AT_CHECKPOINT,      // Paused at checkpoint to measure
  CAL_DOWN_FINISHING,          // Reached end of DOWN checkpoints
  CAL_DONE,
  CAL_ERROR
};

static CalibrationState calState = CAL_IDLE;
static uint32_t calStateChangeTime = 0;
static String calMessage = "";

// Checkpoint tracking
static uint8_t currentCheckpointIndex = 0;
static float checkpointStartSpeed = 0;
static uint32_t checkpointStartTime = 0;
static uint32_t checkpointPauseStart = 0;

// Overall calibration tracking
static float calInitialSpeed = 0;

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
    Serial.println("Calibration failed: Treadmill not running");
    return;
  }

  // Reset calibration data
  calData.isCalibrated = false;
  calData.upSampleCount = 0;
  calData.downSampleCount = 0;

  // Reset state for new calibration
  calState = CAL_STARTING_UP;
  calInitialSpeed = metrics.mps * 3.6f;  // Convert to km/h
  currentCheckpointIndex = 0;
  checkpointStartSpeed = 0;
  checkpointStartTime = 0;
  calStateChangeTime = millis();
  calMessage = "Multi-point calibration started - continuous UP test";

  Serial.printf("\n========================================\n");
  Serial.printf("Multi-Point Calibration Started\n");
  Serial.printf("Initial speed: %.2f km/h\n", calInitialSpeed);
  Serial.printf("UP checkpoints: %d, DOWN checkpoints: %d\n", UP_CHECKPOINT_COUNT, DOWN_CHECKPOINT_COUNT);
  Serial.printf("========================================\n");
}

// Helper function to get interpolated rate for a specific speed
float getInterpolatedRateForSpeed(float currentSpeed_kmh, bool speedUp) {
  const CalibrationSample* samples = speedUp ? calData.upSamples : calData.downSamples;
  uint8_t count = speedUp ? calData.upSampleCount : calData.downSampleCount;

  // If not calibrated, fall back to global average
  if (!calData.isCalibrated || count == 0) {
    return speedUp ? storedGlobals.SPEED_UP_RATE : storedGlobals.SPEED_DOWN_RATE;
  }

  // Find the appropriate segment
  for (uint8_t i = 0; i < count; i++) {
    float segStart = samples[i].startSpeed_kmh;
    float segEnd = samples[i].endSpeed_kmh;

    // Check if current speed is within this segment
    if ((speedUp && currentSpeed_kmh >= segStart && currentSpeed_kmh <= segEnd) ||
        (!speedUp && currentSpeed_kmh <= segStart && currentSpeed_kmh >= segEnd)) {
      return samples[i].avgRate_kmh_per_s;
    }
  }

  // If speed is below first checkpoint, use first segment rate
  if ((speedUp && currentSpeed_kmh < samples[0].startSpeed_kmh) ||
      (!speedUp && currentSpeed_kmh > samples[0].startSpeed_kmh)) {
    return samples[0].avgRate_kmh_per_s;
  }

  // If speed is above last checkpoint, use last segment rate
  if ((speedUp && currentSpeed_kmh > samples[count-1].endSpeed_kmh) ||
      (!speedUp && currentSpeed_kmh < samples[count-1].endSpeed_kmh)) {
    return samples[count-1].avgRate_kmh_per_s;
  }

  // Fallback to average rate
  float avgRate = 0;
  for (uint8_t i = 0; i < count; i++) {
    avgRate += samples[i].avgRate_kmh_per_s;
  }
  return avgRate / count;
}

// Non-blocking calibration state machine - call from main loop
void updateCalibration() {
  if (calState == CAL_IDLE || calState == CAL_DONE || calState == CAL_ERROR) {
    return;
  }

  // Abort if workout stops during calibration
  if (!metrics.isRunning) {
    // Cleanup: release pins if we were pressing
    if (calState == CAL_PRESSING_UP_CONTINUOUS || calState == CAL_UP_AT_CHECKPOINT) {
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 1);  // Release relay
    }
    if (calState == CAL_PRESSING_DOWN_CONTINUOUS || calState == CAL_DOWN_AT_CHECKPOINT) {
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 1);  // Release relay
    }
    calState = CAL_ERROR;
    calMessage = "Error: Workout stopped during calibration";
    Serial.println("Calibration aborted: Workout stopped");
    return;
  }

  uint32_t now = millis();
  uint32_t elapsed = now - calStateChangeTime;
  float currentSpeed = metrics.mps * 3.6f;

  // ===== SPEED UP TEST (CONTINUOUS WITH CHECKPOINTS) =====
  if (calState == CAL_STARTING_UP) {
    // Wait 1 second for initial speed measurement to stabilize
    if (elapsed > 1000) {
      calState = CAL_PRESSING_UP_CONTINUOUS;
      calStateChangeTime = now;
      checkpointStartSpeed = currentSpeed;
      checkpointStartTime = now;
      currentCheckpointIndex = 0;
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 0);  // Start pressing (relay active)
      Serial.printf("[CAL UP] Starting continuous press from %.1f km/h\n", currentSpeed);
      Serial.printf("[CAL UP] Target checkpoint: %.1f km/h\n", UP_CHECKPOINTS[0]);
    }
  }
  else if (calState == CAL_PRESSING_UP_CONTINUOUS) {
    // Check if we reached the next checkpoint
    if (currentCheckpointIndex < UP_CHECKPOINT_COUNT &&
        currentSpeed >= UP_CHECKPOINTS[currentCheckpointIndex]) {

      // Release button and pause for 1 second at checkpoint
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 1);  // Stop pressing

      // Calculate rate for this segment
      uint32_t pressDuration = now - checkpointStartTime;
      float speedChange = currentSpeed - checkpointStartSpeed;
      float rate = (pressDuration > 0) ? (speedChange / (pressDuration / 1000.0f)) : 0;

      // Store sample
      CalibrationSample& sample = calData.upSamples[calData.upSampleCount];
      sample.startSpeed_kmh = checkpointStartSpeed;
      sample.endSpeed_kmh = currentSpeed;
      sample.avgRate_kmh_per_s = rate;
      sample.pressDuration_ms = pressDuration;
      calData.upSampleCount++;

      Serial.printf("[CAL UP] Checkpoint %d reached: %.1f -> %.1f km/h in %.1f s, rate = %.3f km/h/s\n",
                    currentCheckpointIndex + 1,
                    checkpointStartSpeed, currentSpeed,
                    pressDuration / 1000.0f, rate);

      // Move to checkpoint pause state
      calState = CAL_UP_AT_CHECKPOINT;
      calStateChangeTime = now;
      checkpointPauseStart = now;
      currentCheckpointIndex++;
    }
    // Safety: stop at 12.5 km/h or after 15 seconds
    else if (currentSpeed >= 12.5f || elapsed > 15000) {
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 1);  // Stop pressing
      calState = CAL_UP_FINISHING;
      calStateChangeTime = now;
      Serial.printf("[CAL UP] Finished at %.1f km/h, captured %d samples\n",
                    currentSpeed, calData.upSampleCount);
    }
  }
  else if (calState == CAL_UP_AT_CHECKPOINT) {
    // Pause for 1 second at checkpoint
    if (now - checkpointPauseStart >= 1000) {
      // Check if we have more checkpoints
      if (currentCheckpointIndex < UP_CHECKPOINT_COUNT) {
        // Resume pressing for next checkpoint
        checkpointStartSpeed = currentSpeed;
        checkpointStartTime = now;
        calState = CAL_PRESSING_UP_CONTINUOUS;
        calStateChangeTime = now;
        gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 0);  // Resume pressing
        Serial.printf("[CAL UP] Resuming press, target: %.1f km/h\n", UP_CHECKPOINTS[currentCheckpointIndex]);
      } else {
        // All checkpoints reached
        calState = CAL_UP_FINISHING;
        calStateChangeTime = now;
        Serial.printf("[CAL UP] All checkpoints reached, captured %d samples\n", calData.upSampleCount);
      }
    }
  }
  else if (calState == CAL_UP_FINISHING) {
    // Wait 2 seconds for stabilization, then start DOWN test
    if (elapsed > 2000) {
      // Calculate average UP rate for fallback
      float avgUpRate = 0;
      for (uint8_t i = 0; i < calData.upSampleCount; i++) {
        avgUpRate += calData.upSamples[i].avgRate_kmh_per_s;
      }
      if (calData.upSampleCount > 0) {
        avgUpRate /= calData.upSampleCount;
        storedGlobals.SPEED_UP_RATE = avgUpRate;
        Serial.printf("[CAL UP] Average rate: %.3f km/h/s\n", avgUpRate);
      }

      calState = CAL_STARTING_DOWN;
      calStateChangeTime = now;
      calMessage = "UP test done - starting DOWN test";
      Serial.println("\n[CAL DOWN] Starting speed DOWN test...");
    }
  }

  // ===== SPEED DOWN TEST (CONTINUOUS WITH CHECKPOINTS) =====
  else if (calState == CAL_STARTING_DOWN) {
    // Wait 1 second before pressing down
    if (elapsed > 1000) {
      calState = CAL_PRESSING_DOWN_CONTINUOUS;
      calStateChangeTime = now;
      checkpointStartSpeed = currentSpeed;
      checkpointStartTime = now;
      currentCheckpointIndex = 0;
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 0);  // Start pressing (relay active)
      Serial.printf("[CAL DOWN] Starting continuous press from %.1f km/h\n", currentSpeed);
      Serial.printf("[CAL DOWN] Target checkpoint: %.1f km/h\n", DOWN_CHECKPOINTS[0]);
    }
  }
  else if (calState == CAL_PRESSING_DOWN_CONTINUOUS) {
    // Check if we reached the next checkpoint
    if (currentCheckpointIndex < DOWN_CHECKPOINT_COUNT &&
        currentSpeed <= DOWN_CHECKPOINTS[currentCheckpointIndex]) {

      // Release button and pause for 1 second at checkpoint
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 1);  // Stop pressing

      // Calculate rate for this segment
      uint32_t pressDuration = now - checkpointStartTime;
      float speedChange = checkpointStartSpeed - currentSpeed;  // DOWN: start > end
      float rate = (pressDuration > 0) ? (speedChange / (pressDuration / 1000.0f)) : 0;

      // Store sample
      CalibrationSample& sample = calData.downSamples[calData.downSampleCount];
      sample.startSpeed_kmh = checkpointStartSpeed;
      sample.endSpeed_kmh = currentSpeed;
      sample.avgRate_kmh_per_s = rate;
      sample.pressDuration_ms = pressDuration;
      calData.downSampleCount++;

      Serial.printf("[CAL DOWN] Checkpoint %d reached: %.1f -> %.1f km/h in %.1f s, rate = %.3f km/h/s\n",
                    currentCheckpointIndex + 1,
                    checkpointStartSpeed, currentSpeed,
                    pressDuration / 1000.0f, rate);

      // Move to checkpoint pause state
      calState = CAL_DOWN_AT_CHECKPOINT;
      calStateChangeTime = now;
      checkpointPauseStart = now;
      currentCheckpointIndex++;
    }
    // Safety: stop at 1.5 km/h or after 15 seconds
    else if (currentSpeed <= 1.5f || elapsed > 15000) {
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 1);  // Stop pressing
      calState = CAL_DOWN_FINISHING;
      calStateChangeTime = now;
      Serial.printf("[CAL DOWN] Finished at %.1f km/h, captured %d samples\n",
                    currentSpeed, calData.downSampleCount);
    }
  }
  else if (calState == CAL_DOWN_AT_CHECKPOINT) {
    // Pause for 1 second at checkpoint
    if (now - checkpointPauseStart >= 1000) {
      // Check if we have more checkpoints
      if (currentCheckpointIndex < DOWN_CHECKPOINT_COUNT) {
        // Resume pressing for next checkpoint
        checkpointStartSpeed = currentSpeed;
        checkpointStartTime = now;
        calState = CAL_PRESSING_DOWN_CONTINUOUS;
        calStateChangeTime = now;
        gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 0);  // Resume pressing
        Serial.printf("[CAL DOWN] Resuming press, target: %.1f km/h\n", DOWN_CHECKPOINTS[currentCheckpointIndex]);
      } else {
        // All checkpoints reached
        calState = CAL_DOWN_FINISHING;
        calStateChangeTime = now;
        Serial.printf("[CAL DOWN] All checkpoints reached, captured %d samples\n", calData.downSampleCount);
      }
    }
  }
  else if (calState == CAL_DOWN_FINISHING) {
    // Wait 2 seconds for stabilization, then finalize
    if (elapsed > 2000) {
      // Calculate average DOWN rate for fallback
      float avgDownRate = 0;
      for (uint8_t i = 0; i < calData.downSampleCount; i++) {
        avgDownRate += calData.downSamples[i].avgRate_kmh_per_s;
      }
      if (calData.downSampleCount > 0) {
        avgDownRate /= calData.downSampleCount;
        storedGlobals.SPEED_DOWN_RATE = avgDownRate;
        Serial.printf("[CAL DOWN] Average rate: %.3f km/h/s\n", avgDownRate);
      }

      // Mark calibration as complete
      calData.isCalibrated = true;

      // Save to NVS
      Serial.printf("\n========================================\n");
      Serial.printf("Multi-Point Calibration Complete!\n");
      Serial.printf("UP samples: %d, DOWN samples: %d\n", calData.upSampleCount, calData.downSampleCount);
      Serial.printf("Average UP rate: %.3f km/h/s\n", storedGlobals.SPEED_UP_RATE);
      Serial.printf("Average DOWN rate: %.3f km/h/s\n", storedGlobals.SPEED_DOWN_RATE);
      Serial.printf("========================================\n");

      String result = saveSettings();
      if (result == "") {
        calState = CAL_DONE;
        calMessage = String("Success! UP: ") + String(storedGlobals.SPEED_UP_RATE, 3) +
                    " km/h/s (" + String(calData.upSampleCount) + " samples), DOWN: " +
                    String(storedGlobals.SPEED_DOWN_RATE, 3) + " km/h/s (" +
                    String(calData.downSampleCount) + " samples)";
        Serial.println("[CAL] Multi-point calibration saved successfully to NVS!");
      } else {
        calState = CAL_ERROR;
        calMessage = String("Error saving: ") + result;
      }
    }
  }
}

String getCalibrationStatus() {
  String stateStr = "";
  switch (calState) {
    case CAL_IDLE:                      stateStr = "idle"; break;
    case CAL_STARTING_UP:               stateStr = "starting_up"; break;
    case CAL_PRESSING_UP_CONTINUOUS:    stateStr = "pressing_up"; break;
    case CAL_UP_AT_CHECKPOINT:          stateStr = "up_checkpoint"; break;
    case CAL_UP_FINISHING:              stateStr = "up_finishing"; break;
    case CAL_STARTING_DOWN:             stateStr = "starting_down"; break;
    case CAL_PRESSING_DOWN_CONTINUOUS:  stateStr = "pressing_down"; break;
    case CAL_DOWN_AT_CHECKPOINT:        stateStr = "down_checkpoint"; break;
    case CAL_DOWN_FINISHING:            stateStr = "down_finishing"; break;
    case CAL_DONE:                      stateStr = "done"; break;
    case CAL_ERROR:                     stateStr = "error"; break;
    default:                            stateStr = "unknown"; break;
  }

  String json = "{";
  json += "\"state\":\"" + stateStr + "\"";
  json += ",\"message\":\"" + calMessage + "\"";
  json += ",\"isCalibrated\":" + String(calData.isCalibrated ? "true" : "false");
  json += ",\"upSamples\":" + String(calData.upSampleCount);
  json += ",\"downSamples\":" + String(calData.downSampleCount);
  json += ",\"speedUpRate\":" + String(storedGlobals.SPEED_UP_RATE, 3);
  json += ",\"speedDownRate\":" + String(storedGlobals.SPEED_DOWN_RATE, 3);
  json += ",\"checkpoint\":" + String(currentCheckpointIndex);
  json += "}";
  return json;
}
