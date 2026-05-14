#include <Arduino.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"

extern TreadmillMetrics metrics;

WorkoutProgress WorkoutExecutor::getProgress() const {
  WorkoutProgress p;
  p.state         = _state;
  p.duration_mode = _durationMode;

  uint32_t total_value = 0;
  for (const auto& s : _steps) total_value += s.duration_value;
  if (_durationMode == WorkoutDurationMode::Distance) p.total_m = total_value;
  else                                                p.total_s = total_value;

  if (_state == WorkoutState::Idle || _state == WorkoutState::Error || _state == WorkoutState::Finished) {
    if (_state == WorkoutState::Finished) {
      if (_durationMode == WorkoutDurationMode::Distance) p.elapsed_m = p.total_m;
      else                                                p.elapsed_s = p.total_s;
    }
    if (_state == WorkoutState::Error) p.error = _lastError;
    return p;
  }

  uint32_t n = _now();
  uint32_t totalPaused = _pausedTotal_ms;
  if (_state == WorkoutState::Paused && _pauseStart_ms > 0)
    totalPaused += (n - _pauseStart_ms);

  if (_durationMode == WorkoutDurationMode::Distance) {
    uint32_t done_m = 0;
    for (uint32_t i = 0; i < _currentIndex && i < _steps.size(); ++i)
      done_m += _steps[i].duration_value;
    float stepDelta_m = metrics.workoutDistance - _stepStartDistance_m;
    if (stepDelta_m < 0.0f) stepDelta_m = 0.0f;
    p.elapsed_m = done_m + (uint32_t)stepDelta_m;
  } else {
    p.elapsed_s = (n > _workoutStart_ms)
                ? ((n - _workoutStart_ms - totalPaused) / 1000UL) : 0;
  }

  if (_currentIndex >= _steps.size()) {
    p.state      = WorkoutState::Finished;
    p.step_index = _steps.empty() ? 0 : (_steps.size() - 1);
    return p;
  }

  const WorkoutStep& st = _steps[_currentIndex];
  p.step_index = _currentIndex;

  uint32_t stepPausedTime = _pausedTotal_ms;
  if (_state == WorkoutState::Paused && _pauseStart_ms > 0)
    stepPausedTime += (n - _pauseStart_ms);

  if (_durationMode == WorkoutDurationMode::Distance) {
    float stepElapsed_m = metrics.workoutDistance - _stepStartDistance_m;
    if (stepElapsed_m < 0.0f) stepElapsed_m = 0.0f;
    p.step_elapsed_m   = (uint32_t)stepElapsed_m;
    p.step_remaining_m = (st.duration_value > p.step_elapsed_m)
                       ? (st.duration_value - p.step_elapsed_m) : 0;
  } else {
    p.step_elapsed_s   = (n > _stepStart_ms)
                       ? ((n - _stepStart_ms - stepPausedTime) / 1000UL) : 0;
    p.step_remaining_s = (st.duration_value > p.step_elapsed_s)
                       ? (st.duration_value - p.step_elapsed_s) : 0;
  }

  p.current_speed_kph   = st.speed_kph;
  p.current_incline_pct = st.incline_pct;
  p.current_label       = st.label;
  return p;
}
