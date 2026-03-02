#include <stdint.h>
#pragma once
#include <Arduino.h>
#include <vector>
#include <functional>
#include "ESP32_treadmill_tacho_config.h" 

extern TreadmillStoredGlobals storedGlobals;

// If your BLE code already defines these, keep only one set of prototypes.
void bleSetTreadmillSpeedKph(float speedKph);
void bleSetTreadmillInclinePct(float inclinePct);
void physicalSpeedControl(float targetSpeed_kmh, float current_Mps);

struct WorkoutStep {
  uint32_t duration_s = 0;   // seconds
  float    speed_kph  = 0.0; // target speed
  float    incline_pct= 0.0; // target incline (%)
  String   label;            // optional
};

enum class WorkoutState { Idle, Running, Paused, Finished, Error };

struct WorkoutProgress {
  WorkoutState state = WorkoutState::Idle;
  uint32_t total_s = 0;
  uint32_t elapsed_s = 0;

  uint32_t step_index = 0;
  uint32_t step_elapsed_s = 0;
  uint32_t step_remaining_s = 0;

  float  current_speed_kph   = 0.0f;
  float  current_incline_pct = 0.0f;
  String current_label;
  String error;
};

class WorkoutExecutor {
public:
  // Mapping used when ZWO provides Power (IF) instead of Speed.
  float speedAtIF1_kph = 10.0f;

  // Safety clamps
  float minSpeed_kph    = 3.0f;
  float maxSpeed_kph    = 22.0f;
  float minIncline_pct  = 0.0f;
  float maxIncline_pct  = 15.0f;

  // Lifecycle
  bool  loadFromZwoString(const String& xml);
  void  clear();
  void  start();
  void  pause();
  void  resume();
  void  stop();

  // Call regularly (e.g. every 100–200 ms)
  void  update();

  // UI / state
  WorkoutProgress getProgress() const;
  const std::vector<WorkoutStep>& steps() const { return _steps; }
  void  setSpeedScale(float f) { speed_scale_ = f < 0.10f ? 0.10f : (f > 2.50f ? 2.50f : f); }
  void  nudgeSpeedScale(float rel) { setSpeedScale(speed_scale_ * (1.0f + rel)); } // rel = +0.01 for +1%
  float getSpeedScale() const { return speed_scale_; }
  void scaleAllSpeeds(float factor);

  // Threshold (s/km at IF=1.0). 0 = use speedAtIF1_kph fallback.
  void  setThreshold(float secPerKm) { zwo_threshold_sec_per_km_ = secPerKm < 0.0f ? 0.0f : secPerKm; }
  float getThreshold() const {
    return (zwo_threshold_sec_per_km_ > 0.0f) ? zwo_threshold_sec_per_km_
                                              : (speedAtIF1_kph > 0.0f ? 3600.0f / speedAtIF1_kph : 0.0f);
  }

  // Optional callback on step begin
  std::function<void(const WorkoutStep&, uint32_t)> onStepBegin;

  // Utilities
  static float paceToSpeedKph(float paceMin, int units /*1=min/km, 0=min/mile*/);
  float powerFracToSpeedKph(float frac) const; // uses thresholdSecPerKm if present

private:
  // Runtime state
  std::vector<WorkoutStep> _steps;
  WorkoutState _state = WorkoutState::Idle;
  String _lastError;
  float speed_scale_ = 1.0f;   // NEW: scales all step speeds (kph)

  uint32_t _workoutStart_ms = 0;
  uint32_t _pauseStart_ms   = 0;
  uint32_t _pausedTotal_ms  = 0;

  uint32_t _currentIndex = 0;
  uint32_t _stepStart_ms  = 0;

  // Parsing
  bool _parseZwo(const String& xml);

  // Helpers
  void     _applyTargets(const WorkoutStep& st);
  void     _advanceStep();
  uint32_t _now() const { return millis(); }

  // Attribute helpers (case-insensitive on key)
  static bool _getAttrF  (const String& tag, const char* key, float& outVal);
  static bool _getAttrU32(const String& tag, const char* key, uint32_t& outVal);

  static float _clamp(float v, float vmin, float vmax) {
    return v < vmin ? vmin : (v > vmax ? vmax : v);
  }

  // 0 ⇒ unknown; set from <thresholdSecPerKm> if present, else derived from speedAtIF1_kph
  float zwo_threshold_sec_per_km_ = 0.0f;
};

// ===========================================================================
// Speed Control Functions (from workout_speed_control.cpp)
// ===========================================================================

// Get current speed with outlier filtering (removes unrealistic spikes)
// Returns: Filtered speed in km/h
float getCurrentSpeedWithOutlierFilter();

// Helper: Simple button press
void writePress(uint8_t pin, bool pressed);

// Helper: Press button for specific duration with yield
void writePressForDuration(uint8_t pin, uint32_t duration_ms);

// ===========================================================================
// Calibration Functions (from workout_calibration.cpp)
// ===========================================================================

// Start multi-point speed calibration sequence
// Prerequisites: Treadmill must be running
// Will test speed UP and DOWN with continuous pressing and checkpoints
void startSpeedCalibration();

// Update calibration state machine (non-blocking)
// Call regularly from main loop
void updateCalibration();

// Get current calibration status as JSON string
// Returns: JSON object with state, message, speeds, and rates
String getCalibrationStatus();

// Get interpolated rate for a specific speed based on calibration data
// Parameters:
//   currentSpeed_kmh - Current treadmill speed in km/h
//   speedUp - true for speed up, false for speed down
// Returns: Appropriate rate (km/h per second) for this speed range
float getInterpolatedRateForSpeed(float currentSpeed_kmh, bool speedUp);