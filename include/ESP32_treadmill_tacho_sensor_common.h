// ============================================================================
// File   : ESP32_treadmill_tacho_sensor_common.h
// Purpose: Shared structures and functions for sensor calculations
// ============================================================================
#ifndef ESP32_TREADMILL_TACHO_SENSOR_COMMON_H
#define ESP32_TREADMILL_TACHO_SENSOR_COMMON_H

#include <stdint.h>
#include "driver/pulse_cnt.h"

// PCNT unit identifiers (replacing legacy pcnt_unit_t)
enum PcntUnitId : uint8_t {
    PCNT_ID_BAND = 0,
    PCNT_ID_MOTOR = 1
};

// Sensor state tracking
struct SensorState {
    uint32_t last_calculation_ms;
    uint32_t last_calculation_us;
    uint32_t last_pulse_read_us;
    int64_t accumulated_pulses;
    int64_t window_start_pulses;
    float last_valid_mps;
    uint8_t warmup_counter;
};

// Calculation result
struct SensorCalcResult {
    bool valid;              // True if calculation was performed
    bool timeout;            // True if sensor timed out
    float calculated_mps;    // Raw calculated speed
    float calculated_rpm;    // Raw calculated RPM
    float secs;              // Measurement window in seconds
    int64_t delta_pulses;    // Pulses in window
    float delta_m;           // Distance traveled
    bool plausible;          // Plausibility check result
};

// Forward declarations
class SpeedFilter;
struct TreadmillMetrics;

// Sensor metrics update parameters
struct SensorMetricsConfig {
    PcntUnitId pcnt_unit;
    uint32_t ppr_config_value;
    uint32_t ppr_default;
    float belt_distance_mm;
    float motor_to_belt_ratio;  // 1.0 for band, actual ratio for motor
    const char* sensor_name;
    bool reset_filter_on_start;  // true for band, false for motor
    bool track_distance;  // true to update workoutDistance (prevent double-counting)
};

// AUTO mode sensor selection result
struct SensorSelection {
    PcntUnitId selected_unit;        // PCNT_ID_BAND or PCNT_ID_MOTOR
    int16_t pulse_count;             // Pulse count from selected sensor
    uint32_t ppr_config_value;       // PPR from selected sensor
    uint32_t ppr_default;            // Default PPR for selected sensor
    float motor_to_belt_ratio;       // Ratio for selected sensor
    const char* sensor_name;         // "BAND" or "MOTOR"
};

// Function declarations
uint32_t microsDiff(uint32_t later, uint32_t earlier);
int16_t readPCNT(PcntUnitId unit);
SensorSelection readPCNTs(PcntUnitId current_unit, uint8_t sensor_mode, SensorState& band_state, SensorState& motor_state);
void clearPCNT(PcntUnitId unit);
bool checkSpeedPlausibility(float calculated_mps, float last_valid_mps, float measurement_secs, uint8_t& warmup_counter, const char* sensor_name);
void updateWarmupCounter(uint8_t& warmup_counter, const char* sensor_name);
SensorCalcResult calculateSensorSpeed(
    SensorState& state,
    uint32_t now_ms,
    uint32_t now_us,
    PcntUnitId pcnt_unit,
    uint32_t ppr,
    float belt_m,
    uint32_t min_window_us,
    const char* sensor_name,
    bool do_plausibility_check = true,
    int16_t pulse_count = 0
);
void resetSensorState(SensorState& state, PcntUnitId pcnt_unit);

// PCNT handle management
void initPCNTHandles();
pcnt_unit_handle_t getPCNTHandle(PcntUnitId unit);

// Generic sensor metrics update
void updateSensorMetrics(
    uint32_t now_ms, 
    uint32_t now_us,
    const SensorMetricsConfig& config,
    TreadmillMetrics& metrics,
    SpeedFilter& filter
);

#endif // ESP32_TREADMILL_TACHO_SENSOR_COMMON_H
