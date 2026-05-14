#include <Arduino.h>
#include <driver/gpio.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"
#include "ESP32_treadmill_tacho_bootlog.h"

extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;

float getCurrentSpeedRaw() {
  static portMUX_TYPE metrics_spinlock = portMUX_INITIALIZER_UNLOCKED;
  float speed;
  portENTER_CRITICAL(&metrics_spinlock);
  speed = (metrics.mps + metrics.mpsOffset) * 3.6f;
  portEXIT_CRITICAL(&metrics_spinlock);
  return speed;
}

void writePress(uint8_t pin, bool pressed) {
  if (pin == 0) return;
  if (pin == storedGlobals.SPEED_UP_PIN) {
    if (pressed && !speedUpBusy && !speedDownBusy) {
      gpio_set_level((gpio_num_t)pin, 0);
      speedUpBusy = true;
      esp_timer_stop(speedUpTimer);
      esp_timer_start_once(speedUpTimer, PULSE_US);
    }
  } else if (pin == storedGlobals.SPEED_DOWN_PIN) {
    if (pressed && !speedDownBusy && !speedUpBusy) {
      gpio_set_level((gpio_num_t)pin, 0);
      speedDownBusy = true;
      esp_timer_stop(speedDownTimer);
      esp_timer_start_once(speedDownTimer, PULSE_US);
    }
  } else if (pin == storedGlobals.INCLINE_UP_PIN) {
    if (pressed && !inclineUpBusy && !inclineDownBusy) {
      gpio_set_level((gpio_num_t)pin, 0);
      inclineUpBusy = true;
      esp_timer_stop(inclineUpTimer);
      esp_timer_start_once(inclineUpTimer, PULSE_US);
    }
  } else if (pin == storedGlobals.INCLINE_DOWN_PIN) {
    if (pressed && !inclineDownBusy && !inclineUpBusy) {
      gpio_set_level((gpio_num_t)pin, 0);
      inclineDownBusy = true;
      esp_timer_stop(inclineDownTimer);
      esp_timer_start_once(inclineDownTimer, PULSE_US);
    }
  } else {
    Serial.println("ERROR PIN not defined");
  }
}
