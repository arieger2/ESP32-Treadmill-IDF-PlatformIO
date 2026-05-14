#pragma once
#include <Arduino.h>

constexpr float    DEFAULT_CMD_RATE_KMHPS             = 1.0f / 1.39f;
constexpr float    DEFAULT_BELT_RATE_KMHPS            = 1.0f / 2.04f;
constexpr uint32_t DEFAULT_RESPONSE_DELAY_MS          = 400;
constexpr float    DEFAULT_INERTIA_KMH                = 0.20f;
constexpr uint32_t MIN_RELAY_ON_MS                    = 300;
constexpr float    COAST_ERROR_NEAR_KMH               = 0.25f;
constexpr float    COAST_ERROR_FAR_KMH                = 1.00f;
constexpr float    COAST_FAST_RESUME_ERROR_KMH        = 0.55f;
constexpr float    FORCE_LONG_PRESS_ERROR_KMH         = 0.60f;
constexpr float    MIN_LONG_PRESS_THRESH_KMH          = 0.60f;
constexpr float    MAX_LONG_PRESS_THRESH_KMH          = 0.90f;
constexpr float    EARLY_RELEASE_PLAN_FRACTION        = 0.70f;
constexpr float    PREDICTION_HORIZON_MIN_S           = 0.30f;
constexpr float    PREDICTION_HORIZON_MAX_S           = 0.80f;
constexpr float    LARGE_ERROR_CATCHUP_THRESHOLD_KMH  = 2.0f;
constexpr uint32_t LARGE_PRESS_CATCHUP_THRESHOLD_MS   = 3500;
constexpr uint32_t MAX_MODEL_CATCHUP_MS               = 12000;
constexpr uint32_t MIN_LARGE_RAMP_CATCHUP_MS          = 1200;
constexpr float    MODEL_CATCHUP_GAIN                 = 1.60f;
constexpr uint32_t COAST_TIMEOUT_EXTRA_MS             = 3000;

static inline float    sanitizeRate(float v, float fb)       { return (v > 0.05f && v < 5.0f) ? v : fb; }
static inline uint32_t sanitizeDelayMs(uint32_t v, uint32_t fb) { return (v >= 50 && v <= 5000) ? v : fb; }
static inline float    sanitizeInertia(float v, float fb)    { return (v >= 0.0f && v < 5.0f) ? v : fb; }

enum PidajState : uint8_t { PIDAJ_IDLE, PIDAJ_LONG_PRESS, PIDAJ_COAST, PIDAJ_PULSE_COOLDOWN };

struct PidajCtx {
    PidajState state            = PIDAJ_IDLE;
    bool       wasRunning       = false;
    float      integral         = 0.0f;
    float      lastTarget       = -1.0f;
    uint32_t   lastTime_ms      = 0;
    uint32_t   stateTime        = 0;
    uint32_t   pressStartTime   = 0;
    uint8_t    activePin        = 0;
    bool       goingUp          = false;
    bool       inErrorBand      = true;
    uint32_t   dynamicPulseCooldownMs  = 0;
    uint32_t   plannedPressMs          = 0;
    uint32_t   pendingModelCatchupMs   = 0;
    uint32_t   logTimer                = 0;
};

struct PidajInputs {
    uint32_t now;
    float    dt;
    float    speed;
    float    error;
    float    errorAbs;
    float    targetSpeed_kmh;
    float    accel;
    float    jrk;
};

struct PidajModel {
    float    cmdRate;
    float    beltRateUp;
    float    beltRateDown;
    uint32_t responseDelayMs;
    float    inertiaUpKmh;
    float    inertiaDownKmh;
    float    bandEnter;
    float    bandExit;
};

extern PidajCtx pidajCtx;

void pidajHandleIdle(PidajCtx&, const PidajInputs&, const PidajModel&);
void pidajHandleLongPress(PidajCtx&, const PidajInputs&, const PidajModel&);
void pidajHandleCoast(PidajCtx&, const PidajInputs&, const PidajModel&);
void pidajHandlePulseCooldown(PidajCtx&, uint32_t now);
