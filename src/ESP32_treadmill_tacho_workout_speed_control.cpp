#include <Arduino.h>
#include <driver/gpio.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"
#include "ESP32_treadmill_tacho_bootlog.h"

extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;

// ===========================================================================
// REALTIME SPEED FILTER - Outlier rejection with minimal lag
// NOTE: Filter is bypassed during SC_PRESSING to see real speed
// ===========================================================================
float getCurrentSpeedRaw() {
  static portMUX_TYPE metrics_spinlock = portMUX_INITIALIZER_UNLOCKED;
  float speed;
  
  portENTER_CRITICAL(&metrics_spinlock);
  speed = (metrics.mps + metrics.mpsOffset) * 3.6f;
  portEXIT_CRITICAL(&metrics_spinlock);
  
  return speed;
}

float getCurrentSpeedWithOutlierFilter() {
  static float last_mps = 0;         // last accepted speed in m/s
  static float prev_raw_mps = -1;    // previous raw reading to detect new sensor data
  static uint32_t lastTime = 0;

  const float raw_mps = (metrics.mps + metrics.mpsOffset);
  const uint32_t now = millis();
  const uint32_t dt = now - lastTime;

  if (last_mps == 0 || dt > 2000) {
    last_mps = raw_mps;
    prev_raw_mps = raw_mps;
    lastTime = now;
    return raw_mps * 3.6f;
  }

  // Only apply filter when sensor delivers a new reading (~every 200ms)
  // Between updates raw_mps stays the same — just return last accepted
  if (fabsf(raw_mps - prev_raw_mps) < 0.001f) {
    return last_mps * 3.6f;
  }
  prev_raw_mps = raw_mps;

  float dt_s = dt / 1000.0f;

  // Physics-based prediction using acceleration (m/s²) and jerk (m/s³)
  float predicted_mps = last_mps
                      + metrics.acceleration * dt_s
                      + 0.5f * metrics.jerk * dt_s * dt_s;

  // Tolerance: sensor noise floor + scaled by acceleration magnitude
  // 0.08 m/s ≈ 0.3 km/h sensor resolution
  // During acceleration/deceleration, widen tolerance proportionally
  float tolerance_mps = 0.08f + fabsf(metrics.acceleration) * dt_s * 2.0f;

  float deviation = fabsf(raw_mps - predicted_mps);

  float valid_mps;
  if (deviation > tolerance_mps) {
    // Outlier: clamp toward prediction
    valid_mps = predicted_mps + ((raw_mps > predicted_mps) ? tolerance_mps : -tolerance_mps);
    if (valid_mps < 0) valid_mps = 0;
    Serial.printf("[Speed Filter] Outlier: %.2f -> %.2f m/s (predicted %.2f, tolerance %.3f, clamped to %.2f)\n",
                  last_mps, raw_mps, predicted_mps, tolerance_mps, valid_mps);
  } else {
    valid_mps = raw_mps;
  }

  last_mps = valid_mps;
  lastTime = now;
  return valid_mps * 3.6f;
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

  // Get speed - use RAW during pressing to see real speed immediately
  // Use filtered speed during IDLE/STABILIZING to avoid jitter
  const float current_kmh = (state == SC_PRESSING) ? getCurrentSpeedRaw() : getCurrentSpeedWithOutlierFilter();
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

        // Don't press if treadmill is already accelerating toward target on its own
        if (diff > 0 && metrics.acceleration > 0.06f && metrics.jerk >= 0.0f) {
          return;  // Already accelerating — let it coast
        }
        if (diff < 0 && metrics.acceleration < -0.06f && metrics.jerk <= 0.0f) {
          return;  // Already decelerating — let it coast
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

        // Use interpolated rate based on current speed for non-linear response
        float rate = getInterpolatedRateForSpeed(current_kmh, speedUp);
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

      // CRITICAL: Early release to compensate for mechanical inertia
      // Release BEFORE target to account for overshoot/undershoot
      // Overshoot compensation: ~2 km/h for UP, ~1 km/h for DOWN
      const float OVERSHOOT_COMPENSATION_UP = 0.2f;    // km/h before target
      const float OVERSHOOT_COMPENSATION_DOWN = 0.8f;  // km/h before target

      // Safety: abort press if treadmill decelerates against our direction (mechanical issue)
      if (speedUp && metrics.acceleration < -0.06f && metrics.jerk <= 0.0f) {
        gpio_set_level((gpio_num_t)activePin, 1);
        Serial.printf("[Speed Control] Blocking UP: decel %.4f m/s², jerk %.4f m/s³ after %u ms\n",
                      metrics.acceleration, metrics.jerk, elapsed);
        stabilizationStartTime = now_ms;
        state = SC_STABILIZING;
        activePin = 0;
      }
      else if (speedUp && current_kmh >= (targetSpeed_kmh - OVERSHOOT_COMPENSATION_UP)) {
        gpio_set_level((gpio_num_t)activePin, 1);
        Serial.printf("[Speed Control] Early release at %.1f km/h after %u ms (target %.1f, compensating %.1f km/h overshoot)\n",
                      current_kmh, elapsed, targetSpeed_kmh, OVERSHOOT_COMPENSATION_UP);
        stabilizationStartTime = now_ms;
        state = SC_STABILIZING;
        activePin = 0;
      }
      else if (!speedUp && current_kmh <= (targetSpeed_kmh + OVERSHOOT_COMPENSATION_DOWN)) {
        gpio_set_level((gpio_num_t)activePin, 1);
        Serial.printf("[Speed Control] Early release at %.1f km/h after %u ms (target %.1f, compensating %.1f km/h undershoot)\n",
                      current_kmh, elapsed, targetSpeed_kmh, OVERSHOOT_COMPENSATION_DOWN);
        stabilizationStartTime = now_ms;
        state = SC_STABILIZING;
        activePin = 0;
      }
      // Check if press duration reached
      else if (elapsed >= targetPressDuration) {
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
  if (pin == 0) return;
  
  if (pin == storedGlobals.SPEED_UP_PIN ) { 
      //logAppendPrintf("[writePress] SPEED_UP blocked: acceleration=%.4f m/s2 (threshold +-0.042)\n", metrics.acceleration);
      if (pressed && !speedUpBusy && !speedDownBusy ) {   // interlock: block if DOWN active // block if the treadmill is not abording. ±0.042 m/s² = 0.15km/h per second
          gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
          speedUpBusy = true;
          esp_timer_stop(speedUpTimer);
          esp_timer_start_once(speedUpTimer, PULSE_US);
      } 
  } else if (pin == storedGlobals.SPEED_DOWN_PIN) {
      if (pressed && !speedDownBusy && !speedUpBusy) {   // interlock: block if UP active
          gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
          speedDownBusy = true;
          esp_timer_stop(speedDownTimer);
          esp_timer_start_once(speedDownTimer, PULSE_US);
      }
  } else if (pin == storedGlobals.INCLINE_UP_PIN) {
      if (pressed && !inclineUpBusy && !inclineDownBusy) {   // interlock: block if DOWN active
          gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
          inclineUpBusy = true;
          esp_timer_stop(inclineUpTimer);
          esp_timer_start_once(inclineUpTimer, PULSE_US);
      }
  } else if (pin == storedGlobals.INCLINE_DOWN_PIN) {
      if (pressed && !inclineDownBusy && !inclineUpBusy) {   // interlock: block if UP active
          gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
          inclineDownBusy = true;
          esp_timer_stop(inclineDownTimer);
          esp_timer_start_once(inclineDownTimer, PULSE_US);
      }
  } else {
      Serial.println("ERROR PIN not defined");
  }
}

