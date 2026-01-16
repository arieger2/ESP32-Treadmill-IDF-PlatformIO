#include <Arduino.h>
#include <driver/gpio.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"

extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;

// ===========================================================================
// REALTIME SPEED FILTER - Outlier rejection with minimal lag
// ===========================================================================
float getCurrentSpeedWithOutlierFilter() {
  static float lastSpeed = 0;
  static uint32_t lastTime = 0;

  const float raw_kmh = (metrics.mps + metrics.mpsOffset) * 3.6f;
  const uint32_t now = millis();
  const uint32_t dt = now - lastTime;

  if (lastSpeed == 0 || dt > 2000) {
    // First measurement or too long ago - accept as-is
    lastSpeed = raw_kmh;
    lastTime = now;
    return raw_kmh;
  }

  // Maximum realistic change in dt milliseconds
  // Treadmill can change max ~1.5 km/h per second
  float maxChange = (dt / 1000.0f) * 1.5f;
  float actualChange = fabsf(raw_kmh - lastSpeed);

  float validSpeed;
  if (actualChange > maxChange) {
    // Outlier detected - clamp to max realistic change
    validSpeed = lastSpeed + ((raw_kmh > lastSpeed) ? maxChange : -maxChange);
    Serial.printf("[Speed Filter] Outlier: %.1f -> %.1f km/h (clamped to %.1f)\n",
                  lastSpeed, raw_kmh, validSpeed);
  } else {
    validSpeed = raw_kmh;
  }

  lastSpeed = validSpeed;
  lastTime = now;
  return validSpeed;
}

// ===========================================================================
// ADAPTIVE SPEED CONTROL - State machine with simplified button press
// ===========================================================================
enum SpeedControlState {
  SC_IDLE,
  SC_PRESSING,
  SC_STABILIZING
};

void physicalSpeedControl(float targetSpeed_kmh, float current_mps) {
  static SpeedControlState state = SC_IDLE;
  static float lastTargetSpeed = -1.0f;
  static bool wasRunning = false;
  static uint32_t pressStartTime = 0;
  static uint32_t targetPressDuration = 0;
  static uint32_t stabilizationStartTime = 0;
  static uint8_t activePin = 0;
  static bool speedUp = false;

  const uint32_t now_ms = millis();
  const float MAX_SPEED_KMH = 18.0f;
  const float MIN_SPEED_KMH = 1.6f;
  const float HYSTERESIS = 0.35f;  // Fixed hysteresis for jitter tolerance

  // Reset on treadmill start
  if (!wasRunning && metrics.isRunning && !metrics.isPaused) {
    state = SC_IDLE;
    lastTargetSpeed = -1.0f;
    activePin = 0;
  }

  wasRunning = metrics.isRunning && !metrics.isPaused;

  // Abort and cleanup if workout stops
  if (!metrics.isRunning || metrics.isPaused) {
    if (state == SC_PRESSING && activePin != 0) {
      gpio_set_level((gpio_num_t)activePin, 1);  // Release button
      Serial.println("[Speed Control] Workout stopped - releasing button");
    }
    state = SC_IDLE;
    lastTargetSpeed = -1.0f;
    activePin = 0;
    return;
  }

  // Safety: Treadmill minimum speed
  if (targetSpeed_kmh < MIN_SPEED_KMH) {
    return;
  }

  // Get filtered realtime speed (outlier rejection)
  const float current_kmh = getCurrentSpeedWithOutlierFilter();
  const float diff = targetSpeed_kmh - current_kmh;

  // State machine
  switch (state) {
    case SC_IDLE: {
      // Check if we need to adjust speed
      if (fabsf(diff) > HYSTERESIS) {
        // Don't press if already at limits
        if (diff > 0 && current_kmh >= MAX_SPEED_KMH) {
          Serial.printf("[Speed Control] At max speed %.1f km/h\n", current_kmh);
          return;
        }
        if (diff < 0 && current_kmh <= MIN_SPEED_KMH) {
          Serial.printf("[Speed Control] At min speed %.1f km/h\n", current_kmh);
          return;
        }

        // Log new target
        if (lastTargetSpeed < 0.0f || fabsf(targetSpeed_kmh - lastTargetSpeed) > 0.05f) {
          lastTargetSpeed = targetSpeed_kmh;
          Serial.printf("[Speed Control] New target: %.1f km/h, Current: %.1f km/h, Diff: %.2f km/h\n",
                        targetSpeed_kmh, current_kmh, diff);
        }

        // Calculate press duration
        speedUp = (diff > 0);
        activePin = speedUp ? storedGlobals.SPEED_UP_PIN : storedGlobals.SPEED_DOWN_PIN;

        if (activePin == 0) {
          Serial.println("[Speed Control] ERROR: PIN not configured");
          return;
        }

        float rate = speedUp ? storedGlobals.SPEED_UP_RATE : storedGlobals.SPEED_DOWN_RATE;
        if (rate < 0.1f) rate = 0.5f;  // Fallback if not calibrated

        // Calculate press time: diff / rate, subtract inertia delay
        uint32_t calcPress_ms = (uint32_t)((fabsf(diff) / rate) * 1000.0f);
        uint32_t inertiaComp_ms = storedGlobals.INERTIA_DELAY_MS;
        targetPressDuration = (calcPress_ms > inertiaComp_ms) ? (calcPress_ms - inertiaComp_ms) : 50;

        // Safety limits
        if (targetPressDuration < 50) targetPressDuration = 50;
        if (targetPressDuration > 30000) targetPressDuration = 30000;

        Serial.printf("[Speed Control] %s: %.1f -> %.1f km/h (rate=%.3f, press=%u ms)\n",
                      speedUp ? "UP" : "DOWN", current_kmh, targetSpeed_kmh,
                      rate, targetPressDuration);

        // Start pressing
        gpio_set_level((gpio_num_t)activePin, 0);  // LOW = relay active
        pressStartTime = now_ms;
        state = SC_PRESSING;
      }
      break;
    }

    case SC_PRESSING: {
      uint32_t elapsed = now_ms - pressStartTime;

      // Check if press duration reached
      if (elapsed >= targetPressDuration) {
        gpio_set_level((gpio_num_t)activePin, 1);  // HIGH = relay inactive
        Serial.printf("[Speed Control] Released after %u ms at %.1f km/h\n", elapsed, current_kmh);

        stabilizationStartTime = now_ms;
        state = SC_STABILIZING;
        activePin = 0;
      }
      // Safety checks during pressing
      else if (speedUp && current_kmh >= MAX_SPEED_KMH) {
        gpio_set_level((gpio_num_t)activePin, 1);
        Serial.printf("[Speed Control] Safety stop at max: %.1f km/h after %u ms\n", current_kmh, elapsed);
        stabilizationStartTime = now_ms;
        state = SC_STABILIZING;
        activePin = 0;
      }
      else if (!speedUp && current_kmh <= MIN_SPEED_KMH) {
        gpio_set_level((gpio_num_t)activePin, 1);
        Serial.printf("[Speed Control] Safety stop at min: %.1f km/h after %u ms\n", current_kmh, elapsed);
        stabilizationStartTime = now_ms;
        state = SC_STABILIZING;
        activePin = 0;
      }
      break;
    }

    case SC_STABILIZING: {
      uint32_t stabilizationTime = now_ms - stabilizationStartTime;

      // Wait for inertia delay before checking result
      if (stabilizationTime >= storedGlobals.INERTIA_DELAY_MS) {
        Serial.printf("[Speed Control] Stabilized at %.1f km/h (target %.1f km/h, diff %.2f km/h)\n",
                      current_kmh, targetSpeed_kmh, fabsf(targetSpeed_kmh - current_kmh));
        state = SC_IDLE;  // Next cycle will check if another press is needed
      }
      break;
    }
  }
}

// ============================================================================
// HELPER: Simple button press with GPIO
// ============================================================================
void writePress(uint8_t pin, bool pressed) {
  if (pin == 0) {
      Serial.println("ERROR PIN not defined");
  }
}

// ============================================================================
// Continuous press for calculated duration (non-blocking with yield)
// ============================================================================
void writePressForDuration(uint8_t pin, uint32_t duration_ms) {
  if (pin == 0) return;

  gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active

  // Split delay into small chunks with yield() to keep system responsive
  const uint32_t YIELD_INTERVAL_MS = 10;  // Yield every 10ms
  uint32_t elapsed = 0;

  while (elapsed < duration_ms) {
    uint32_t chunk = (duration_ms - elapsed) > YIELD_INTERVAL_MS ? YIELD_INTERVAL_MS : (duration_ms - elapsed);
    delay(chunk);
    yield();  // Let other tasks run (WiFi, Web Server, etc.)
    elapsed += chunk;
  }

  gpio_set_level((gpio_num_t)pin, 1);  // HIGH = relay inactive
}
