#include <Arduino.h>
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

