// ============================================================================
// File   : ESP32_treadmill_tacho_motor.cpp
// Purpose: Motor sensor PCNT measurement and calculation
// ============================================================================
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_filters.h"
#include "ESP32_treadmill_tacho_sensor_common.h"

extern TreadmillMetrics metrics;
extern SpeedFilter motorFilter;
extern volatile bool testdata;
extern volatile bool metrics_need_reset;

// ============================================================================
// SPEED CALCULATION - Motor Sensor
// ============================================================================
void updateMetricsMotor(uint32_t now_ms, uint32_t now_us) {
    // Track distance only if motor is the active sensor (to prevent double-counting)
    const bool track_dist = (storedGlobals.SENSOR_SOURCE_MODE == SENSOR_MOTOR);
    
    SensorMetricsConfig config = {
        .pcnt_unit = PCNT_ID_MOTOR,
        .ppr_config_value = storedGlobals.MOTOR_PULSES_PER_REV,
        .ppr_default = 12,
        .belt_distance_mm = (float)storedGlobals.BELT_DISTANCE_MM,
        .motor_to_belt_ratio = storedGlobals.MOTOR_TO_BELT_RATIO,
        .sensor_name = "MOTOR",
        .reset_filter_on_start = false,  // Don't reset motor filter on start
        .track_distance = track_dist
    };
    
    updateSensorMetrics(now_ms, now_us, config, metrics, motorFilter);
}
