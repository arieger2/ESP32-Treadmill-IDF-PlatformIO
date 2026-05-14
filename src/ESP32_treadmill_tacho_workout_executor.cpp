#include <stdint.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"

uint8_t workoutStatus = WORKOUT_INACTIVE;

namespace {
constexpr float STOP_EXIT_SPEED_TOLERANCE_KMH = 0.25f;
constexpr float STOP_EXIT_DROP_THRESHOLD_KMH = 0.03f;
constexpr uint32_t STOP_EXIT_SAMPLE_MS = 200;
constexpr uint32_t STOP_EXIT_STABLE_MS = 1200;
constexpr uint32_t AUTO_PAUSE_GRACE_MS = 2500;
}

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
  workoutStatus = WORKOUT_STOPPED;
  _steps.clear();
  _state = WorkoutState::Idle;
  _durationMode = WorkoutDurationMode::Time;
  _lastError = "";
  _workoutStart_ms = _pauseStart_ms = _pausedTotal_ms = 0;
  _currentIndex = 0;
  _stepStart_ms = 0;
  _stepStartDistance_m = 0.0f;
  _runningLostSince_ms = 0;
}

bool WorkoutExecutor::loadFromZwoString(const String& xml) {
  clear();

  // Read <durationType>time|distance</durationType>
  int dtStart = xml.indexOf("<durationType>");
  if (dtStart >= 0) {
    dtStart += String("<durationType>").length();
    int dtEnd = xml.indexOf("</durationType>", dtStart);
    if (dtEnd > dtStart) {
      String v = xml.substring(dtStart, dtEnd);
      v.trim();
      v.toLowerCase();
      _durationMode = (v == "distance") ? WorkoutDurationMode::Distance : WorkoutDurationMode::Time;
    }
  }

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
  workoutStatus = WORKOUT_STOPPED;
  if (_steps.empty()) { _state = WorkoutState::Error; _lastError = "No workout loaded."; return; }
  if (!metrics.isRunning) { _state = WorkoutState::Error; _lastError = "Cannot start: treadmill not running"; return; }
  workoutStatus = WORKOUT_RUNNING;
  _state = WorkoutState::Running;
  _workoutStart_ms = _now();
  _pausedTotal_ms  = 0;
  _currentIndex    = 0;
  _stepStart_ms    = _workoutStart_ms;
  _stepStartDistance_m = metrics.workoutDistance;
  _runningLostSince_ms = 0;
  _applyTargets(_steps[_currentIndex]);
  if (onStepBegin) onStepBegin(_steps[_currentIndex], _currentIndex);
}

void WorkoutExecutor::pause() {
  workoutStatus = WORKOUT_RUNNING;
  if (_state == WorkoutState::Running) { 
    _state = WorkoutState::Paused; _pauseStart_ms = _now(); 
    _runningLostSince_ms = 0;
    if (metrics.isRunning) {
      bleSetTreadmillSpeedKph(MIN_SPEED_KMH);  // Minimum treadmill speed
      bleSetTreadmillInclinePct(0.0f);
    }
  }
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
    _runningLostSince_ms = 0;
    _state = WorkoutState::Running;
    _applyTargets(_steps[_currentIndex]); // safety - reapply current targets
  }
}

void WorkoutExecutor::stop() {
  _state = WorkoutState::Finished;
  workoutStatus = WORKOUT_STOPPED;
  // Set target to minimum speed - the reactive speed control will handle ramping down
  bleSetTreadmillSpeedKph(MIN_SPEED_KMH);  // Minimum treadmill speed
  bleSetTreadmillInclinePct(0.0f);
}

void WorkoutExecutor::update() {
  const uint32_t n = _now();
  const float currentSpeed_kmh = (metrics.mps + metrics.mpsOffset) * 3.6f;
  static float stopPrevSpeed_kmh = 0.0f;
  static uint32_t stopLastSample_ms = 0;
  static uint32_t stopStableSince_ms = 0;
  static bool stopSampleValid = false;

  // Stop mode is a two-phase state:
  // 1) press Stop -> target ramps down to minimum speed
  // 2) once speed has settled at minimum, leave workout mode completely
  if (workoutStatus == WORKOUT_STOPPED) {
    if (n - stopLastSample_ms >= STOP_EXIT_SAMPLE_MS) {
      stopLastSample_ms = n;

      if (!stopSampleValid) {
        stopPrevSpeed_kmh = currentSpeed_kmh;
        stopSampleValid = true;
      }

      const float speedDrop_kmh = stopPrevSpeed_kmh - currentSpeed_kmh;
      stopPrevSpeed_kmh = currentSpeed_kmh;

      float stopTargetKmh = metrics.targetSpeed;
      if (stopTargetKmh < MIN_SPEED_KMH) stopTargetKmh = MIN_SPEED_KMH;

      const bool nearMinimum = (currentSpeed_kmh <= (stopTargetKmh + STOP_EXIT_SPEED_TOLERANCE_KMH));
      const bool stillSlowing = (speedDrop_kmh > STOP_EXIT_DROP_THRESHOLD_KMH);
      if (nearMinimum && !stillSlowing) {
        if (stopStableSince_ms == 0) stopStableSince_ms = n;
      } else {
        stopStableSince_ms = 0;
      }
    }

    if (stopStableSince_ms != 0 && (n - stopStableSince_ms) >= STOP_EXIT_STABLE_MS) {
      _state = WorkoutState::Idle;
      _lastError = "";
      _workoutStart_ms = 0;
      _pauseStart_ms = 0;
      _pausedTotal_ms = 0;
      _currentIndex = 0;
      _stepStart_ms = 0;
      _stepStartDistance_m = 0.0f;
      _runningLostSince_ms = 0;
      workoutStatus = WORKOUT_INACTIVE;
      metrics.targetSpeed = 0.0f;
      metrics.targetInclination = 0.0f;
      metrics.controlRequested = false;

      stopPrevSpeed_kmh = 0.0f;
      stopLastSample_ms = 0;
      stopStableSince_ms = 0;
      stopSampleValid = false;

      Serial.printf("[Workout] Stop settled at %.1f km/h - leaving workout mode\n", currentSpeed_kmh);
      return;
    }
  } else {
    stopPrevSpeed_kmh = 0.0f;
    stopLastSample_ms = 0;
    stopStableSince_ms = 0;
    stopSampleValid = false;
  }

  if (_state != WorkoutState::Running) return;

  // Auto-pause if treadmill is turned off (speed = 0), with grace period
  if (!metrics.isRunning) {
    if (_runningLostSince_ms == 0) {
      _runningLostSince_ms = n;
    } else if ((n - _runningLostSince_ms) >= AUTO_PAUSE_GRACE_MS) {
      Serial.println("[Workout] Treadmill stopped - auto-pausing workout");
      pause();
    }
    return;
  }
  _runningLostSince_ms = 0;

  const WorkoutStep& st = _steps[_currentIndex];
  if (_durationMode == WorkoutDurationMode::Distance) {
    float elapsedStep_m = metrics.workoutDistance - _stepStartDistance_m;
    if (elapsedStep_m < 0.0f) elapsedStep_m = 0.0f;
    if (elapsedStep_m >= (float)st.duration_value) _advanceStep();
  } else {
    uint32_t elapsedStep_ms = n - _stepStart_ms - _pausedTotal_ms;
    if (elapsedStep_ms >= st.duration_value * 1000UL) _advanceStep();
  }
}

WorkoutProgress WorkoutExecutor::getProgress() const {
  WorkoutProgress p;
  p.state = _state;
  p.duration_mode = _durationMode;
  uint32_t total_value = 0;
  for (const auto& s : _steps) total_value += s.duration_value;
  if (_durationMode == WorkoutDurationMode::Distance) p.total_m = total_value;
  else p.total_s = total_value;

  if (_state == WorkoutState::Idle || _state == WorkoutState::Error || _state == WorkoutState::Finished) {
    if (_state == WorkoutState::Finished) {
      if (_durationMode == WorkoutDurationMode::Distance) p.elapsed_m = p.total_m;
      else p.elapsed_s = p.total_s;
    } else {
      p.elapsed_s = 0;
      p.elapsed_m = 0;
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

  if (_durationMode == WorkoutDurationMode::Distance) {
    uint32_t done_m = 0;
    for (uint32_t i = 0; i < _currentIndex && i < _steps.size(); ++i) done_m += _steps[i].duration_value;
    float stepDelta_m = metrics.workoutDistance - _stepStartDistance_m;
    if (stepDelta_m < 0.0f) stepDelta_m = 0.0f;
    done_m += (uint32_t)stepDelta_m;
    p.elapsed_m = done_m;
  } else {
    // Calculate elapsed time by subtracting paused time
    p.elapsed_s = (n > _workoutStart_ms) ? ((n - _workoutStart_ms - totalPaused) / 1000UL) : 0;
  }

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
  if (_durationMode == WorkoutDurationMode::Distance) {
    float stepElapsed_m = metrics.workoutDistance - _stepStartDistance_m;
    if (stepElapsed_m < 0.0f) stepElapsed_m = 0.0f;
    p.step_elapsed_m = (uint32_t)stepElapsed_m;
    p.step_remaining_m = (st.duration_value > p.step_elapsed_m) ? (st.duration_value - p.step_elapsed_m) : 0;
  } else {
    p.step_elapsed_s   = (n > _stepStart_ms) ? ((n - _stepStart_ms - stepPausedTime) / 1000UL) : 0;
    p.step_remaining_s = (st.duration_value > p.step_elapsed_s) ? (st.duration_value - p.step_elapsed_s) : 0;
  }
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
    _currentIndex += 1;
    _stepStart_ms   = _now();
    _stepStartDistance_m = metrics.workoutDistance;
    _pausedTotal_ms = 0;
    _applyTargets(_steps[_currentIndex]);
    if (onStepBegin) onStepBegin(_steps[_currentIndex], _currentIndex);
  } else {
    stop();
  }
}

// ===================== ZWO parsing (document order) =====================
bool WorkoutExecutor::_parseZwo(const String& xml) {
  _steps.reserve(32);

  auto push = [this](uint32_t dur, float spKph, float incl, const char* label) {
    if (!dur) return;
    WorkoutStep s; s.duration_value = dur; s.speed_kph = spKph; s.incline_pct = incl; s.label = label ? label : "";
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
