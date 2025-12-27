// ============================================================================
// File   : ESP32_treadmill_tacho_sensor.cpp
// Purpose: Sensor selection and mode handling
// ============================================================================
#include "ESP32_treadmill_tacho_config.h"

// ============================================================================
// SENSOR SELECTION (now just returns the mode, PCNT handles both sensors)
// ============================================================================
uint8_t sensorSelection(bool init) {
    // With PCNT, both sensors are always counting in hardware
    // This function now just reports which sensor the software will use for speed
    return storedGlobals.SENSOR_SOURCE_MODE;
}
