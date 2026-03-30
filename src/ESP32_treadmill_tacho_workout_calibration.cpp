#include <Arduino.h>
#include <driver/gpio.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"

extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;
extern String saveSettings();

// ============================================================================
// CALIBRATION: Measure treadmill motor response at different speed ranges
//
// Measures: continuous acceleration rate (UP/DOWN), response delay,
// motor lag (overshoot after release), all at multiple speed ranges.
//
// The treadmill display counts up instantly on button press, but the motor
// lags behind. We measure the ACTUAL motor speed via sensor, not the display.
// ============================================================================

// ----------------------------------------------------------------------------
// Data structures
// ----------------------------------------------------------------------------

#define CAL_MAX_SEGMENTS 6

struct CalSegment {
    float startSpeed_kmh;
    float endSpeed_kmh;
    float rate_kmh_per_s;
    uint32_t duration_ms;
};

struct CalResult {
    bool valid;

    uint8_t upCount;
    CalSegment up[CAL_MAX_SEGMENTS];

    uint8_t downCount;
    CalSegment down[CAL_MAX_SEGMENTS];

    uint32_t responseDelay_ms;

    float inertiaUp_kmh;
    uint32_t inertiaUp_ms;
    float inertiaDown_kmh;
    uint32_t inertiaDown_ms;
};

static CalResult calResult = {};

// Checkpoints based on SENSOR speed (actual motor speed, not display)
static const float UP_CP[] = {4.0f, 8.0f, 12.0f, 15.0f};
static const uint8_t UP_CP_COUNT = sizeof(UP_CP) / sizeof(UP_CP[0]);

static const float DOWN_CP[] = {12.0f, 8.0f, 4.0f, 1.5f};
static const uint8_t DOWN_CP_COUNT = sizeof(DOWN_CP) / sizeof(DOWN_CP[0]);

// ----------------------------------------------------------------------------
// State machine
// ----------------------------------------------------------------------------

enum CalState : uint8_t {
    CAL_IDLE,
    CAL_WAIT_STABLE,
    CAL_UP_PRESS,
    CAL_UP_INERTIA,
    CAL_WAIT_BEFORE_DOWN,
    CAL_DOWN_PRESS,
    CAL_DOWN_INERTIA,
    CAL_FINALIZE,
    CAL_DONE,
    CAL_ERROR
};

static CalState calState = CAL_IDLE;
static uint32_t stateEntryTime = 0;
static String calMessage = "";

// Press tracking
static float pressStartSpeed = 0.0f;
static uint32_t pressStartTime = 0;
static float segStartSpeed = 0.0f;
static uint32_t segStartTime = 0;
static uint8_t cpIndex = 0;
static bool responseDelayDone = false;

// Inertia tracking
static float releaseSpeed = 0.0f;
static float inertiaPeak = 0.0f;
static uint32_t releaseTime = 0;
static float lastInertiaSpeed = 0.0f;
static uint32_t lastSpeedChangeTime = 0;

// Safety
static const uint32_t SAFETY_TIMEOUT_MS = 20000;
static const uint32_t INERTIA_SETTLE_MS = 3000;

// ----------------------------------------------------------------------------
// Helper: read current sensor speed in km/h
// ----------------------------------------------------------------------------

static float sensorSpeed_kmh() {
    return (metrics.mps + metrics.mpsOffset) * 3.6f;
}

// ----------------------------------------------------------------------------
// Helper: release all calibration pins (safety)
// ----------------------------------------------------------------------------

static void releaseAllPins() {
    gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 1);
    gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 1);
}

// ----------------------------------------------------------------------------
// Helper: enter new state
// ----------------------------------------------------------------------------

static void enterState(CalState newState) {
    calState = newState;
    stateEntryTime = millis();
}

// ----------------------------------------------------------------------------
// Helper: abort with error
// ----------------------------------------------------------------------------

static void calError(const char* msg) {
    releaseAllPins();
    calState = CAL_ERROR;
    calMessage = msg;
    Serial.printf("[CAL] ERROR: %s\n", msg);
}

// ============================================================================
// startSpeedCalibration() — called from web API
// ============================================================================

void startSpeedCalibration() {
    if (calState != CAL_IDLE && calState != CAL_DONE && calState != CAL_ERROR) {
        calMessage = "Calibration already running";
        return;
    }

    if (!metrics.isRunning) {
        calError("Treadmill must be running");
        return;
    }

    // Reset all results
    memset(&calResult, 0, sizeof(calResult));

    calMessage = "Calibration starting - waiting for stable speed";
    enterState(CAL_WAIT_STABLE);

    Serial.printf("\n========================================\n");
    Serial.printf("[CAL] Started at %.1f km/h (sensor)\n", sensorSpeed_kmh());
    Serial.printf("[CAL] UP checkpoints: ");
    for (uint8_t i = 0; i < UP_CP_COUNT; i++) Serial.printf("%.0f ", UP_CP[i]);
    Serial.printf("\n[CAL] DOWN checkpoints: ");
    for (uint8_t i = 0; i < DOWN_CP_COUNT; i++) Serial.printf("%.0f ", DOWN_CP[i]);
    Serial.printf("\n========================================\n");
}

// ============================================================================
// updateCalibration() — called every ~1ms from main loop
// ============================================================================

void updateCalibration() {
    if (calState == CAL_IDLE || calState == CAL_DONE || calState == CAL_ERROR) {
        return;
    }

    // Abort if treadmill stops
    if (!metrics.isRunning) {
        calError("Treadmill stopped during calibration");
        return;
    }

    const uint32_t now = millis();
    const uint32_t elapsed = now - stateEntryTime;
    const float speed = sensorSpeed_kmh();

    switch (calState) {

    // ----------------------------------------------------------------
    // Wait 2s for speed to stabilize before starting
    // ----------------------------------------------------------------
    case CAL_WAIT_STABLE: {
        if (elapsed >= 2000) {
            // Start UP press
            pressStartSpeed = speed;
            pressStartTime = now;
            segStartSpeed = speed;
            segStartTime = now;
            cpIndex = 0;
            responseDelayDone = false;

            // Skip checkpoints below current speed
            while (cpIndex < UP_CP_COUNT && UP_CP[cpIndex] <= speed + 0.5f) {
                cpIndex++;
            }
            if (cpIndex >= UP_CP_COUNT) {
                calError("Speed already above all UP checkpoints");
                return;
            }

            gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 0);
            enterState(CAL_UP_PRESS);
            calMessage = "Pressing UP - measuring acceleration";

            Serial.printf("[CAL UP] Long press started at %.1f km/h, first checkpoint: %.0f km/h\n",
                          speed, UP_CP[cpIndex]);
        }
        break;
    }

    // ----------------------------------------------------------------
    // Long press UP — record segments at checkpoints
    // ----------------------------------------------------------------
    case CAL_UP_PRESS: {
        // Measure response delay: time until speed changes > 0.15 km/h from start
        if (!responseDelayDone && speed > pressStartSpeed + 0.15f) {
            calResult.responseDelay_ms = now - pressStartTime;
            responseDelayDone = true;
            Serial.printf("[CAL UP] Response delay: %u ms (speed %.1f -> %.1f)\n",
                          calResult.responseDelay_ms, pressStartSpeed, speed);
        }

        // Check if we reached the next checkpoint
        if (cpIndex < UP_CP_COUNT && speed >= UP_CP[cpIndex]) {
            // Record this segment
            if (calResult.upCount < CAL_MAX_SEGMENTS) {
                CalSegment& seg = calResult.up[calResult.upCount];
                seg.startSpeed_kmh = segStartSpeed;
                seg.endSpeed_kmh = speed;
                seg.duration_ms = now - segStartTime;
                seg.rate_kmh_per_s = (seg.duration_ms > 0)
                    ? (speed - segStartSpeed) / (seg.duration_ms / 1000.0f)
                    : 0.0f;
                calResult.upCount++;

                Serial.printf("[CAL UP] Segment %d: %.1f -> %.1f km/h in %.1fs = %.2f km/h/s\n",
                              calResult.upCount, seg.startSpeed_kmh, seg.endSpeed_kmh,
                              seg.duration_ms / 1000.0f, seg.rate_kmh_per_s);
            }

            // Prepare next segment
            segStartSpeed = speed;
            segStartTime = now;
            cpIndex++;

            // All checkpoints reached?
            if (cpIndex >= UP_CP_COUNT) {
                gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 1);
                releaseSpeed = speed;
                releaseTime = now;
                inertiaPeak = speed;
                lastInertiaSpeed = speed;
                lastSpeedChangeTime = now;
                enterState(CAL_UP_INERTIA);
                calMessage = "UP done - measuring motor lag";
                Serial.printf("[CAL UP] Released at %.1f km/h, measuring inertia\n", speed);
            }
        }

        // Safety: MAX_SPEED or timeout
        if (speed >= MAX_SPEED_KMH - 0.5f || elapsed >= SAFETY_TIMEOUT_MS) {
            gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 1);

            // Record partial segment if we moved
            if (speed > segStartSpeed + 0.3f && calResult.upCount < CAL_MAX_SEGMENTS) {
                CalSegment& seg = calResult.up[calResult.upCount];
                seg.startSpeed_kmh = segStartSpeed;
                seg.endSpeed_kmh = speed;
                seg.duration_ms = now - segStartTime;
                seg.rate_kmh_per_s = (seg.duration_ms > 0)
                    ? (speed - segStartSpeed) / (seg.duration_ms / 1000.0f)
                    : 0.0f;
                calResult.upCount++;
                Serial.printf("[CAL UP] Partial segment: %.1f -> %.1f km/h = %.2f km/h/s\n",
                              seg.startSpeed_kmh, seg.endSpeed_kmh, seg.rate_kmh_per_s);
            }

            releaseSpeed = speed;
            releaseTime = now;
            inertiaPeak = speed;
            lastInertiaSpeed = speed;
            lastSpeedChangeTime = now;
            enterState(CAL_UP_INERTIA);
            calMessage = "UP safety stop - measuring motor lag";
            Serial.printf("[CAL UP] Safety stop at %.1f km/h (elapsed %us), measuring inertia\n",
                          speed, elapsed / 1000);
        }
        break;
    }

    // ----------------------------------------------------------------
    // After UP release: measure how much motor continues to accelerate
    // ----------------------------------------------------------------
    case CAL_UP_INERTIA: {
        // Track peak speed after release
        if (speed > inertiaPeak) {
            inertiaPeak = speed;
        }

        // Detect stabilization: speed stopped changing significantly
        if (fabsf(speed - lastInertiaSpeed) > 0.05f) {
            lastInertiaSpeed = speed;
            lastSpeedChangeTime = now;
        }

        // Stable for 2s OR acceleration near zero for 2s
        bool stableBySpeed = (now - lastSpeedChangeTime) >= 2000;
        bool stableByAccel = (fabsf(metrics.acceleration) < 0.02f) && (elapsed >= 2000);

        if (stableBySpeed || stableByAccel || elapsed >= INERTIA_SETTLE_MS + 5000) {
            calResult.inertiaUp_kmh = inertiaPeak - releaseSpeed;
            calResult.inertiaUp_ms = now - releaseTime;

            Serial.printf("[CAL UP] Inertia: +%.2f km/h over %u ms (release=%.1f, peak=%.1f, settled=%.1f)\n",
                          calResult.inertiaUp_kmh, calResult.inertiaUp_ms,
                          releaseSpeed, inertiaPeak, speed);

            enterState(CAL_WAIT_BEFORE_DOWN);
            calMessage = "Waiting before DOWN test";
        }
        break;
    }

    // ----------------------------------------------------------------
    // Pause 3s before starting DOWN test
    // ----------------------------------------------------------------
    case CAL_WAIT_BEFORE_DOWN: {
        if (elapsed >= 3000) {
            pressStartSpeed = speed;
            pressStartTime = now;
            segStartSpeed = speed;
            segStartTime = now;
            cpIndex = 0;

            // Skip checkpoints above current speed
            while (cpIndex < DOWN_CP_COUNT && DOWN_CP[cpIndex] >= speed - 0.5f) {
                cpIndex++;
            }
            if (cpIndex >= DOWN_CP_COUNT) {
                calError("Speed already below all DOWN checkpoints");
                return;
            }

            gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 0);
            enterState(CAL_DOWN_PRESS);
            calMessage = "Pressing DOWN - measuring deceleration";

            Serial.printf("[CAL DOWN] Long press started at %.1f km/h, first checkpoint: %.0f km/h\n",
                          speed, DOWN_CP[cpIndex]);
        }
        break;
    }

    // ----------------------------------------------------------------
    // Long press DOWN — record segments at checkpoints
    // ----------------------------------------------------------------
    case CAL_DOWN_PRESS: {
        // Check if we reached the next checkpoint (speed falling)
        if (cpIndex < DOWN_CP_COUNT && speed <= DOWN_CP[cpIndex]) {
            // Record this segment
            if (calResult.downCount < CAL_MAX_SEGMENTS) {
                CalSegment& seg = calResult.down[calResult.downCount];
                seg.startSpeed_kmh = segStartSpeed;
                seg.endSpeed_kmh = speed;
                seg.duration_ms = now - segStartTime;
                seg.rate_kmh_per_s = (seg.duration_ms > 0)
                    ? (segStartSpeed - speed) / (seg.duration_ms / 1000.0f)
                    : 0.0f;
                calResult.downCount++;

                Serial.printf("[CAL DOWN] Segment %d: %.1f -> %.1f km/h in %.1fs = %.2f km/h/s\n",
                              calResult.downCount, seg.startSpeed_kmh, seg.endSpeed_kmh,
                              seg.duration_ms / 1000.0f, seg.rate_kmh_per_s);
            }

            // Prepare next segment
            segStartSpeed = speed;
            segStartTime = now;
            cpIndex++;

            // All checkpoints reached?
            if (cpIndex >= DOWN_CP_COUNT) {
                gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 1);
                releaseSpeed = speed;
                releaseTime = now;
                inertiaPeak = speed;
                lastInertiaSpeed = speed;
                lastSpeedChangeTime = now;
                enterState(CAL_DOWN_INERTIA);
                calMessage = "DOWN done - measuring motor lag";
                Serial.printf("[CAL DOWN] Released at %.1f km/h, measuring inertia\n", speed);
            }
        }

        // Safety: MIN_SPEED or timeout
        if (speed <= MIN_SPEED_KMH + 0.3f || elapsed >= SAFETY_TIMEOUT_MS) {
            gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 1);

            // Record partial segment
            if (segStartSpeed > speed + 0.3f && calResult.downCount < CAL_MAX_SEGMENTS) {
                CalSegment& seg = calResult.down[calResult.downCount];
                seg.startSpeed_kmh = segStartSpeed;
                seg.endSpeed_kmh = speed;
                seg.duration_ms = now - segStartTime;
                seg.rate_kmh_per_s = (seg.duration_ms > 0)
                    ? (segStartSpeed - speed) / (seg.duration_ms / 1000.0f)
                    : 0.0f;
                calResult.downCount++;
                Serial.printf("[CAL DOWN] Partial segment: %.1f -> %.1f km/h = %.2f km/h/s\n",
                              seg.startSpeed_kmh, seg.endSpeed_kmh, seg.rate_kmh_per_s);
            }

            releaseSpeed = speed;
            releaseTime = now;
            inertiaPeak = speed;
            lastInertiaSpeed = speed;
            lastSpeedChangeTime = now;
            enterState(CAL_DOWN_INERTIA);
            calMessage = "DOWN safety stop - measuring motor lag";
            Serial.printf("[CAL DOWN] Safety stop at %.1f km/h, measuring inertia\n", speed);
        }
        break;
    }

    // ----------------------------------------------------------------
    // After DOWN release: measure how much motor continues to decelerate
    // ----------------------------------------------------------------
    case CAL_DOWN_INERTIA: {
        // Track minimum speed after release (motor still decelerating)
        if (speed < inertiaPeak) {
            inertiaPeak = speed;
        }

        if (fabsf(speed - lastInertiaSpeed) > 0.05f) {
            lastInertiaSpeed = speed;
            lastSpeedChangeTime = now;
        }

        bool stableBySpeed = (now - lastSpeedChangeTime) >= 2000;
        bool stableByAccel = (fabsf(metrics.acceleration) < 0.02f) && (elapsed >= 2000);

        if (stableBySpeed || stableByAccel || elapsed >= INERTIA_SETTLE_MS + 5000) {
            calResult.inertiaDown_kmh = releaseSpeed - inertiaPeak;
            calResult.inertiaDown_ms = now - releaseTime;

            Serial.printf("[CAL DOWN] Inertia: -%.2f km/h over %u ms (release=%.1f, min=%.1f, settled=%.1f)\n",
                          calResult.inertiaDown_kmh, calResult.inertiaDown_ms,
                          releaseSpeed, inertiaPeak, speed);

            enterState(CAL_FINALIZE);
        }
        break;
    }

    // ----------------------------------------------------------------
    // Save results to storedGlobals and NVS
    // ----------------------------------------------------------------
    case CAL_FINALIZE: {
        calResult.valid = true;

        // Compute average UP rate
        float avgUp = 0.0f;
        for (uint8_t i = 0; i < calResult.upCount; i++) {
            avgUp += calResult.up[i].rate_kmh_per_s;
        }
        if (calResult.upCount > 0) avgUp /= calResult.upCount;

        // Compute average DOWN rate
        float avgDown = 0.0f;
        for (uint8_t i = 0; i < calResult.downCount; i++) {
            avgDown += calResult.down[i].rate_kmh_per_s;
        }
        if (calResult.downCount > 0) avgDown /= calResult.downCount;

        // Store to globals
        storedGlobals.INERTIA_DELAY_MS = (calResult.inertiaUp_ms + calResult.inertiaDown_ms) / 2;

        // Print summary
        Serial.printf("\n========================================\n");
        Serial.printf("[CAL] CALIBRATION COMPLETE\n");
        Serial.printf("[CAL] Response delay: %u ms\n", calResult.responseDelay_ms);
        Serial.printf("[CAL] UP segments: %d\n", calResult.upCount);
        for (uint8_t i = 0; i < calResult.upCount; i++) {
            Serial.printf("  [%d] %.1f -> %.1f km/h  %.2f km/h/s  (%u ms)\n",
                          i, calResult.up[i].startSpeed_kmh, calResult.up[i].endSpeed_kmh,
                          calResult.up[i].rate_kmh_per_s, calResult.up[i].duration_ms);
        }
        Serial.printf("[CAL] DOWN segments: %d\n", calResult.downCount);
        for (uint8_t i = 0; i < calResult.downCount; i++) {
            Serial.printf("  [%d] %.1f -> %.1f km/h  %.2f km/h/s  (%u ms)\n",
                          i, calResult.down[i].startSpeed_kmh, calResult.down[i].endSpeed_kmh,
                          calResult.down[i].rate_kmh_per_s, calResult.down[i].duration_ms);
        }
        Serial.printf("[CAL] Inertia UP: +%.2f km/h in %u ms\n",
                      calResult.inertiaUp_kmh, calResult.inertiaUp_ms);
        Serial.printf("[CAL] Inertia DOWN: -%.2f km/h in %u ms\n",
                      calResult.inertiaDown_kmh, calResult.inertiaDown_ms);
        Serial.printf("[CAL] Avg UP rate: %.3f km/h/s\n", avgUp);
        Serial.printf("[CAL] Avg DOWN rate: %.3f km/h/s\n", avgDown);
        Serial.printf("[CAL] Avg inertia: %u ms\n", storedGlobals.INERTIA_DELAY_MS);
        Serial.printf("========================================\n");

        String result = saveSettings();
        if (result == "") {
            calState = CAL_DONE;
            calMessage = String("OK! UP=") + String(avgUp, 2)
                       + " DOWN=" + String(avgDown, 2)
                       + " delay=" + String(calResult.responseDelay_ms) + "ms"
                       + " inertia=" + String(storedGlobals.INERTIA_DELAY_MS) + "ms";
        } else {
            calError(("NVS save failed: " + result).c_str());
        }
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// getCalibrationStatus() — JSON for web UI
// ============================================================================

String getCalibrationStatus() {
    const char* stateStr;
    switch (calState) {
        case CAL_IDLE:             stateStr = "idle"; break;
        case CAL_WAIT_STABLE:      stateStr = "stabilizing"; break;
        case CAL_UP_PRESS:         stateStr = "pressing_up"; break;
        case CAL_UP_INERTIA:       stateStr = "up_inertia"; break;
        case CAL_WAIT_BEFORE_DOWN: stateStr = "wait_down"; break;
        case CAL_DOWN_PRESS:       stateStr = "pressing_down"; break;
        case CAL_DOWN_INERTIA:     stateStr = "down_inertia"; break;
        case CAL_FINALIZE:         stateStr = "finalizing"; break;
        case CAL_DONE:             stateStr = "done"; break;
        case CAL_ERROR:            stateStr = "error"; break;
        default:                   stateStr = "unknown"; break;
    }

    String json = "{";
    json += "\"state\":\"" + String(stateStr) + "\"";
    json += ",\"message\":\"" + calMessage + "\"";
    json += ",\"isCalibrated\":" + String(calResult.valid ? "true" : "false");
    json += ",\"upSamples\":" + String(calResult.upCount);
    json += ",\"downSamples\":" + String(calResult.downCount);
    json += ",\"responseDelay\":" + String(calResult.responseDelay_ms);
    json += ",\"responseDelay\":" + String(calResult.responseDelay_ms);
    json += ",\"inertiaUp\":" + String(calResult.inertiaUp_kmh, 2);
    json += ",\"inertiaDown\":" + String(calResult.inertiaDown_kmh, 2);
    json += ",\"currentSpeed\":" + String(sensorSpeed_kmh(), 1);
    json += ",\"checkpoint\":" + String(cpIndex);
    json += "}";
    return json;
}
