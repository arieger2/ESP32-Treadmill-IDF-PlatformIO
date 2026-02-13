// ============================================================================
// Web API - Monitor value getters
// ============================================================================

#include "ESP32_treadmill_tacho_web.h"
#include "ESP32_treadmill_tacho_config.h"
#include <TimeLib.h>

// Pace display cache/state (shared by getPaceMin/getPaceSec)
static float    g_pace_sec_filt = 0.0f;
static uint32_t g_pace_last_ms  = 0;
static int      g_pace_min_disp = -1;
static bool     g_pace_valid    = false;
static bool     g_pace_was_moving = false;

static void updatePaceDisplay() {
  float base_mps = metrics.mpsSmooth + metrics.mpsOffset;
  bool moving = (base_mps >= 0.28f);
  uint32_t now_ms = millis();
  const uint32_t timeout_ms = 4000;

  // Track movement state changes
  if (!moving && g_pace_was_moving) {
    g_pace_last_ms = now_ms;
  }
  g_pace_was_moving = moving;

  // Validity check
  if (moving) {
    g_pace_valid = true;
    g_pace_last_ms = now_ms;
  } else if ((now_ms - g_pace_last_ms) >= timeout_ms) {
    g_pace_valid = false;
    g_pace_min_disp = -1;
    g_pace_sec_filt = 0.0f;
    return;
  }

  // Use already filtered speed directly (no additional smoothing needed)
  if (base_mps < 0.0f) base_mps = 0.0f;

  // Convert filtered speed -> instantaneous pace (sec/km)
  // At 10 km/h (2.7777... m/s), we want exactly 360.0 sec (6:00 min/km)
  float pace_sec = (base_mps > 0.001f) ? (1000.0f / base_mps) : 9999.0f;

  // Minute display with tighter hysteresis
  if (g_pace_min_disp < 0) g_pace_min_disp = (int)(pace_sec / 60.0f);

  const float FAST_MARGIN_SEC = 1.0f;
  const float SLOW_MARGIN_SEC = 1.0f;
  float faster_threshold = g_pace_min_disp * 60.0f - FAST_MARGIN_SEC;
  float slower_threshold = (g_pace_min_disp + 1) * 60.0f + SLOW_MARGIN_SEC;

  if (pace_sec <= faster_threshold && g_pace_min_disp > 0) {
    g_pace_min_disp--;
  } else if (pace_sec >= slower_threshold) {
    g_pace_min_disp++;
  }

  g_pace_sec_filt = pace_sec;
}

static uint32_t g_pausedTotal_ms = 0;
static uint32_t g_pauseStart_ms = 0;
static bool g_wasPaused = false;

void resetWorkoutTimer() {
  g_pausedTotal_ms = 0;
  g_pauseStart_ms = 0;
  g_wasPaused = false;
}

void resetMonitorViewState() {
  g_pace_sec_filt = 0.0f;
  g_pace_last_ms = 0;
  g_pace_min_disp = -1;
  g_pace_valid = false;
  g_pace_was_moving = false;
}

static uint32_t getWorkoutElapsedSeconds() {
  if (metrics.sessionStartTime == 0) return 0;
  
  uint32_t now = millis();
  float currentSpeed = (metrics.mpsSmooth + metrics.mpsOffset) * 3.6f; // km/h
  bool shouldPause = (currentSpeed <= 0.0f);
  
  // Detect pause state change
  if (shouldPause && !g_wasPaused) {
    // Just stopped - start pause timer
    g_pauseStart_ms = now;
    g_wasPaused = true;
  } else if (!shouldPause && g_wasPaused) {
    // Just resumed - add pause duration to total
    if (g_pauseStart_ms > 0) {
      g_pausedTotal_ms += (now - g_pauseStart_ms);
    }
    g_pauseStart_ms = 0;
    g_wasPaused = false;
  }
  
  // Calculate total paused time
  uint32_t totalPaused = g_pausedTotal_ms;
  if (shouldPause && g_pauseStart_ms > 0) {
    totalPaused += (now - g_pauseStart_ms);
  }
  
  // Return elapsed time minus paused time
  uint32_t elapsed_ms = (now > metrics.sessionStartTime) 
                        ? (now - metrics.sessionStartTime - totalPaused) 
                        : 0;
  return elapsed_ms / 1000UL;
}

String getSpeed() {
  float kmh = (metrics.mpsSmooth + metrics.mpsOffset) * 3.6f;
  static float q = 0.0f;
  float r = roundf(kmh * 10.0f) / 10.0f;
  if (fabsf(kmh - q) > 0.06f) q = r;
  return String(q, 1);
}

String getPaceMin() {
  updatePaceDisplay();
  if (!g_pace_valid || g_pace_min_disp < 0) return "--";
  return String(g_pace_min_disp);
}

String getPaceSec() {
  updatePaceDisplay();
  if (!g_pace_valid || g_pace_min_disp < 0) return "--";
  float sec_in_min = g_pace_sec_filt - (g_pace_min_disp * 60.0f);
  if (sec_in_min < 0.0f)  sec_in_min = 0.0f;
  if (sec_in_min > 59.9f) sec_in_min = 59.9f;
  int sec_rounded = (int)(sec_in_min + 0.5f);
  if (sec_rounded > 59) sec_rounded = 59;
  char buf[3];
  snprintf(buf, sizeof(buf), "%02d", sec_rounded);
  return String(buf);
}

String getMotorSensorInterrupts() { return String(0); }  // Removed
String getMotorRPM()              { return String((int)metrics.motorRPM); }

String getDistance() {
  const float meters = metrics.workoutDistance; // Already in meters
  if (meters < 1000.0f) {
    uint32_t m_rounded = (uint32_t)(meters + 0.5f);
    return String(m_rounded);
  }
  const float km = meters / 1000.0f;
  char buf[16];
  dtostrf(km, 0, 2, buf);
  return String(buf);
}

String getDistanceUnit() {
  const float meters = metrics.workoutDistance; // Already in meters
  return (meters < 1000.0f) ? "m" : "km";
}

String getRPM() { return String((int)metrics.rpm); }

String getHour() {
  uint32_t elapsed = getWorkoutElapsedSeconds();
  return String((int)(elapsed / 3600UL));
}

String getMinute() {
  uint32_t elapsed = getWorkoutElapsedSeconds();
  return String((int)((elapsed / 60UL) % 60UL));
}

String getSecond() {
  uint32_t elapsed = getWorkoutElapsedSeconds();
  return String((int)(elapsed % 60UL));
}

String getOffset() { return String(metrics.mpsOffset * 3.6f, 1); }

String getHeartRate() {
  if (!metrics.heartRateConnected || metrics.heartRateBpm == 0) {
    return String("--");
  }
  return String(metrics.heartRateBpm);
}

String getRR() {
  if (!metrics.heartRateConnected || metrics.rrLastMs == 0) {
    return String("--");
  }
  return String(metrics.rrLastMs);
}

String getDateTime() {
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hour(), minute(), second());
  return String(buf);
}

String getTestDataButtonText() {
  return testdata ? String("Turn Test Data OFF") : String("Turn Test Data ON");
}

String getSignalQuality() {
  if (metrics.signalCV < 0.05f) return String("Excellent");
  if (metrics.signalCV < 0.10f) return String("Good");
  if (metrics.signalCV < 0.15f) return String("Fair");
  return String("Bad");
}

String getSignalCV() {
  return String(metrics.signalCV * 100.0f, 1);
}

String getSignalFrequency() {
  return String((int)metrics.signalFrequency);
}

String getSignalQualityClass() {
  if (metrics.signalCV < 0.05f) return String("quality-excellent");
  if (metrics.signalCV < 0.10f) return String("quality-good");
  if (metrics.signalCV < 0.15f) return String("quality-fair");
  return String("quality-bad");
}
