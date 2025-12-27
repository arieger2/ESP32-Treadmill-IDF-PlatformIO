// ============================================================================
// File   : ESP32_treadmill_tacho_test.cpp
// Purpose: Test data generation and simulation
// ============================================================================
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_sensor_common.h"

extern TreadmillMetrics metrics;
extern volatile int64_t test_pulse_count;
extern volatile bool testdata;
extern volatile bool metrics_need_reset;

// ============================================================================
// TEST DATA GENERATION
// ============================================================================
// Called by web interface to enable/disable test mode
void enableTestdata(bool on) {
    if (on) {
        // Enter test mode - stop PCNT hardware and use simulated pulses
        pcnt_unit_handle_t band_handle = getPCNTHandle(PCNT_ID_BAND);
        pcnt_unit_handle_t motor_handle = getPCNTHandle(PCNT_ID_MOTOR);
        if (band_handle) pcnt_unit_stop(band_handle);
        if (motor_handle) pcnt_unit_stop(motor_handle);
        test_pulse_count = 0;
        testdata = true;
        metrics.isRunning = true;
        metrics_need_reset = false;  // Clear reset flag when entering test mode
        Serial.println("[TEST] Test data mode ENABLED - PCNT paused, using simulated pulses");
    } else {
        // Exit test mode - restart PCNT hardware
        testdata = false;
        metrics.isRunning = false;
        
        // Clear PCNT counters before resuming
        pcnt_unit_handle_t band_handle = getPCNTHandle(PCNT_ID_BAND);
        pcnt_unit_handle_t motor_handle = getPCNTHandle(PCNT_ID_MOTOR);
        if (band_handle) {
            pcnt_unit_clear_count(band_handle);
            pcnt_unit_start(band_handle);
        }
        if (motor_handle) {
            pcnt_unit_clear_count(motor_handle);
            pcnt_unit_start(motor_handle);
        }
        test_pulse_count = 0;
        
        // Immediately clear all displayed metrics
        metrics.motorRPM = 0;
        metrics.rpm = 0;
        metrics.mps = 0;
        metrics.mpsSmooth = 0;
        
        metrics_need_reset = true;  // Signal that internal state needs to be reset
        Serial.println("[TEST] Test data mode DISABLED - PCNT resumed, metrics cleared");
    }
}

// Called by loop() to generate simulated pulses in test mode
void generateTestData() {
    if (testdata) {
        // Simulate a pulse by incrementing the test counter
        test_pulse_count = test_pulse_count + 1;
    }
}
