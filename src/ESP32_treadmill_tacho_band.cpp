// ============================================================================
// File   : ESP32_treadmill_tacho_band.cpp
// Purpose: Band sensor PCNT measurement and calculation
// ============================================================================
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_filters.h"
#include "ESP32_treadmill_tacho_sensor_common.h"

extern TreadmillMetrics metrics;
extern SpeedFilter bandFilter;
extern volatile bool testdata;
extern volatile bool metrics_need_reset;

// ============================================================================
// SPEED CALCULATION - Band Sensor
// ============================================================================  
void updateMetricsBand(uint32_t now_ms, uint32_t now_us) {
    // Track distance only if band is the active sensor (to prevent double-counting)
    const bool track_dist = (storedGlobals.SENSOR_SOURCE_MODE == SENSOR_BAND || 
                             storedGlobals.SENSOR_SOURCE_MODE == SENSOR_AUTO);
    
    SensorMetricsConfig config = {
        .pcnt_unit = PCNT_ID_BAND,
        .ppr_config_value = storedGlobals.PULSES_PER_REV,
        .ppr_default = 2,
        .belt_distance_mm = (float)storedGlobals.BELT_DISTANCE_MM,
        .motor_to_belt_ratio = 1.0f,  // No ratio for band sensor
        .sensor_name = "BAND",
        .reset_filter_on_start = true,  // Reset filter when starting from zero
        .track_distance = track_dist
    };
    
    updateSensorMetrics(now_ms, now_us, config, metrics, bandFilter);
}
