// ============================================================================
// File   : ESP32_treadmill_tacho_helpers.cpp
// Purpose: Shared helper functions for sensor calculations
// ============================================================================
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_sensor_common.h"
#include "ESP32_treadmill_tacho_filters.h"
#include "driver/pulse_cnt.h"

// PCNT handles (initialized in setup)
static pcnt_unit_handle_t pcnt_unit_band = NULL;
static pcnt_unit_handle_t pcnt_unit_motor = NULL;

extern volatile int64_t test_pulse_count;
extern volatile bool testdata;
extern volatile bool metrics_need_reset;
extern TreadmillStoredGlobals storedGlobals;

// Get PCNT handle by unit ID
pcnt_unit_handle_t getPCNTHandle(PcntUnitId unit) {
    return (unit == PCNT_ID_BAND) ? pcnt_unit_band : pcnt_unit_motor;
}

// Set PCNT handles (called from setup)
void setPCNTHandles(pcnt_unit_handle_t band, pcnt_unit_handle_t motor) {
    pcnt_unit_band = band;
    pcnt_unit_motor = motor;
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================
// Safe micros difference calculation with overflow handling
uint32_t microsDiff(uint32_t later, uint32_t earlier) {
    return (later >= earlier) ? (later - earlier) : (UINT32_MAX - earlier + later + 1);
}

// Helper to read PCNT counter value without clearing
int16_t readPCNT(PcntUnitId unit) {
    static int64_t last_test_count = 0;
    
    // Reset test counter tracking when exiting test mode
    if (metrics_need_reset) {
        last_test_count = 0;
    }
    
    if (testdata) {
        // In test mode, return current test count
        return (int16_t)test_pulse_count;
    } else {
        // Read PCNT hardware counter without clearing
        int count = 0;
        pcnt_unit_handle_t handle = getPCNTHandle(unit);
        if (handle) {
            pcnt_unit_get_count(handle, &count);
        }
        return (int16_t)count;
    }
}

// Helper to clear PCNT counter
void clearPCNT(PcntUnitId unit) {
    if (!testdata) {
        pcnt_unit_handle_t handle = getPCNTHandle(unit);
        if (handle) {
            pcnt_unit_clear_count(handle);
        }
    }
}

// ============================================================================
// AUTO MODE SENSOR SELECTION
// ============================================================================
// Compare both sensors and select the one with smallest RPM change (most stable)
SensorSelection selectStableSensor(SensorState& band_state, SensorState& motor_state) {
    // Read both sensors
    int16_t band_count = readPCNT(PCNT_ID_BAND);
    int16_t motor_count = readPCNT(PCNT_ID_MOTOR);
    
    // Get configuration values for both sensors
    const uint32_t BAND_PPR = (storedGlobals.PULSES_PER_REV > 0) ? storedGlobals.PULSES_PER_REV : 2;
    const uint32_t MOTOR_PPR = (storedGlobals.MOTOR_PULSES_PER_REV > 0) ? storedGlobals.MOTOR_PULSES_PER_REV : 12;
    const float BAND_BELT_M = (float)storedGlobals.BELT_DISTANCE_MM / 1000.0f;
    const float MOTOR_BELT_M = ((float)storedGlobals.BELT_DISTANCE_MM * storedGlobals.MOTOR_TO_BELT_RATIO) / 1000.0f;
    
    // Calculate raw RPM for both (simple estimation based on pulse count)
    // This is approximate - we don't have time delta here, so we use last known values
    float band_current_mps = 0.0f;
    float motor_current_mps = 0.0f;
    
    if (band_count > 0 && band_state.last_calculation_us > 0) {
        // Rough estimate: pulses * belt_m / PPR gives distance
        // We use a simple ratio compared to accumulated pulses
        const float band_ratio = (float)band_count / (float)BAND_PPR;
        band_current_mps = band_ratio * BAND_BELT_M * 2.0f; // *2 for 500ms window approximation
    }
    
    if (motor_count > 0 && motor_state.last_calculation_us > 0) {
        const float motor_ratio = (float)motor_count / (float)MOTOR_PPR;
        motor_current_mps = motor_ratio * MOTOR_BELT_M * 1.0f; // *1 for 1000ms window
    }
    
    // Calculate absolute change from last valid measurement
    float band_change = fabsf(band_current_mps - band_state.last_valid_mps);
    float motor_change = fabsf(motor_current_mps - motor_state.last_valid_mps);
    
    // Default to band if both are zero or equal
    bool use_motor = false;
    
    // If motor has valid data and is more stable, use motor
    if (motor_count > 0 && band_count > 0) {
        use_motor = (motor_change < band_change);
    } else if (motor_count > 0 && band_count == 0) {
        use_motor = true;
    }
    // else: use_motor = false (band is default)
    
    SensorSelection selection;
    if (use_motor) {
        selection.selected_unit = PCNT_ID_MOTOR;
        selection.pulse_count = motor_count;
        selection.ppr_config_value = storedGlobals.MOTOR_PULSES_PER_REV;
        selection.ppr_default = 12;
        selection.motor_to_belt_ratio = storedGlobals.MOTOR_TO_BELT_RATIO;
        selection.sensor_name = "MOTOR";
    } else {
        selection.selected_unit = PCNT_ID_BAND;
        selection.pulse_count = band_count;
        selection.ppr_config_value = storedGlobals.PULSES_PER_REV;
        selection.ppr_default = 2;
        selection.motor_to_belt_ratio = 1.0f;
        selection.sensor_name = "BAND";
    }
    
    return selection;
}

// Read PCNT counts based on sensor mode
// For AUTO mode: compares both sensors and returns the more stable one
SensorSelection readPCNTs(PcntUnitId current_unit, uint8_t sensor_mode, SensorState& band_state, SensorState& motor_state) {
    SensorSelection selection;
    
    if (sensor_mode == SENSOR_AUTO) {
        // AUTO mode: compare both sensors and select the stable one
        selection = selectStableSensor(band_state, motor_state);
    } else {
        // BAND or MOTOR mode: use the specified sensor
        int16_t count = readPCNT(current_unit);
        selection.selected_unit = current_unit;
        selection.pulse_count = count;
        
        if (current_unit == PCNT_ID_BAND) {
            // Band sensor
            selection.ppr_config_value = storedGlobals.PULSES_PER_REV;
            selection.ppr_default = 2;
            selection.motor_to_belt_ratio = 1.0f;
            selection.sensor_name = "BAND";
        } else {
            // Motor sensor
            selection.ppr_config_value = storedGlobals.MOTOR_PULSES_PER_REV;
            selection.ppr_default = 12;
            selection.motor_to_belt_ratio = storedGlobals.MOTOR_TO_BELT_RATIO;
            selection.sensor_name = "MOTOR";
        }
    }
    
    return selection;
}

// ============================================================================
// PLAUSIBILITY AND WARMUP HELPERS
// ============================================================================
// Plausibility check with warm-up handling
// Returns true if calculated speed change is plausible compared to last speed
bool checkSpeedPlausibility(
    float calculated_mps, 
    float last_valid_mps, 
    float measurement_secs,
    uint8_t& warmup_counter,
    const char* sensor_name
) {
    const uint8_t WARMUP_SAMPLES = 10;
    bool in_warmup = (warmup_counter < WARMUP_SAMPLES);
    
    // Starting from zero is always plausible
    if (last_valid_mps <= 0.1f) {
        return true;
    }
    
    float absolute_change = fabsf(calculated_mps - last_valid_mps);
    float change_ratio = absolute_change / last_valid_mps;
    
    bool plausible;
    
    if (in_warmup) {
        // During warm-up: allow much larger changes (filter is settling)
        // Allow up to 150% change or 2.0 m/s absolute change per second
        plausible = (absolute_change <= 2.0f * measurement_secs) || (change_ratio <= 1.5f);
    } else {
        // After warm-up: normal checks
        // Typical treadmill can accelerate ~0.5 m/s per second
        plausible = (absolute_change <= 0.8f * measurement_secs) || (change_ratio <= 0.6f);
    }
    
    return plausible;
}

// Increment warm-up counter
void updateWarmupCounter(uint8_t& warmup_counter, const char* sensor_name) {
    const uint8_t WARMUP_SAMPLES = 10;
    if (warmup_counter < WARMUP_SAMPLES + 1) {
        warmup_counter++;
    }
}

// ============================================================================
// SHARED SENSOR CALCULATION HELPER
// ============================================================================
// Common sensor calculation logic
// Returns calculation result; sensor-specific code handles metrics updates
SensorCalcResult calculateSensorSpeed(
    SensorState& state,
    uint32_t now_ms,
    uint32_t now_us,
    PcntUnitId pcnt_unit,
    uint32_t ppr,
    float belt_m,
    uint32_t min_window_us,
    const char* sensor_name,
    bool do_plausibility_check,
    int16_t pulse_count  // Now passed from readPCNTs
) {
    SensorCalcResult result = {false, false, 0.0f, 0.0f, 0.0f, 0, 0.0f, true};
    
    // Use pulse count passed from readPCNTs (already selected based on mode)
    int16_t current_pcnt = pulse_count;
    uint32_t read_time_us = now_us;
    int16_t pulse_delta = current_pcnt;
    
    if (!testdata) {
        clearPCNT(pcnt_unit);
    }
    
    state.accumulated_pulses += pulse_delta;
    state.last_pulse_read_us = read_time_us;
    
    const uint32_t MAXP_MS = storedGlobals.MAX_REVOLUTION_TIME_MS ? storedGlobals.MAX_REVOLUTION_TIME_MS : 2000;
    
    // Timeout check
    if (state.last_calculation_ms != 0) {
        uint32_t dt = now_ms - state.last_calculation_ms;
        if (dt > MAXP_MS) {
            result.timeout = true;
            state.last_valid_mps = 0.0f;
            state.last_calculation_ms = 0;
            state.last_calculation_us = 0;
            state.window_start_pulses = state.accumulated_pulses;
            return result;
        }
    }
    
    // First measurement initialization
    if (state.last_calculation_us == 0) {
        state.window_start_pulses = state.accumulated_pulses;
        state.last_calculation_ms = now_ms;
        state.last_calculation_us = now_us;
        return result;
    }
    
    // Calculate deltas
    uint32_t delta_us = microsDiff(now_us, state.last_calculation_us);
    int64_t delta_pulses = state.accumulated_pulses - state.window_start_pulses;
    
    // Check if window is long enough
    if (delta_us >= min_window_us) {
        // Handle zero-pulse window
        if (delta_pulses == 0) {
            return result;
        }
        
        // Calculate speed
        const float secs = (float)delta_us / 1e6f;
        const float revs = (float)delta_pulses / (float)ppr;
        const float rps = revs / secs;
        const float calculated_mps = rps * belt_m;
        const float calculated_rpm = rps * 60.0f;
        
        result.secs = secs;
        result.delta_pulses = delta_pulses;
        result.calculated_mps = calculated_mps;
        result.calculated_rpm = calculated_rpm;
        result.delta_m = revs * belt_m;
        result.valid = true;
        
        // Plausibility check
        if (do_plausibility_check) {
            result.plausible = checkSpeedPlausibility(calculated_mps, state.last_valid_mps, secs, state.warmup_counter, sensor_name);
        }
        
        // Update state for next calculation
        state.window_start_pulses = state.accumulated_pulses;
        state.last_calculation_ms = now_ms;
        state.last_calculation_us = read_time_us;
        
        if (result.plausible) {
            state.last_valid_mps = calculated_mps;
        }
    }
    
    return result;
}

// Reset sensor state
void resetSensorState(SensorState& state, PcntUnitId pcnt_unit) {
    state.accumulated_pulses = 0;
    state.window_start_pulses = 0;
    state.last_valid_mps = 0.0f;
    state.last_calculation_ms = 0;
    state.last_calculation_us = 0;
    state.last_pulse_read_us = 0;
    state.warmup_counter = 0;
    clearPCNT(pcnt_unit);
}

// ============================================================================
// GENERIC SENSOR METRICS UPDATE
// ============================================================================
void updateSensorMetrics(
    uint32_t now_ms, 
    uint32_t now_us,
    const SensorMetricsConfig& config,
    TreadmillMetrics& metrics,
    SpeedFilter& filter
) {
    static SensorState band_state = {0, 0, 0, 0, 0, 0.0f, 0};
    static SensorState motor_state = {0, 0, 0, 0, 0, 0.0f, 0};
    
    // Check if we need to reset after exiting test mode or sensor switch
    extern volatile bool metrics_need_reset;
    if (metrics_need_reset) {
        // Reset BOTH sensor states to ensure clean state when switching sensors
        resetSensorState(band_state, PCNT_ID_BAND);
        resetSensorState(motor_state, PCNT_ID_MOTOR);
        metrics_need_reset = false;
        metrics.rpm = 0;
        metrics.motorRPM = 0;
        metrics.mps = 0;
        metrics.mpsSmooth = 0;
    }
    
    // Read PCNT values - in AUTO mode this compares both sensors
    SensorSelection selection = readPCNTs(config.pcnt_unit, storedGlobals.SENSOR_SOURCE_MODE, band_state, motor_state);
    
    // Use selected sensor's configuration
    const uint32_t PPR = (selection.ppr_config_value > 0) ? selection.ppr_config_value : selection.ppr_default;
    const float belt_m = (config.belt_distance_mm * selection.motor_to_belt_ratio) / 1000.0f;
    const uint32_t MIN_WINDOW_US = 1000000UL;  // 1000ms for both sensors
    
    // Determine which state to use based on selection
    const bool is_band = (selection.selected_unit == PCNT_ID_BAND);
    SensorState& state = is_band ? band_state : motor_state;
    
    // Calculate using shared helper with selected sensor's pulse count
    SensorCalcResult result = calculateSensorSpeed(
        state, now_ms, now_us,
        selection.selected_unit,
        PPR,
        belt_m,
        MIN_WINDOW_US,
        selection.sensor_name,
        true,  // Enable plausibility checking
        selection.pulse_count  // Use selected sensor's count
    );
    
    // Handle timeout
    if (result.timeout) {
        metrics.rpm = 0;
        metrics.motorRPM = 0;
        metrics.mps = 0;
        metrics.mpsSmooth = 0;
        metrics.isRunning = false;
        return;
    }
    
    // No calculation this cycle
    if (!result.valid) {
        return;
    }
    
    // Update distance only if enabled (to prevent double-counting when both sensors run)
    if (config.track_distance && result.plausible) {
        metrics.workoutDistance += (uint32_t)(result.delta_m * 1000.0f);
    }
    
    // Update metrics if plausible
    if (result.plausible) {
        const float total_revisions = (float)state.accumulated_pulses / (float)PPR;
        
        if (is_band) {
            metrics.rpm = result.calculated_rpm;
            metrics.mps = result.calculated_mps;
            
            // If starting from zero, reset filter
            if (config.reset_filter_on_start && state.last_valid_mps <= 0.1f && result.calculated_mps > 0.1f) {
                filter.reset();
            }
            
            // Apply filter
            bool isDecelerating = (result.calculated_mps < state.last_valid_mps);
            metrics.mpsSmooth = filter.update(result.calculated_mps, isDecelerating);
        } else {
            metrics.motorRPM = result.calculated_rpm;
            metrics.mps = result.calculated_mps;
            
            // Apply filter
            bool isDecelerating = (result.calculated_mps < state.last_valid_mps);
            metrics.mpsSmooth = filter.update(result.calculated_mps, isDecelerating);
        }
        
        // Update isRunning based on actual sensor speed (threshold: 0.1 km/h ~ 0.028 m/s)
        metrics.isRunning = (metrics.mpsSmooth > 0.028f);
        
        // Update warm-up counter
        updateWarmupCounter(state.warmup_counter, config.sensor_name);
    }
}
