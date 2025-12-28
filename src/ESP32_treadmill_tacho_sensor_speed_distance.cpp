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
 * Get RPM and delta distance from sensor
 * 
 * Returns both RPM and distance traveled since last measurement.
 * This function combines RPM calculation with distance measurement in a single
 * atomic sensor read operation.
 * 
 * @param sensor Pointer to sensor structure
 * @param pulses_per_rev Number of pulses in one complete revolution
 * @param belt_distance_mm Belt circumference in millimeters
 * @param belt_ratio Motor-to-belt ratio (1.0 for direct drive)
 * @return sensor_result_t containing rpm and delta_distance (in meters)
 * 
 * USAGE EXAMPLES:
 * ---------------
 * // Band sensor with 48 pulses/rev, 1600mm belt
 * sensor_result_t result = speed_sensor_get_rpm_and_delta(sensor1, 48, 1600.0f, 1.0f);
 * 
 * // Motor sensor with 12 pulses/rev, 1600mm belt, 0.5 ratio
 * sensor_result_t result = speed_sensor_get_rpm_and_delta(sensor2, 12, 1600.0f, 0.5f);
 * 
 * BEHAVIOR:
 * ---------
 * - Returns {0.0, 0.0} if sensor pointer is NULL
 * - Returns last valid RPM with 0.0 distance if no new measurement
 * - Returns updated RPM and distance when new measurement available
 * - Thread-safe: uses critical section for atomic access
 */
sensor_result_t speed_sensor_get_rpm_and_delta(speed_sensor_t *sensor, uint32_t pulses_per_rev, 
                                                 float belt_distance_mm, float belt_ratio)
{
    sensor_result_t result = {0.0f, 0.0f};
    
    if (sensor == NULL) return result;
    
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

    // Calculate new RPM and distance if we have valid new data
    if (has_new && dt > 0 && pulses_per_rev > 0) {
        // Step 1: Convert timer ticks to seconds
        float seconds = (float)dt / (float)CAPTURE_RES_HZ;
        
        // Step 2: Calculate frequency in Hz
        float hz = (float)used / seconds;
        
        // Step 3: Convert to RPM
        *last_rpm = (hz * 60.0f) / (float)pulses_per_rev;
        
        // Step 4: Calculate delta distance in meters
        // revolutions = pulses / pulses_per_rev
        // distance = belt_circumference × revolutions × gear_ratio
        float revolutions = (float)used / (float)pulses_per_rev;
        float distance_mm = belt_distance_mm * revolutions * belt_ratio;
        result.delta_distance = distance_mm / 1000.0f;  // Convert mm to meters
    }
    
    // Always return current RPM (cached or newly calculated)
    result.rpm = *last_rpm;
    
    return result;
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

void updateMetrics(TreadmillMetrics& metrics, speed_sensor_t *sensor) {
    if (sensor == NULL) return;
    
    // Determine mode and validate sensor matches the mode
    uint8_t mode = storedGlobals.SENSOR_SOURCE_MODE;
    uint8_t motorPulsesPerRev = storedGlobals.MOTOR_PULSES_PER_REV;
    uint8_t bandPulsesPerRev = storedGlobals.PULSES_PER_REV;
    uint8_t beltDistanceMM = storedGlobals.BELT_DISTANCE_MM;
    uint8_t motorToBeltRatio = storedGlobals.MOTOR_TO_BELT_RATIO;
    
    if (mode == SENSOR_AUTO) {
        // AUTO mode: update both band and motor metrics
        // Use motor sensor for speed calculation (more stable)
        sensor_result_t motorResult = speed_sensor_get_rpm_and_delta(sensor, motorPulsesPerRev, 
                                                                       beltDistanceMM, motorToBeltRatio);
        metrics.motorRPM = motorResult.rpm;
        metrics.workoutDistance += motorResult.delta_distance;
        
        sensor_result_t bandResult = speed_sensor_get_rpm_and_delta(speed_sensor_get_sensor1(), bandPulsesPerRev, 
                                                                      beltDistanceMM, 1.0f);
        metrics.rpm = bandResult.rpm;
        metrics.mps = speed_sensor_get_mps(motorResult.rpm, beltDistanceMM, motorToBeltRatio);
        
    } else if (mode == SENSOR_BAND) {
        // BAND mode: only update if sensor is actually the band sensor
        if (sensor->sensor_type == SENSOR_TYPE_BAND) {
            sensor_result_t result = speed_sensor_get_rpm_and_delta(sensor, bandPulsesPerRev, 
                                                                      beltDistanceMM, 1.0f);
            metrics.rpm = result.rpm;
            metrics.workoutDistance += result.delta_distance;
            metrics.mps = speed_sensor_get_mps(result.rpm, beltDistanceMM, 1.0f);
        }
        // else: sensor is motor but mode is band - do nothing
    } else if (mode == SENSOR_MOTOR) {
        // MOTOR mode: only update if sensor is actually the motor sensor
        if (sensor->sensor_type == SENSOR_TYPE_MOTOR) {
            sensor_result_t result = speed_sensor_get_rpm_and_delta(sensor, motorPulsesPerRev, 
                                                                      beltDistanceMM, motorToBeltRatio);
            metrics.motorRPM = result.rpm;
            metrics.workoutDistance += result.delta_distance;
            metrics.mps = speed_sensor_get_mps(result.rpm, beltDistanceMM, motorToBeltRatio);
        }
        // else: sensor is band but mode is motor - do nothing
    }
}
