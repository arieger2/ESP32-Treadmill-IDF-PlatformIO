#include <Arduino.h>
#include <driver/gpio.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"
#include "ESP32_treadmill_tacho_bootlog.h"

extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;

// ===========================================================================
// RAW SPEED - Thread-safe read from sensor (200ms measurement window)
// ===========================================================================
float getCurrentSpeedRaw() {
  static portMUX_TYPE metrics_spinlock = portMUX_INITIALIZER_UNLOCKED;
  float speed;
  
  portENTER_CRITICAL(&metrics_spinlock);
  speed = (metrics.mps + metrics.mpsOffset) * 3.6f;
  portEXIT_CRITICAL(&metrics_spinlock);
  
  return speed;
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
  static float speedBeforePress = 0.0f;
  static uint8_t consecutiveFailures = 0;

  const uint32_t now_ms = millis();
  const float HYSTERESIS = 0.35f;

  // Acceleration thresholds derived from treadmill physics:
  // - Nominal button-press acceleration: ~0.11 m/s² (from calibration rate 0.398 km/h/s)
  // - Natural belt deceleration after release: ~0.08 m/s²
  // - Shutdown deceleration: 0.24-0.42 m/s²
  // Coast threshold: high enough to avoid triggering on sensor noise
  const float ACCEL_COAST_THRESHOLD = 0.10f;  // m/s² — skip press if already moving toward target this fast
  // Decel guard threshold: clearly fighting our press direction (only checked after minimum elapsed)
  const float ACCEL_GUARD_THRESHOLD = 0.10f;  // m/s² — abort press if decelerating this hard after 1s
  const uint32_t GUARD_MIN_ELAPSED_MS = 1000; // Treadmill needs time to respond to relay press

  // Reset on treadmill start
  if (!wasRunning && metrics.isRunning && !metrics.isPaused) {
    state = SC_IDLE;
    lastTargetSpeed = -1.0f;
    activePin = 0;
    consecutiveFailures = 0;
  }

  wasRunning = metrics.isRunning && !metrics.isPaused;

  // Abort and cleanup if workout stops
  if (!metrics.isRunning || metrics.isPaused) {
    if (state == SC_PRESSING && activePin != 0) {
      gpio_set_level((gpio_num_t)activePin, 1);
      Serial.println("[Speed Control] Workout stopped - releasing button");
    }
    state = SC_IDLE;
    lastTargetSpeed = -1.0f;
    activePin = 0;
    consecutiveFailures = 0;
    return;
  }

  if (targetSpeed_kmh < MIN_SPEED_KMH) {
    return;
  }

  const float current_kmh = getCurrentSpeedRaw();
  const float diff = targetSpeed_kmh - current_kmh;

  switch (state) {
    case SC_IDLE: {
      if (fabsf(diff) > HYSTERESIS) {
        // Don't press if already at limits
        if (diff > 0 && current_kmh >= MAX_SPEED_KMH) return;
        if (diff < 0 && current_kmh <= MIN_SPEED_KMH) return;

        // Don't press if treadmill is already moving toward target on its own
        if (diff > 0 && metrics.acceleration > ACCEL_COAST_THRESHOLD && metrics.jerk >= 0.0f) {
          return;
        }
        if (diff < 0 && metrics.acceleration < -ACCEL_COAST_THRESHOLD && metrics.jerk <= 0.0f) {
          return;
        }

        // Persistent failure: stop trying if consecutive presses all fail
        if (consecutiveFailures >= 5) {
          Serial.printf("[Speed Control] Giving up after %d consecutive failures (speed %.1f, target %.1f)\n",
                        consecutiveFailures, current_kmh, targetSpeed_kmh);
          return;
        }

        // Log new target
        if (lastTargetSpeed < 0.0f || fabsf(targetSpeed_kmh - lastTargetSpeed) > 0.05f) {
          lastTargetSpeed = targetSpeed_kmh;
          consecutiveFailures = 0;  // New target resets failure counter
          Serial.printf("[Speed Control] New target: %.1f km/h, Current: %.1f km/h, Diff: %.2f km/h\n",
                        targetSpeed_kmh, current_kmh, diff);
        }

        speedUp = (diff > 0);
        activePin = speedUp ? storedGlobals.SPEED_UP_PIN : storedGlobals.SPEED_DOWN_PIN;

        if (activePin == 0) {
          Serial.println("[Speed Control] ERROR: PIN not configured");
          return;
        }

        float rate = getInterpolatedRateForSpeed(current_kmh, speedUp);
        if (rate < 0.1f) rate = 0.5f;

        uint32_t calcPress_ms = (uint32_t)((fabsf(diff) / rate) * 1000.0f);
        uint32_t inertiaComp_ms = storedGlobals.INERTIA_DELAY_MS;
        targetPressDuration = (calcPress_ms > inertiaComp_ms) ? (calcPress_ms - inertiaComp_ms) : 50;

        if (targetPressDuration < 50) targetPressDuration = 50;
        if (targetPressDuration > 30000) targetPressDuration = 30000;

        Serial.printf("[Speed Control] %s: %.1f -> %.1f km/h (rate=%.3f, press=%u ms, failures=%d)\n",
                      speedUp ? "UP" : "DOWN", current_kmh, targetSpeed_kmh,
                      rate, targetPressDuration, consecutiveFailures);

        speedBeforePress = current_kmh;
        gpio_set_level((gpio_num_t)activePin, 0);  // LOW = relay active
        pressStartTime = now_ms;
        state = SC_PRESSING;
      }
      break;
    }

    case SC_PRESSING: {
      uint32_t elapsed = now_ms - pressStartTime;

      // Overshoot compensation using configurable factor
      const float OVERSHOOT_COMP_UP = 0.2f * storedGlobals.OVERSHOOT_FACTOR;
      const float OVERSHOOT_COMP_DOWN = 0.8f * storedGlobals.OVERSHOOT_FACTOR;

      // --- SAFETY FIRST: hard speed limits ---
      if (speedUp && current_kmh >= MAX_SPEED_KMH) {
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
      // --- Early release: approaching target speed ---
      else if (speedUp && current_kmh >= (targetSpeed_kmh - OVERSHOOT_COMP_UP)) {
        gpio_set_level((gpio_num_t)activePin, 1);
        Serial.printf("[Speed Control] Early release at %.1f km/h after %u ms (target %.1f)\n",
                      current_kmh, elapsed, targetSpeed_kmh);
        consecutiveFailures = 0;  // Reached target
        stabilizationStartTime = now_ms;
        state = SC_STABILIZING;
        activePin = 0;
      }
      else if (!speedUp && current_kmh <= (targetSpeed_kmh + OVERSHOOT_COMP_DOWN)) {
        gpio_set_level((gpio_num_t)activePin, 1);
        Serial.printf("[Speed Control] Early release at %.1f km/h after %u ms (target %.1f)\n",
                      current_kmh, elapsed, targetSpeed_kmh);
        consecutiveFailures = 0;  // Reached target
        stabilizationStartTime = now_ms;
        state = SC_STABILIZING;
        activePin = 0;
      }
      // --- Decel guard: abort if treadmill fights press AFTER it had time to respond ---
      else if (speedUp && elapsed >= GUARD_MIN_ELAPSED_MS
               && metrics.acceleration < -ACCEL_GUARD_THRESHOLD && metrics.jerk <= 0.0f) {
        gpio_set_level((gpio_num_t)activePin, 1);
        Serial.printf("[Speed Control] Abort UP: decel %.4f m/s², jerk %.4f after %u ms\n",
                      metrics.acceleration, metrics.jerk, elapsed);
        stabilizationStartTime = now_ms;
        state = SC_STABILIZING;
        activePin = 0;
      }
      else if (!speedUp && elapsed >= GUARD_MIN_ELAPSED_MS
               && metrics.acceleration > ACCEL_GUARD_THRESHOLD && metrics.jerk >= 0.0f) {
        gpio_set_level((gpio_num_t)activePin, 1);
        Serial.printf("[Speed Control] Abort DOWN: accel %.4f m/s², jerk %.4f after %u ms\n",
                      metrics.acceleration, metrics.jerk, elapsed);
        stabilizationStartTime = now_ms;
        state = SC_STABILIZING;
        activePin = 0;
      }
      // --- Duration reached ---
      else if (elapsed >= targetPressDuration) {
        gpio_set_level((gpio_num_t)activePin, 1);
        Serial.printf("[Speed Control] Released after %u ms at %.1f km/h\n", elapsed, current_kmh);
        stabilizationStartTime = now_ms;
        state = SC_STABILIZING;
        activePin = 0;
      }
      break;
    }

    case SC_STABILIZING: {
      uint32_t stabilizationTime = now_ms - stabilizationStartTime;

      if (stabilizationTime >= storedGlobals.INERTIA_DELAY_MS) {
        // Track if speed moved toward or away from target
        float diffBefore = fabsf(targetSpeed_kmh - speedBeforePress);
        float diffAfter = fabsf(targetSpeed_kmh - current_kmh);
        if (diffAfter >= diffBefore) {
          consecutiveFailures++;
        } else {
          consecutiveFailures = 0;
        }

        Serial.printf("[Speed Control] Stabilized at %.1f km/h (target %.1f, diff %.2f, failures=%d)\n",
                      current_kmh, targetSpeed_kmh, diffAfter, consecutiveFailures);
        state = SC_IDLE;
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

