#include <Arduino.h>
#include <driver/gpio.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"

extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;
extern String saveSettings();

// ============================================================================
// Data structures
// ============================================================================

#define CAL_MAX_SEGMENTS 6

struct CalSegment {
    float    startSpeed_kmh;
    float    endSpeed_kmh;
    float    rate_kmh_per_s;
    uint32_t duration_ms;
};

struct CalResult {
    bool     valid;
    uint8_t  upCount;
    CalSegment up[CAL_MAX_SEGMENTS];
    uint8_t  downCount;
    CalSegment down[CAL_MAX_SEGMENTS];
    uint32_t responseDelay_ms;
    float    inertiaUp_kmh;
    uint32_t inertiaUp_ms;
    float    inertiaDown_kmh;
    uint32_t inertiaDown_ms;
};

static CalResult calResult = {};

// Checkpoints based on SENSOR speed (actual motor speed, not display)
static const float    UP_CP[]         = {4.0f, 8.0f, 12.0f, 15.0f};
static const float    DOWN_CP[]       = {12.0f, 8.0f, 4.0f, 1.5f};
static const uint8_t  UP_CP_COUNT     = sizeof(UP_CP)   / sizeof(UP_CP[0]);
static const uint8_t  DOWN_CP_COUNT   = sizeof(DOWN_CP) / sizeof(DOWN_CP[0]);

// ============================================================================
// State machine
// ============================================================================

enum CalState : uint8_t {
    CAL_IDLE, CAL_WAIT_STABLE,
    CAL_UP_PRESS, CAL_UP_INERTIA, CAL_WAIT_BEFORE_DOWN,
    CAL_DOWN_PRESS, CAL_DOWN_INERTIA,
    CAL_FINALIZE, CAL_DONE, CAL_ERROR
};

static CalState  calState        = CAL_IDLE;
static uint32_t  stateEntryTime  = 0;
static String    calMessage      = "";

// Press tracking
static float     pressStartSpeed   = 0.0f;
static uint32_t  pressStartTime    = 0;
static float     segStartSpeed     = 0.0f;
static uint32_t  segStartTime      = 0;
static uint8_t   cpIndex           = 0;
static bool      responseDelayDone = false;

// Inertia tracking
static float     releaseSpeed       = 0.0f;
static float     inertiaPeak        = 0.0f;
static uint32_t  releaseTime        = 0;
static float     lastInertiaSpeed   = 0.0f;
static uint32_t  lastSpeedChangeTime = 0;

static const uint32_t SAFETY_TIMEOUT_MS = 20000;
static const uint32_t INERTIA_SETTLE_MS = 3000;

// ============================================================================
// Helpers
// ============================================================================

static float sensorSpeed_kmh() {
    return (metrics.mps + metrics.mpsOffset) * 3.6f;
}

static void releaseAllPins() {
    gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN,   1);
    gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 1);
}

static void enterState(CalState s) {
    calState       = s;
    stateEntryTime = millis();
}

static void calError(const char* msg) {
    releaseAllPins();
    calState   = CAL_ERROR;
    calMessage = msg;
    Serial.printf("[CAL] ERROR: %s\n", msg);
}

// ============================================================================
// calHandlePress — unified UP and DOWN press handler
// ============================================================================

static void calHandlePress(uint32_t now, uint32_t elapsed, float speed, bool goingUp) {
    const float*   cp      = goingUp ? UP_CP      : DOWN_CP;
    const uint8_t  cpCount = goingUp ? UP_CP_COUNT : DOWN_CP_COUNT;
    uint8_t&       count   = goingUp ? calResult.upCount : calResult.downCount;
    CalSegment*    segs    = goingUp ? calResult.up      : calResult.down;
    const char*    tag     = goingUp ? "[CAL UP]" : "[CAL DOWN]";

    // Measure response delay (UP only)
    if (goingUp && !responseDelayDone && speed > pressStartSpeed + 0.15f) {
        calResult.responseDelay_ms = now - pressStartTime;
        responseDelayDone = true;
        Serial.printf("%s Response delay: %u ms (speed %.1f -> %.1f)\n",
                      tag, calResult.responseDelay_ms, pressStartSpeed, speed);
    }

    // Checkpoint reached?
    const bool cpReached = goingUp ? (speed >= cp[cpIndex]) : (speed <= cp[cpIndex]);
    if (cpIndex < cpCount && cpReached) {
        if (count < CAL_MAX_SEGMENTS) {
            CalSegment& seg    = segs[count];
            seg.startSpeed_kmh = segStartSpeed;
            seg.endSpeed_kmh   = speed;
            seg.duration_ms    = now - segStartTime;
            seg.rate_kmh_per_s = seg.duration_ms > 0
                ? fabsf(speed - segStartSpeed) / (seg.duration_ms / 1000.0f) : 0.0f;
            count++;
            Serial.printf("%s Segment %d: %.1f -> %.1f km/h in %.1fs = %.2f km/h/s\n",
                          tag, count, seg.startSpeed_kmh, seg.endSpeed_kmh,
                          seg.duration_ms / 1000.0f, seg.rate_kmh_per_s);
        }
        segStartSpeed = speed;
        segStartTime  = now;
        cpIndex++;

        if (cpIndex >= cpCount) {
            gpio_set_level((gpio_num_t)(goingUp ? storedGlobals.SPEED_UP_PIN : storedGlobals.SPEED_DOWN_PIN), 1);
            releaseSpeed = speed; releaseTime = now;
            inertiaPeak = lastInertiaSpeed = speed; lastSpeedChangeTime = now;
            enterState(goingUp ? CAL_UP_INERTIA : CAL_DOWN_INERTIA);
            calMessage = goingUp ? "UP done - measuring motor lag" : "DOWN done - measuring motor lag";
            Serial.printf("%s Released at %.1f km/h, measuring inertia\n", tag, speed);
            return;
        }
    }

    // Safety: speed limit or timeout
    const bool safetyHit = goingUp ? (speed >= MAX_SPEED_KMH - 0.5f) : (speed <= MIN_SPEED_KMH + 0.3f);
    if (safetyHit || elapsed >= SAFETY_TIMEOUT_MS) {
        gpio_set_level((gpio_num_t)(goingUp ? storedGlobals.SPEED_UP_PIN : storedGlobals.SPEED_DOWN_PIN), 1);
        const bool partialValid = goingUp ? (speed > segStartSpeed + 0.3f) : (segStartSpeed > speed + 0.3f);
        if (partialValid && count < CAL_MAX_SEGMENTS) {
            CalSegment& seg    = segs[count];
            seg.startSpeed_kmh = segStartSpeed;
            seg.endSpeed_kmh   = speed;
            seg.duration_ms    = now - segStartTime;
            seg.rate_kmh_per_s = seg.duration_ms > 0
                ? fabsf(speed - segStartSpeed) / (seg.duration_ms / 1000.0f) : 0.0f;
            count++;
            Serial.printf("%s Partial segment: %.1f -> %.1f km/h = %.2f km/h/s\n",
                          tag, seg.startSpeed_kmh, seg.endSpeed_kmh, seg.rate_kmh_per_s);
        }
        releaseSpeed = speed; releaseTime = now;
        inertiaPeak = lastInertiaSpeed = speed; lastSpeedChangeTime = now;
        enterState(goingUp ? CAL_UP_INERTIA : CAL_DOWN_INERTIA);
        calMessage = goingUp ? "UP safety stop - measuring motor lag" : "DOWN safety stop - measuring motor lag";
        Serial.printf("%s Safety stop at %.1f km/h (elapsed %us), measuring inertia\n",
                      tag, speed, elapsed / 1000);
    }
}

// ============================================================================
// calHandleInertia — unified UP and DOWN inertia measurement
// ============================================================================

static void calHandleInertia(uint32_t now, uint32_t elapsed, float speed, bool goingUp) {
    // Track peak (UP = max, DOWN = min)
    if (goingUp ? (speed > inertiaPeak) : (speed < inertiaPeak))
        inertiaPeak = speed;

    if (fabsf(speed - lastInertiaSpeed) > 0.05f) {
        lastInertiaSpeed    = speed;
        lastSpeedChangeTime = now;
    }

    const bool stableBySpeed = (now - lastSpeedChangeTime) >= 2000;
    const bool stableByAccel = (fabsf(metrics.acceleration) < 0.02f) && (elapsed >= 2000);

    if (stableBySpeed || stableByAccel || elapsed >= INERTIA_SETTLE_MS + 5000) {
        const char* tag = goingUp ? "[CAL UP]" : "[CAL DOWN]";
        if (goingUp) {
            calResult.inertiaUp_kmh = inertiaPeak - releaseSpeed;
            calResult.inertiaUp_ms  = now - releaseTime;
            Serial.printf("%s Inertia: +%.2f km/h over %u ms (release=%.1f, peak=%.1f, settled=%.1f)\n",
                          tag, calResult.inertiaUp_kmh, calResult.inertiaUp_ms,
                          releaseSpeed, inertiaPeak, speed);
            enterState(CAL_WAIT_BEFORE_DOWN);
            calMessage = "Waiting before DOWN test";
        } else {
            calResult.inertiaDown_kmh = releaseSpeed - inertiaPeak;
            calResult.inertiaDown_ms  = now - releaseTime;
            Serial.printf("%s Inertia: -%.2f km/h over %u ms (release=%.1f, min=%.1f, settled=%.1f)\n",
                          tag, calResult.inertiaDown_kmh, calResult.inertiaDown_ms,
                          releaseSpeed, inertiaPeak, speed);
            enterState(CAL_FINALIZE);
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

void startSpeedCalibration() {
    if (calState != CAL_IDLE && calState != CAL_DONE && calState != CAL_ERROR) {
        calMessage = "Calibration already running";
        return;
    }
    if (!metrics.isRunning) { calError("Treadmill must be running"); return; }

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

void updateCalibration() {
    if (calState == CAL_IDLE || calState == CAL_DONE || calState == CAL_ERROR) return;
    if (!metrics.isRunning) { calError("Treadmill stopped during calibration"); return; }

    const uint32_t now     = millis();
    const uint32_t elapsed = now - stateEntryTime;
    const float    speed   = sensorSpeed_kmh();

    switch (calState) {

    case CAL_WAIT_STABLE:
        if (elapsed >= 2000) {
            pressStartSpeed = speed; pressStartTime = now;
            segStartSpeed   = speed; segStartTime   = now;
            cpIndex = 0; responseDelayDone = false;
            while (cpIndex < UP_CP_COUNT && UP_CP[cpIndex] <= speed + 0.5f) cpIndex++;
            if (cpIndex >= UP_CP_COUNT) { calError("Speed already above all UP checkpoints"); return; }
            gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 0);
            enterState(CAL_UP_PRESS);
            calMessage = "Pressing UP - measuring acceleration";
            Serial.printf("[CAL UP] Long press started at %.1f km/h, first checkpoint: %.0f km/h\n",
                          speed, UP_CP[cpIndex]);
        }
        break;

    case CAL_UP_PRESS:   calHandlePress(now, elapsed, speed, true);  break;
    case CAL_UP_INERTIA: calHandleInertia(now, elapsed, speed, true); break;

    case CAL_WAIT_BEFORE_DOWN:
        if (elapsed >= 3000) {
            pressStartSpeed = speed; pressStartTime = now;
            segStartSpeed   = speed; segStartTime   = now;
            cpIndex = 0;
            while (cpIndex < DOWN_CP_COUNT && DOWN_CP[cpIndex] >= speed - 0.5f) cpIndex++;
            if (cpIndex >= DOWN_CP_COUNT) { calError("Speed already below all DOWN checkpoints"); return; }
            gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 0);
            enterState(CAL_DOWN_PRESS);
            calMessage = "Pressing DOWN - measuring deceleration";
            Serial.printf("[CAL DOWN] Long press started at %.1f km/h, first checkpoint: %.0f km/h\n",
                          speed, DOWN_CP[cpIndex]);
        }
        break;

    case CAL_DOWN_PRESS:   calHandlePress(now, elapsed, speed, false);   break;
    case CAL_DOWN_INERTIA: calHandleInertia(now, elapsed, speed, false); break;

    case CAL_FINALIZE: {
        calResult.valid = true;
        float avgUp = 0.0f;
        for (uint8_t i = 0; i < calResult.upCount; i++) avgUp += calResult.up[i].rate_kmh_per_s;
        if (calResult.upCount > 0) avgUp /= calResult.upCount;

        float avgDown = 0.0f;
        for (uint8_t i = 0; i < calResult.downCount; i++) avgDown += calResult.down[i].rate_kmh_per_s;
        if (calResult.downCount > 0) avgDown /= calResult.downCount;

        storedGlobals.INERTIA_DELAY_MS = (calResult.inertiaUp_ms + calResult.inertiaDown_ms) / 2;
        if (avgUp   > 0.05f && avgUp   < 5.0f) storedGlobals.CTRL_BELT_RATE_UP_KMHPS   = avgUp;
        if (avgDown > 0.05f && avgDown < 5.0f) storedGlobals.CTRL_BELT_RATE_DOWN_KMHPS = avgDown;
        if (calResult.responseDelay_ms >= 50 && calResult.responseDelay_ms <= 5000)
            storedGlobals.CTRL_RESPONSE_DELAY_MS = calResult.responseDelay_ms;
        if (calResult.inertiaUp_kmh   >= 0.0f && calResult.inertiaUp_kmh   < 5.0f)
            storedGlobals.CTRL_INERTIA_UP_KMH   = calResult.inertiaUp_kmh;
        if (calResult.inertiaDown_kmh >= 0.0f && calResult.inertiaDown_kmh < 5.0f)
            storedGlobals.CTRL_INERTIA_DOWN_KMH = calResult.inertiaDown_kmh;

        Serial.printf("\n========================================\n[CAL] CALIBRATION COMPLETE\n");
        Serial.printf("[CAL] Response delay: %u ms\n", calResult.responseDelay_ms);
        Serial.printf("[CAL] UP segments: %d\n", calResult.upCount);
        for (uint8_t i = 0; i < calResult.upCount; i++)
            Serial.printf("  [%d] %.1f -> %.1f km/h  %.2f km/h/s  (%u ms)\n",
                          i, calResult.up[i].startSpeed_kmh, calResult.up[i].endSpeed_kmh,
                          calResult.up[i].rate_kmh_per_s, calResult.up[i].duration_ms);
        Serial.printf("[CAL] DOWN segments: %d\n", calResult.downCount);
        for (uint8_t i = 0; i < calResult.downCount; i++)
            Serial.printf("  [%d] %.1f -> %.1f km/h  %.2f km/h/s  (%u ms)\n",
                          i, calResult.down[i].startSpeed_kmh, calResult.down[i].endSpeed_kmh,
                          calResult.down[i].rate_kmh_per_s, calResult.down[i].duration_ms);
        Serial.printf("[CAL] Inertia UP: +%.2f km/h in %u ms\n", calResult.inertiaUp_kmh, calResult.inertiaUp_ms);
        Serial.printf("[CAL] Inertia DOWN: -%.2f km/h in %u ms\n", calResult.inertiaDown_kmh, calResult.inertiaDown_ms);
        Serial.printf("[CAL] Avg UP=%.3f DOWN=%.3f inertia=%ums\n", avgUp, avgDown, storedGlobals.INERTIA_DELAY_MS);
        Serial.printf("[CAL] Model: up=%.3f dn=%.3f rsp=%ums inrtUp=%.2f inrtDn=%.2f\n",
                      storedGlobals.CTRL_BELT_RATE_UP_KMHPS, storedGlobals.CTRL_BELT_RATE_DOWN_KMHPS,
                      storedGlobals.CTRL_RESPONSE_DELAY_MS,
                      storedGlobals.CTRL_INERTIA_UP_KMH, storedGlobals.CTRL_INERTIA_DOWN_KMH);
        Serial.printf("========================================\n");

        String result = saveSettings();
        if (result == "") {
            calState   = CAL_DONE;
            calMessage = String("OK! UP=") + String(avgUp, 2)
                       + " DOWN=" + String(avgDown, 2)
                       + " delay=" + String(calResult.responseDelay_ms) + "ms"
                       + " inertia=" + String(storedGlobals.INERTIA_DELAY_MS) + "ms";
        } else {
            calError(("NVS save failed: " + result).c_str());
        }
        break;
    }

    default: break;
    }
}

// ============================================================================
// getCalibrationStatus — JSON for web UI
// ============================================================================

String getCalibrationStatus() {
    const char* stateStr;
    switch (calState) {
        case CAL_IDLE:             stateStr = "idle";          break;
        case CAL_WAIT_STABLE:      stateStr = "stabilizing";   break;
        case CAL_UP_PRESS:         stateStr = "pressing_up";   break;
        case CAL_UP_INERTIA:       stateStr = "up_inertia";    break;
        case CAL_WAIT_BEFORE_DOWN: stateStr = "wait_down";     break;
        case CAL_DOWN_PRESS:       stateStr = "pressing_down"; break;
        case CAL_DOWN_INERTIA:     stateStr = "down_inertia";  break;
        case CAL_FINALIZE:         stateStr = "finalizing";    break;
        case CAL_DONE:             stateStr = "done";          break;
        case CAL_ERROR:            stateStr = "error";         break;
        default:                   stateStr = "unknown";       break;
    }
    String json = "{";
    json += "\"state\":\""        + String(stateStr)                         + "\"";
    json += ",\"message\":\""     + calMessage                                + "\"";
    json += ",\"isCalibrated\":"  + String(calResult.valid ? "true" : "false");
    json += ",\"upSamples\":"     + String(calResult.upCount);
    json += ",\"downSamples\":"   + String(calResult.downCount);
    json += ",\"responseDelay\":" + String(calResult.responseDelay_ms);
    json += ",\"inertiaUp\":"     + String(calResult.inertiaUp_kmh,   2);
    json += ",\"inertiaDown\":"   + String(calResult.inertiaDown_kmh, 2);
    json += ",\"currentSpeed\":"  + String(sensorSpeed_kmh(),          1);
    json += ",\"checkpoint\":"    + String(cpIndex);
    json += "}";
    return json;
}
