// ============================================================================
// File   : ESP32_treadmill_tacho_test.cpp
// Purpose: Realistic test data generation using timeout callback
// ============================================================================
#include "ESP32_treadmill_tacho_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <Arduino.h>

extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;
extern volatile bool testdata;

// ISR call counter for debugging
volatile uint32_t test_isr_call_count = 0;

// Protect shared test-mode state between ISR and task context
static portMUX_TYPE test_metrics_mux = portMUX_INITIALIZER_UNLOCKED;

// Shared state exposed to loop() for post-processing
static volatile bool test_metrics_pending = false;
static volatile bool test_metrics_running = false;
static volatile int32_t test_speed_milli_kmh = 0;
static volatile uint16_t test_ticks_in_state = 0;

// ============================================================================
// TEST STATE MACHINE
// ============================================================================
// Realistic treadmill simulation:
// - Ramp up from 0 to 12 km/h over 30 seconds
// - Hold at 12 km/h for 2 minutes
// - Ramp down from 12 to 0 km/h over 20 seconds
// - Hold at 0 for 10 seconds
// - Repeat cycle (total: 3 min 20 sec per cycle)
// ============================================================================

typedef enum {
    TEST_STATE_RAMP_UP,      // 0 → 12 km/h (30 sec)
    TEST_STATE_HOLD_HIGH,    // Hold 12 km/h (2 min)
    TEST_STATE_RAMP_DOWN,    // 12 km/h → 0 (20 sec)
    TEST_STATE_HOLD_LOW      // Hold 0 (10 sec)
} test_state_t;

static test_state_t test_state = TEST_STATE_RAMP_UP;

// Timing constants expressed in 200 ms timer ticks
static constexpr uint16_t TEST_TICKS_RAMP_UP = 150;   // 30 s / 0.2 s
static constexpr uint16_t TEST_TICKS_HOLD_HIGH = 600; // 120 s / 0.2 s
static constexpr uint16_t TEST_TICKS_RAMP_DOWN = 100; // 20 s / 0.2 s
static constexpr uint16_t TEST_TICKS_HOLD_LOW = 50;   // 10 s / 0.2 s

// Speed scaling (milli-km/h to avoid floating point in ISR)
static constexpr int32_t TEST_SPEED_MAX_MILLI = 12000;      // 12.000 km/h
static constexpr int32_t TEST_STEP_RAMP_UP_MILLI = 80;      // 0.08 km/h per tick
static constexpr int32_t TEST_STEP_RAMP_DOWN_MILLI = 120;   // 0.12 km/h per tick
static constexpr float   TEST_INTERVAL_SEC = 0.2f;          // GPTimer period

// ============================================================================
// TEST METRICS UPDATE - Called every 200ms from on_timeout_cb ISR
// ============================================================================
void IRAM_ATTR updateTestMetrics() {
    if (!testdata) {
        return;
    }

    test_isr_call_count += 1;  // Track ISR calls for debugging

    portENTER_CRITICAL_ISR(&test_metrics_mux);

    uint16_t ticks = test_ticks_in_state + 1;
    int32_t speed_milli = test_speed_milli_kmh;

    switch (test_state) {
        case TEST_STATE_RAMP_UP:
            if (speed_milli < TEST_SPEED_MAX_MILLI) {
                speed_milli += TEST_STEP_RAMP_UP_MILLI;
                if (speed_milli > TEST_SPEED_MAX_MILLI) {
                    speed_milli = TEST_SPEED_MAX_MILLI;
                }
            }
            if (ticks >= TEST_TICKS_RAMP_UP) {
                test_state = TEST_STATE_HOLD_HIGH;
                ticks = 0;
            }
            break;

        case TEST_STATE_HOLD_HIGH:
            speed_milli = TEST_SPEED_MAX_MILLI;
            if (ticks >= TEST_TICKS_HOLD_HIGH) {
                test_state = TEST_STATE_RAMP_DOWN;
                ticks = 0;
            }
            break;

        case TEST_STATE_RAMP_DOWN:
            if (speed_milli > 0) {
                if (speed_milli >= TEST_STEP_RAMP_DOWN_MILLI) {
                    speed_milli -= TEST_STEP_RAMP_DOWN_MILLI;
                } else {
                    speed_milli = 0;
                }
            }
            if (ticks >= TEST_TICKS_RAMP_DOWN) {
                test_state = TEST_STATE_HOLD_LOW;
                ticks = 0;
            }
            break;

        case TEST_STATE_HOLD_LOW:
            speed_milli = 0;
            if (ticks >= TEST_TICKS_HOLD_LOW) {
                test_state = TEST_STATE_RAMP_UP;
                ticks = 0;
            }
            break;
    }

    test_speed_milli_kmh = speed_milli;
    test_ticks_in_state = ticks;
    test_metrics_running = (speed_milli > 0);
    test_metrics_pending = true;

    portEXIT_CRITICAL_ISR(&test_metrics_mux);
}

// ============================================================================
// TEST MODE CONTROL
// ============================================================================
// Called by web interface to enable/disable test mode
void enableTestdata(bool on) {
    if (on) {
        // Enter test mode - reset state machine
        test_state = TEST_STATE_RAMP_UP;
        testdata = true;
        metrics.isRunning = false;
        metrics.workoutDistance = 0.0f;

        portENTER_CRITICAL(&test_metrics_mux);
        test_ticks_in_state = 0;
        test_speed_milli_kmh = 0;
        test_metrics_running = false;
        test_metrics_pending = true; // ensure immediate update
        portEXIT_CRITICAL(&test_metrics_mux);
        Serial.println("[TEST] Test mode ENABLED - Realistic speed simulation active");
        Serial.println("[TEST] Profile: 0→12 km/h (30s) → hold (2min) → 12→0 km/h (20s) → hold (10s) → repeat");
    } else {
        // Exit test mode - return to real sensor data
        testdata = false;
        metrics.isRunning = false;
        
        // Clear metrics for clean transition back to real sensors
        metrics.motorRPM = 0;
        metrics.rpm = 0;
        metrics.mps = 0;
        metrics.mpsSmooth = 0;

        portENTER_CRITICAL(&test_metrics_mux);
        test_ticks_in_state = 0;
        test_speed_milli_kmh = 0;
        test_metrics_running = false;
        test_metrics_pending = false;
        portEXIT_CRITICAL(&test_metrics_mux);
        
        Serial.println("[TEST] Test mode DISABLED - Returning to real sensor data");
    }
}

void processPendingTestMetrics() {
    if (!testdata) {
        return;
    }

    bool pending = false;
    int32_t speed_milli = 0;
    bool running = false;

    portENTER_CRITICAL(&test_metrics_mux);
    if (test_metrics_pending) {
        pending = true;
        speed_milli = test_speed_milli_kmh;
        running = test_metrics_running;
        test_metrics_pending = false;
    }
    portEXIT_CRITICAL(&test_metrics_mux);

    if (!pending) {
        return;
    }

    float speed_kmh = (float)speed_milli / 1000.0f;
    float mps = speed_kmh / 3.6f;
    float belt_m = storedGlobals.BELT_DISTANCE_MM / 1000.0f;
    float rpm = 0.0f;
    if (belt_m > 0.001f) {
        float rps = mps / belt_m;
        rpm = rps * 60.0f;
    }

    metrics.mps = mps;
    metrics.mpsSmooth = mps;
    metrics.rpm = rpm;
    metrics.motorRPM = rpm;
    metrics.isRunning = running;

    metrics.workoutDistance += mps * TEST_INTERVAL_SEC;
}

