/******************************************************************************
 * DUAL SPEED SENSOR - Speed and Distance Calculations 
 * 
 * This file contains speed/RPM calculation functions for the dual speed
 * sensor system. Separated from the main sensor logic for better code
 * organization.
 * 
 * SENSOR MAPPING:
 * ===============
 * - s_sensor1 (speed_sensor_get_sensor1()) → BAND SENSOR (GPIO 18, MCPWM Group 0)
 * - s_sensor2 (speed_sensor_get_sensor2()) → MOTOR SENSOR (GPIO 19, MCPWM Group 1)
 * 
 * CALCULATION APPROACH:
 * =====================
 * - Uses hardware-captured pulse count and timing data
 * - Converts pulses + time → frequency → RPM → m/s
 * - Thread-safe access to sensor results via critical sections
 * - Maintains last valid RPM for each sensor independently
 * 
 * RPM CALCULATION FORMULA:
 * ========================
 * 1. Time in seconds = dt_ticks / CAPTURE_RES_HZ
 * 2. Frequency (Hz) = used_periods / seconds
 * 3. RPM = (Hz × 60) / pulses_per_rev
 * 
 * Example:
 * --------
 * - 100 pulses measured in 47,385 ticks
 * - Timer resolution: 10 MHz (10,000,000 Hz)
 * - Time = 47,385 / 10,000,000 = 0.0047385 seconds
 * - Frequency = 100 / 0.0047385 = 21,103 Hz
 * - If pulses_per_rev = 48: RPM = (21,103 × 60) / 48 = 26,379 RPM
 * 
 * THREAD SAFETY:
 * ==============
 * - Uses portENTER_CRITICAL/portEXIT_CRITICAL for atomic reads
 * - Clears new_result flag after reading to prevent re-processing
 * - Static variables maintain last valid value per sensor
 ******************************************************************************/

#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_sensor.h"

// External references to sensor instances (defined in sensor.cpp)
extern speed_sensor_t s_sensor1;
extern speed_sensor_t s_sensor2;

/**
 * Compute RPM using last published result
 * 
 * This function reads the latest measurement from a sensor and converts it
 * to RPM based on the pulses per revolution configuration.
 * 
 * @param sensor Pointer to sensor structure (from speed_sensor_get_sensor1/2())
 * @param pulses_per_rev Number of pulses in one complete revolution
 * @return RPM value (last valid measurement if no new data available)
 * 
 * USAGE EXAMPLES:
 * ---------------
 * // Belt sensor with 48 pulses per revolution
 * float rpm = speed_sensor_get_rpm(speed_sensor_get_sensor1(), 48);
 * 
 * // Motor sensor with 12 pulses per revolution
 * float rpm = speed_sensor_get_rpm(speed_sensor_get_sensor2(), 12);
 * 
 * BEHAVIOR:
 * ---------
 * - Returns 0.0 if sensor pointer is NULL
 * - Returns last valid RPM if no new measurement available
 * - Updates internal cache only when new measurement is available
 * - Thread-safe: uses critical section for atomic access
 * 
 * CALCULATION DETAILS:
 * --------------------
 * - Reads used_periods (pulse count) and dt_ticks (time in timer ticks)
 * - Converts to seconds using CAPTURE_RES_HZ (10 MHz = 0.1 µs precision)
 * - Calculates frequency in Hz
 * - Converts to RPM: (frequency × 60) / pulses_per_rev
 */
float speed_sensor_get_rpm(speed_sensor_t *sensor, uint32_t pulses_per_rev)
{
    if (sensor == NULL) return 0.0f;
    
    // Maintain separate static cache for each sensor
    static float last_rpm_s1 = 0.0f;
    static float last_rpm_s2 = 0.0f;
    float *last_rpm = (sensor == &s_sensor1) ? &last_rpm_s1 : &last_rpm_s2;

    bool has_new = false;
    uint32_t used = 0;
    uint32_t dt = 0;

    // Atomic read of sensor result
    portENTER_CRITICAL(&sensor->mux);
    has_new = sensor->new_result;
    if (has_new) {
        used = sensor->used_periods;
        dt = sensor->dt_ticks;
        sensor->new_result = false;  // Clear flag after reading
    }
    portEXIT_CRITICAL(&sensor->mux);

    // Calculate new RPM if we have valid new data
    if (has_new && dt > 0 && pulses_per_rev > 0) {
        // Step 1: Convert timer ticks to seconds
        // CAPTURE_RES_HZ = 10 MHz = 10,000,000 ticks per second
        // Example: 47,385 ticks / 10,000,000 = 0.0047385 seconds
        float seconds = (float)dt / (float)CAPTURE_RES_HZ;
        
        // Step 2: Calculate frequency in Hz
        // Example: 100 pulses / 0.0047385 s = 21,103 Hz
        float hz = (float)used / seconds;
        
        // Step 3: Convert to RPM
        // Example: (21,103 Hz × 60 s/min) / 48 pulses/rev = 26,379 RPM
        *last_rpm = (hz * 60.0f) / (float)pulses_per_rev;
    }

    // Return cached value (either newly calculated or last valid)
    return *last_rpm;
}

/**
 * Convert RPM to meters per second (m/s)
 * 
 * Converts rotational speed (RPM) to linear speed (m/s) based on
 * belt/wheel circumference.
 * 
 * @param rpm Rotational speed in revolutions per minute
 * @param belt_distance_mm Belt circumference in millimeters
 * @param motor_to_belt_ratio Motor to belt ratio for gear compensation
 * @return Linear speed in meters per second
 * 
 * USAGE EXAMPLES:
 * ---------------
 * // Band sensor with 1600mm belt circumference
 * float mps = speed_sensor_get_mps(rpm, 1600.0f, 1.0f);
 * 
 * // Motor sensor - needs to account for belt ratio
 * float mps = speed_sensor_get_mps(motorRPM, belt_mm, motor_to_belt_ratio);
 * 
 * CALCULATION:
 * ------------
 * - Belt distance per revolution = belt_distance_mm / 1000 (convert to meters)
 * - Revolutions per second = rpm * motor_to_belt_ratio / 60
 * - Speed (m/s) = (belt_distance_mm / 1000) × (rpm * motor_to_belt_ratio / 60)
 * 
 * Example:
 * --------
 * - Belt: 1600 mm = 1.6 meters
 * - RPM: 100
 * - Ratio: 0.5
 * - Speed = 1.6 × (100 * 0.5 / 60) = 1.33 m/s
 */
float speed_sensor_get_mps(float rpm, float belt_distance_mm, float motor_to_belt_ratio) {
    if (rpm < 0.0f) return 0.0f;
    if (belt_distance_mm <= 0.0f) return 0.0f;
    
    // Convert belt distance from mm to meters
    float belt_distance_m = belt_distance_mm / 1000.0f;
    
    // Convert RPM to revolutions per second
    float revolutions_per_second = rpm * motor_to_belt_ratio / 60.0f;
    
    // Calculate linear speed: distance per revolution × revolutions per second
    float mps = belt_distance_m * revolutions_per_second;
    
    return mps;
}

/**
 * Get pointer to sensor 1 structure (BAND SENSOR)
 * 
 * Provides access to the sensor 1 instance for use with other API functions.
 * 
 * @return Pointer to sensor 1 (GPIO 18, MCPWM Group 0)
 * 
 * USAGE:
 * ------
 * speed_sensor_t *sensor = speed_sensor_get_sensor1();
 * float rpm = speed_sensor_get_rpm(sensor, 48);
 */
speed_sensor_t* speed_sensor_get_sensor1(void) {
    return &s_sensor1;
}

/**
 * Get pointer to sensor 2 structure
 * 
 * Provides access to the sensor 2 instance for use with other API functions.
 * 
 * @return Pointer to sensor 2 (GPIO 19, MCPWM Group 1)
 * 
 * USAGE:
 * ------
 * speed_sensor_t *sensor = speed_sensor_get_sensor2();
 * float rpm = speed_sensor_get_rpm(sensor, 12);
 */
speed_sensor_t* speed_sensor_get_sensor2(void) {
    return &s_sensor2;
}

void updateMetrics(TreadmillMetrics& metrics) {
    if (storedGlobals.SENSOR_SOURCE_MODE == SENSOR_BAND) {
        metrics.rpm = speed_sensor_get_rpm(speed_sensor_get_sensor1(), storedGlobals.PULSES_PER_REV);
        metrics.mps = speed_sensor_get_mps(metrics.rpm, storedGlobals.BELT_DISTANCE_MM, 1.0f);
    } else if (storedGlobals.SENSOR_SOURCE_MODE == SENSOR_MOTOR) {
        metrics.motorRPM = speed_sensor_get_rpm(speed_sensor_get_sensor2(), storedGlobals.MOTOR_PULSES_PER_REV);
        metrics.mps = speed_sensor_get_mps(metrics.motorRPM, storedGlobals.BELT_DISTANCE_MM, storedGlobals.MOTOR_TO_BELT_RATIO);
    } else {
        // AUTO mode - prefer motor for stability
        metrics.rpm = speed_sensor_get_rpm(speed_sensor_get_sensor1(), storedGlobals.PULSES_PER_REV);
        metrics.motorRPM = speed_sensor_get_rpm(speed_sensor_get_sensor2(), storedGlobals.MOTOR_PULSES_PER_REV);
        metrics.mps = speed_sensor_get_mps(metrics.motorRPM, storedGlobals.BELT_DISTANCE_MM, storedGlobals.MOTOR_TO_BELT_RATIO);
    }
    //metrics.workoutDistance += revs * belt_m// change distance to float
    //metrics.workoutDistance += (uint32_t)(result.delta_m * 1000.0f);
    //metrics.mps  my be not needed anymore, need only delta distance
}
