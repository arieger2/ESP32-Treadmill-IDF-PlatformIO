#include <stdint.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"

// Forward declarations for sensor selection
extern uint8_t sensorSelection(bool init);

// BLE control functions are implemented in ESP32_treadmill_ble_control.cpp
// They set metrics.targetSpeed/targetInclination which is then chased by physicalSpeedControl() 

// -------- local helper to extract key="value" (case-insensitive key) --------
static bool _extractAttr(const String& src, const char* key, String& outVal) {
  String k = String(key) + "=\"";
  int pos = src.indexOf(k);
  if (pos < 0) {
    String lower = src; lower.toLowerCase();
    String kl = String(key); kl.toLowerCase();
    String pat = kl + "=\"";
    int p = lower.indexOf(pat);
    if (p < 0) return false;
    int q1 = p + pat.length();
    int q2 = lower.indexOf('\"', q1);
    if (q2 < 0) return false;
    outVal = src.substring(q1, q2);
    return true;
  }
  int q1 = pos + k.length();
  int q2 = src.indexOf('\"', q1);
  if (q2 < 0) return false;
  outVal = src.substring(q1, q2);
  return true;
}

bool WorkoutExecutor::_getAttrF(const String& tag, const char* key, float& outVal) {
  String v; if (!_extractAttr(tag, key, v)) return false;
  outVal = v.toFloat(); return true;
}
bool WorkoutExecutor::_getAttrU32(const String& tag, const char* key, uint32_t& outVal) {
  String v; if (!_extractAttr(tag, key, v)) return false;
  long t = v.toInt(); if (t < 0) t = 0; outVal = (uint32_t)t; return true;
}

// ===================== Public API =====================
void WorkoutExecutor::clear() {
  _steps.clear();
  _state = WorkoutState::Idle;
  _lastError = "";
  _workoutStart_ms = _pauseStart_ms = _pausedTotal_ms = 0;
  _currentIndex = 0;
  _stepStart_ms = 0;
}

bool WorkoutExecutor::loadFromZwoString(const String& xml) {
  clear();

  // Read <thresholdSecPerKm> (for Power→speed mapping)
  int a = xml.indexOf("<thresholdSecPerKm>");
  if (a >= 0) {
    a += String("<thresholdSecPerKm>").length();
    int b = xml.indexOf("</thresholdSecPerKm>", a);
    if (b > a) {
      String v = xml.substring(a, b);
      v.trim();
      zwo_threshold_sec_per_km_ = v.toFloat();
    }
  }
  if (zwo_threshold_sec_per_km_ <= 0.0f && speedAtIF1_kph > 0.0f) {
    zwo_threshold_sec_per_km_ = 3600.0f / speedAtIF1_kph;
  }

  if (!_parseZwo(xml)) {
    _state = WorkoutState::Error;
    if (_lastError.isEmpty()) _lastError = "ZWO parse failed.";
    return false;
  }
  if (_steps.empty()) {
    _state = WorkoutState::Error;
    _lastError = "No steps parsed from ZWO.";
    return false;
  }
  _state = WorkoutState::Idle;
  return true;
}

void WorkoutExecutor::start() {
  if (_steps.empty()) { _state = WorkoutState::Error; _lastError = "No workout loaded."; return; }
  if (!metrics.isRunning) { _state = WorkoutState::Error; _lastError = "Cannot start: treadmill not running"; return; }
  
  _state = WorkoutState::Running;
  _workoutStart_ms = _now();
  _pausedTotal_ms  = 0;
  _currentIndex    = 0;
  _stepStart_ms    = _workoutStart_ms;
  _applyTargets(_steps[_currentIndex]);
  if (onStepBegin) onStepBegin(_steps[_currentIndex], _currentIndex);
}

void WorkoutExecutor::pause() {
  if (_state == WorkoutState::Running) { _state = WorkoutState::Paused; _pauseStart_ms = _now(); }
}

void WorkoutExecutor::resume() {
  if (_state == WorkoutState::Paused) {
    // Only allow resume if treadmill is physically running
    if (!metrics.isRunning) {
      _lastError = "Cannot resume: treadmill not running";
      return;
    }
    
    uint32_t n = _now();
    _pausedTotal_ms += (n - _pauseStart_ms);
    _pauseStart_ms = 0;
    _state = WorkoutState::Running;
    _applyTargets(_steps[_currentIndex]); // safety - reapply current targets
  }
}

void WorkoutExecutor::stop() { 
  _state = WorkoutState::Finished;
  // Set target to minimum speed - the reactive speed control will handle ramping down
  bleSetTreadmillSpeedKph(1.6f);  // Minimum treadmill speed
  bleSetTreadmillInclinePct(0.0f);
}

void WorkoutExecutor::update() {
  if (_state != WorkoutState::Running) return;
  
  // Auto-pause if treadmill is turned off (speed = 0)
  float currentSpeed_kmh = (metrics.mpsSmooth + metrics.mpsOffset) * 3.6f;
  if (currentSpeed_kmh < 0.5f) {
    Serial.println("[Workout] Treadmill stopped - auto-pausing workout");
    pause();
    return;
  }
  
  uint32_t n = _now();
  const WorkoutStep& st = _steps[_currentIndex];
  uint32_t elapsedStep_ms = n - _stepStart_ms - _pausedTotal_ms;
  if (elapsedStep_ms >= st.duration_s * 1000UL) _advanceStep();
}

WorkoutProgress WorkoutExecutor::getProgress() const {
  WorkoutProgress p;
  p.state = _state;
  uint32_t total_s = 0; for (const auto& s : _steps) total_s += s.duration_s; p.total_s = total_s;

  if (_state == WorkoutState::Idle || _state == WorkoutState::Error || _state == WorkoutState::Finished) {
    if (_state == WorkoutState::Finished) {
      p.elapsed_s = p.total_s; // Show full duration when finished
    } else {
      p.elapsed_s = 0;
    }
    if (_state == WorkoutState::Error) p.error = _lastError; 
    return p;
  }

  uint32_t n = _now();
  uint32_t totalPaused = _pausedTotal_ms;
  
  // If currently paused, add current pause duration
  if (_state == WorkoutState::Paused && _pauseStart_ms > 0) {
    totalPaused += (n - _pauseStart_ms);
  }
  
  // Calculate elapsed time by subtracting paused time
  p.elapsed_s = (n > _workoutStart_ms) ? ((n - _workoutStart_ms - totalPaused) / 1000UL) : 0;

  if (_currentIndex >= _steps.size()) {
    p.state = WorkoutState::Finished;
    p.step_index = _steps.empty() ? 0 : (_steps.size() - 1);
    return p;
  }

  const WorkoutStep& st = _steps[_currentIndex];
  p.step_index       = _currentIndex;
  
  // Calculate step elapsed time, including current pause if paused
  uint32_t stepPausedTime = _pausedTotal_ms;
  if (_state == WorkoutState::Paused && _pauseStart_ms > 0) {
    stepPausedTime += (n - _pauseStart_ms);
  }
  p.step_elapsed_s   = (n > _stepStart_ms) ? ((n - _stepStart_ms - stepPausedTime) / 1000UL) : 0;
  p.step_remaining_s = (st.duration_s > p.step_elapsed_s) ? (st.duration_s - p.step_elapsed_s) : 0;
  p.current_speed_kph   = st.speed_kph;
  p.current_incline_pct = st.incline_pct;
  p.current_label       = st.label;
  return p;
}

// ===================== Internals =====================
void WorkoutExecutor::_applyTargets(const WorkoutStep& st) {
  float sp = _clamp(st.speed_kph * speed_scale_,   minSpeed_kph,  maxSpeed_kph);  // <— multiply
  float ic = _clamp(st.incline_pct, minIncline_pct, maxIncline_pct);
  bleSetTreadmillSpeedKph(sp);
  bleSetTreadmillInclinePct(ic);
}

void WorkoutExecutor::_advanceStep() {
  if (_currentIndex + 1 < _steps.size()) {
    _currentIndex++;
    _stepStart_ms   = _now();
    _pausedTotal_ms = 0;
    _applyTargets(_steps[_currentIndex]);
    if (onStepBegin) onStepBegin(_steps[_currentIndex], _currentIndex);
  } else {
    _state = WorkoutState::Finished;
  }
}

// ===================== ZWO parsing (document order) =====================
bool WorkoutExecutor::_parseZwo(const String& xml) {
  _steps.reserve(32);

  auto push = [this](uint32_t dur, float spKph, float incl, const char* label) {
    if (!dur) return;
    WorkoutStep s; s.duration_s = dur; s.speed_kph = spKph; s.incline_pct = incl; s.label = label ? label : "";
    this->_steps.push_back(s);
  };
  auto pushRamp = [&](uint32_t dur, float v0, float v1, float incl, const char* label) {
    if (!dur) return;
    const uint32_t segs = (dur >= 240) ? 16 : (dur >= 120 ? 12 : (dur >= 60 ? 8 : 4));
    uint32_t base = dur / segs, rem  = dur % segs;
    for (uint32_t i = 0; i < segs; ++i) {
      float t  = (segs == 1) ? 1.0f : (float)i / (float)(segs - 1);
      float sp = v0 + (v1 - v0) * t;
      push(base + (i < rem ? 1U : 0U), sp, incl, label);
    }
  };

  int pos = 0;
  int iterCount = 0;
  while (true) {
    // Feed watchdog every 100 iterations to prevent reset on large files
    if (++iterCount % 100 == 0) yield();
    
    int lt = xml.indexOf('<', pos); if (lt < 0) break;
    int gt = xml.indexOf('>', lt + 1); if (gt < 0) break;

    String tagOpen = xml.substring(lt + 1, gt); tagOpen.trim();
    if (tagOpen.startsWith("/")) { pos = gt + 1; continue; } // closing tag

    int sp = tagOpen.indexOf(' ');
    String name = (sp >= 0) ? tagOpen.substring(0, sp) : tagOpen;
    String nameLower = name; nameLower.toLowerCase();

    bool selfClosed = tagOpen.endsWith("/");
    int afterOpen = gt + 1;
    int afterTag  = afterOpen;
    if (!selfClosed) {
      String closeSeq = "</" + name + ">";
      int close = xml.indexOf(closeSeq, afterOpen);
      if (close >= 0) afterTag = close + closeSeq.length();
    }

    String attrs = "<" + tagOpen + ">"; // read attributes from opening tag

    if (nameLower == "steadystate") {
      uint32_t dur = 0; float spd=0, inc=0, pwr=0;
      bool hasDur = _getAttrU32(attrs, "Duration", dur);
      bool hasSp  = _getAttrF(attrs,  "Speed",    spd);
      bool hasInc = _getAttrF(attrs,  "Incline",  inc);
      bool hasPwr = _getAttrF(attrs,  "Power",    pwr);
      if (hasDur && dur) {
        float speedKph = hasSp ? spd : (hasPwr ? powerFracToSpeedKph(pwr) : minSpeed_kph);
        push(dur, speedKph, hasInc ? inc : 0.0f, "SteadyState");
      }
    }
    else if (nameLower == "intervalst") {
      uint32_t rep=0, onDur=0, offDur=0;
      float onSp=0, offSp=0, onInc=0, offInc=0, onP=0, offP=0;
      bool hasOnSp  = _getAttrF(attrs, "OnSpeed", onSp);
      bool hasOffSp = _getAttrF(attrs, "OffSpeed", offSp);
      bool hasOnInc = _getAttrF(attrs, "OnIncline", onInc);
      bool hasOffInc= _getAttrF(attrs, "OffIncline", offInc);
      bool hasOnP   = _getAttrF(attrs, "OnPower", onP);
      bool hasOffP  = _getAttrF(attrs, "OffPower", offP);
      _getAttrU32(attrs, "Repeat",     rep);
      _getAttrU32(attrs, "OnDuration", onDur);
      _getAttrU32(attrs, "OffDuration",offDur);

      if (rep && (onDur || offDur)) {
        float onSpeed  = hasOnSp  ? onSp  : (hasOnP  ? powerFracToSpeedKph(onP)  : minSpeed_kph);
        float offSpeed = hasOffSp ? offSp : (hasOffP ? powerFracToSpeedKph(offP) : minSpeed_kph);
        float onIncl   = hasOnInc ? onInc : 0.0f;
        float offIncl  = hasOffInc? offInc: 0.0f;

        for (uint32_t i = 0; i < rep; ++i) {
          if (onDur)  push(onDur,  onSpeed,  onIncl,  "IntervalsT-ON");
          if (offDur) push(offDur, offSpeed, offIncl, "IntervalsT-OFF");
        }
      }
    }
    else if (nameLower == "warmup") {
      uint32_t dur = 0; float pl=0, ph=0, sl=0, sh=0, inc=0;
      bool hasDur = _getAttrU32(attrs, "Duration", dur);
      bool hasPL  = _getAttrF(attrs, "PowerLow",  pl);
      bool hasPH  = _getAttrF(attrs, "PowerHigh", ph);
      bool hasSL  = _getAttrF(attrs, "SpeedLow",  sl);
      bool hasSH  = _getAttrF(attrs, "SpeedHigh", sh);
      bool hasInc = _getAttrF(attrs, "Incline",   inc);
      if (hasDur && dur) {
        float v0 = hasSL ? sl : (hasPL ? powerFracToSpeedKph(pl) : minSpeed_kph);
        float v1 = hasSH ? sh : (hasPH ? powerFracToSpeedKph(ph) : v0);
        pushRamp(dur, v0, v1, hasInc ? inc : 0.0f, "Warmup");
      }
    }
    else if (nameLower == "cooldown") {
      uint32_t dur = 0; float pl=0, ph=0, sl=0, sh=0, inc=0;
      bool hasDur = _getAttrU32(attrs, "Duration", dur);
      bool hasPL  = _getAttrF(attrs, "PowerLow",  pl);
      bool hasPH  = _getAttrF(attrs, "PowerHigh", ph);
      bool hasSL  = _getAttrF(attrs, "SpeedLow",  sl);
      bool hasSH  = _getAttrF(attrs, "SpeedHigh", sh);
      bool hasInc = _getAttrF(attrs, "Incline",   inc);
      if (hasDur && dur) {
        float v0 = hasSL ? sl : (hasPL ? powerFracToSpeedKph(pl) : minSpeed_kph);
        float v1 = hasSH ? sh : (hasPH ? powerFracToSpeedKph(ph) : v0);
        pushRamp(dur, v0, v1, hasInc ? inc : 0.0f, "Cooldown");
      }
    }

    pos = gt + 1;   // keep scanning inside this element
  }

  return true;
}

void WorkoutExecutor::scaleAllSpeeds(float factor) {
  if (factor <= 0.0f) return;
  for (auto &s : _steps) {
    s.speed_kph = _clamp(s.speed_kph * factor, minSpeed_kph, maxSpeed_kph);
  }
  if (_state == WorkoutState::Running && _currentIndex < _steps.size()) {
    _applyTargets(_steps[_currentIndex]); // feel it immediately
  }
}

// -------------------- Utilities --------------------
float WorkoutExecutor::paceToSpeedKph(float paceMin, int units) {
  if (paceMin <= 0) return 0.0f;
  if (units == 0) { // min/mile
    const float km_per_mile = 1.609344f;
    return (60.0f / paceMin) * km_per_mile;
  }
  return 60.0f / paceMin; // min/km
}

float WorkoutExecutor::powerFracToSpeedKph(float frac) const {
  if (frac <= 0) return 0.0f;
  if (zwo_threshold_sec_per_km_ > 0.0f) {
    float vt = 3600.0f / zwo_threshold_sec_per_km_; // km/h at IF=1.0
    return frac * vt;
  }
  return frac * speedAtIF1_kph;  // fallback
}


void writePress(uint8_t pin, bool pressed) {
  if (pin == 0) return;
  
  if (pin == storedGlobals.SPEED_UP_PIN) {
      if (pressed && !speedUpBusy) {
          gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
          speedUpBusy = true;
          esp_timer_stop(speedUpTimer);
          esp_timer_start_once(speedUpTimer, PULSE_US);
      }
  } else if (pin == storedGlobals.SPEED_DOWN_PIN) {
      if (pressed && !speedUownBusy) {
          gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
          speedUownBusy = true;
          esp_timer_stop(speedDownTimer);
          esp_timer_start_once(speedDownTimer, PULSE_US);
      }
  } else if (pin == storedGlobals.INCLINE_UP_PIN) {
      if (pressed && !inclineUpBusy) {
          gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
          inclineUpBusy = true;
          esp_timer_stop(inclineUpTimer);
          esp_timer_start_once(inclineUpTimer, PULSE_US);
      }
  } else if (pin == storedGlobals.INCLINE_DOWN_PIN) {
      if (pressed && !inclineDownBusy) {
          gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
          inclineDownBusy = true;
          esp_timer_stop(inclineDownTimer);
          esp_timer_start_once(inclineDownTimer, PULSE_US);
      }
  } else {
      Serial.println("ERROR PIN not defined");
  }
}

// Forward declaration
void pressUntilSpeedChanges(uint8_t pin, bool speedUp, float targetSpeed_kmh);

// ===========================================================================
// ADAPTIVE SPEED CONTROL - Watches actual speed change with hysteresis
// Presses button until speed actually changes, then waits for stabilization
// ===========================================================================
void physicalSpeedControl(float targetSpeed_kmh, float current_mps) {
  static float lastTargetSpeed = -1.0f;
  static bool initialized = false;
  static bool wasRunning = false;
  static uint32_t lastActionTime_ms = 0;
  static float speedBeforeAction = 0.0f;
  static bool waitingForStabilization = false;

  const uint32_t now_ms = millis();

  // Reset on treadmill start
  if (!wasRunning && metrics.isRunning && !metrics.isPaused) {
    initialized = false;
    lastTargetSpeed = -1.0f;
    waitingForStabilization = false;
  }
  
  wasRunning = metrics.isRunning && !metrics.isPaused;
  
  if (!metrics.isRunning || metrics.isPaused) {
    initialized = false;
    lastTargetSpeed = -1.0f;
    waitingForStabilization = false;
    return;
  }
  
  // Safety: Treadmill minimum speed is 1.6 km/h
  if (targetSpeed_kmh < 1.6f) {
    return;
  }
  
  if (!initialized) {
    initialized = true;
    lastTargetSpeed = -1.0f;
    waitingForStabilization = false;
  }

  const float current_kmh = (metrics.mpsSmooth + metrics.mpsOffset) * 3.6f;
  
  // If waiting for stabilization after a button press
  if (waitingForStabilization) {
    // Use INERTIA_DELAY_MS as hysteresis time - wait until speed stabilizes
    if ((now_ms - lastActionTime_ms) < storedGlobals.INERTIA_DELAY_MS) {
      return;  // Still waiting
    }
    // Check if speed has changed from before the action
    float speedChange = fabsf(current_kmh - speedBeforeAction);
    if (speedChange > 0.1f) {
      // Speed changed, stabilization complete
      Serial.printf("[Speed Control] Stabilized: %.1f -> %.1f km/h (change: %.2f)\n",
                    speedBeforeAction, current_kmh, speedChange);
    }
    waitingForStabilization = false;
  }

  const float diff = targetSpeed_kmh - current_kmh;
  const float TOLERANCE = 0.15f;
  
  if (fabsf(diff) <= TOLERANCE) {
    lastTargetSpeed = targetSpeed_kmh;
    return;  // Target reached
  }

  // Log new target
  if (lastTargetSpeed < 0.0f || fabsf(targetSpeed_kmh - lastTargetSpeed) > 0.05f) {
    lastTargetSpeed = targetSpeed_kmh;
    Serial.printf("[Speed Control] Target: %.1f km/h, Current: %.1f km/h, Diff: %.2f km/h\n", 
                  targetSpeed_kmh, current_kmh, diff);
  }

  // Determine direction and pin
  uint8_t pin = (diff > 0) ? storedGlobals.SPEED_UP_PIN : storedGlobals.SPEED_DOWN_PIN;
  if (pin == 0) return;
  
  // Remember speed before action
  speedBeforeAction = current_kmh;
  
  // Press button and watch speed change in real-time
  pressUntilSpeedChanges(pin, diff > 0, targetSpeed_kmh);
  
  lastActionTime_ms = millis();
  waitingForStabilization = true;
}

// ============================================================================
// Calibration-based speed control
// Calculates press duration = diff / rate, subtracts INERTIA_DELAY
// Just press for calculated time, then release and let physicalSpeedControl monitor
// ============================================================================
void pressUntilSpeedChanges(uint8_t pin, bool speedUp, float targetSpeed_kmh) {
  const float startSpeed_kmh = (metrics.mpsSmooth + metrics.mpsOffset) * 3.6f;
  const float MAX_SPEED_KMH = 18.0f;          // Safety: never exceed this
  const float MIN_SPEED_KMH = 1.6f;           // Treadmill minimum
  const uint32_t MAX_PRESS_MS = 30000;        // Safety timeout: 30 seconds max
  const uint32_t MIN_PRESS_MS = 50;           // Minimum press time
  const uint32_t CHECK_INTERVAL_MS = 50;      // Check speed every 50ms (less CPU)
  
  // Safety limits
  if (speedUp && targetSpeed_kmh > MAX_SPEED_KMH) targetSpeed_kmh = MAX_SPEED_KMH;
  if (!speedUp && targetSpeed_kmh < MIN_SPEED_KMH) targetSpeed_kmh = MIN_SPEED_KMH;
  
  // Don't press UP if already at max
  if (speedUp && startSpeed_kmh >= MAX_SPEED_KMH) {
    Serial.printf("[Speed Control] Already at max speed %.1f km/h, not pressing UP\n", startSpeed_kmh);
    return;
  }
  // Don't press DOWN if already at min
  if (!speedUp && startSpeed_kmh <= MIN_SPEED_KMH) {
    Serial.printf("[Speed Control] Already at min speed %.1f km/h, not pressing DOWN\n", startSpeed_kmh);
    return;
  }
  
  // Calculate required press duration from calibration
  const float diff_kmh = fabsf(targetSpeed_kmh - startSpeed_kmh);
  float rate = speedUp ? storedGlobals.SPEED_UP_RATE : storedGlobals.SPEED_DOWN_RATE;
  if (rate < 0.1f) rate = 0.5f;  // Fallback if not calibrated
  
  // Time needed = diff / rate, then subtract INERTIA_DELAY for momentum
  uint32_t calcPress_ms = (uint32_t)((diff_kmh / rate) * 1000.0f);
  uint32_t inertiaComp_ms = storedGlobals.INERTIA_DELAY_MS;
  
  // Reduce press time by inertia delay (the treadmill continues moving after release)
  uint32_t targetPress_ms = (calcPress_ms > inertiaComp_ms) ? (calcPress_ms - inertiaComp_ms) : MIN_PRESS_MS;
  if (targetPress_ms < MIN_PRESS_MS) targetPress_ms = MIN_PRESS_MS;
  if (targetPress_ms > MAX_PRESS_MS) targetPress_ms = MAX_PRESS_MS;
  
  Serial.printf("[Speed Control] %s: %.1f -> %.1f km/h (diff=%.2f, rate=%.3f, calc=%u ms, inertia=%u ms, press=%u ms)\n",
                speedUp ? "UP" : "DOWN", startSpeed_kmh, targetSpeed_kmh, 
                diff_kmh, rate, calcPress_ms, inertiaComp_ms, targetPress_ms);
  
  // Activate relay and press for calculated duration
  gpio_set_level((gpio_num_t)pin, 0);  // LOW = relay active
  
  uint32_t startTime = millis();
  uint32_t elapsed = 0;
  uint32_t lastSensorUpdate = 0;
  
  // Simple loop: press for targetPress_ms, only checking safety limits
  // Use vTaskDelay instead of delay() to ensure WiFi/HTTP tasks get CPU time
  float lastSpeed_kmh = startSpeed_kmh;
  uint32_t noChangeCount = 0;
  const uint32_t NO_CHANGE_THRESHOLD = 3;  // Stop after 3 checks (~1.5s) with no speed change
  
  while (elapsed < targetPress_ms) {
    vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));  // FreeRTOS delay - yields to other tasks
    elapsed = millis() - startTime;
    
    // Metrics are updated automatically by ISR callbacks - no manual update needed
    // OLD SYSTEM REMOVED: Previously called updateMetricsMotor/Band here
    if (elapsed - lastSensorUpdate >= 500) {
      lastSensorUpdate = elapsed;
      
      // Check if speed is still changing in the expected direction
      float currentSpeed_kmh = (metrics.mpsSmooth + metrics.mpsOffset) * 3.6f;
      float speedChange = currentSpeed_kmh - lastSpeed_kmh;
      
      // For UP: speed should increase (positive change)
      // For DOWN: speed should decrease (negative change)
      bool expectedChange = speedUp ? (speedChange > 0.05f) : (speedChange < -0.05f);
      
      if (!expectedChange && elapsed > 1000) {  // Only check after 1 second
        noChangeCount++;
        if (noChangeCount >= NO_CHANGE_THRESHOLD) {
          Serial.printf("[Speed Control] No more %s change detected after %u ms at %.1f km/h\n",
                        speedUp ? "UP" : "DOWN", elapsed, currentSpeed_kmh);
          break;
        }
      } else {
        noChangeCount = 0;  // Reset counter if speed is changing
      }
      
      lastSpeed_kmh = currentSpeed_kmh;
    }
    
    float currentSpeed_kmh = (metrics.mpsSmooth + metrics.mpsOffset) * 3.6f;
    
    // Safety: stop if exceeding limits
    if (speedUp && currentSpeed_kmh >= MAX_SPEED_KMH) {
      Serial.printf("[Speed Control] Safety stop at max: %.1f km/h after %u ms\n", currentSpeed_kmh, elapsed);
      break;
    }
    if (!speedUp && currentSpeed_kmh <= MIN_SPEED_KMH) {
      Serial.printf("[Speed Control] Safety stop at min: %.1f km/h after %u ms\n", currentSpeed_kmh, elapsed);
      break;
    }
  }
  
  // Release relay
  gpio_set_level((gpio_num_t)pin, 1);  // HIGH = relay inactive
  
  float finalSpeed = (metrics.mpsSmooth + metrics.mpsOffset) * 3.6f;
  Serial.printf("[Speed Control] Released after %u ms, speed: %.1f -> %.1f km/h\n",
                elapsed, startSpeed_kmh, finalSpeed);
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
  calStartSpeed = metrics.mpsSmooth * 3.6f;  // Convert to km/h
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
    float currentSpeed = metrics.mpsSmooth * 3.6f;
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
      calDownStartSpeed = metrics.mpsSmooth * 3.6f;  // Speed before pressing DOWN
      calState = CAL_PRESSING_DOWN;
      calStateChangeTime = now;
      gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 0);  // Start pressing (relay active)
      Serial.printf("Calibration DOWN: Starting at %.1f km/h, pressing speed down (max 5s or min 2 km/h)...\n", calDownStartSpeed);
    }
  }
  else if (calState == CAL_PRESSING_DOWN) {
    float currentSpeed = metrics.mpsSmooth * 3.6f;
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
      
      Serial.printf("Calibration DOWN complete: speed %.2f -> %.2f km/h (at release), decrease = %.2f km/h in %.1f s, rate = %.3f km/h/s\\n",
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
