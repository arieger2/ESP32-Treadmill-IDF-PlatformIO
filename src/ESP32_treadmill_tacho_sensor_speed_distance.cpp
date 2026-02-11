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

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_sensor.h"
#include "ESP32_treadmill_tacho_filters.h"

// External references to sensor instances (defined in sensor.cpp)
extern speed_sensor_t s_sensor1;
extern speed_sensor_t s_sensor2;

// Static filter instances (one for band, one for motor)
static SpeedFilter bandFilter;
static SpeedFilter motorFilter;
// Flags let applySpeedFilter() reset smoothing when a zero-speed timeout fires
static volatile bool bandFilterResetPending = false;
static volatile bool motorFilterResetPending = false;


/**
 * Get RPM, speed (m/s) and delta distance from sensor
 *
 * Reads accumulated pulse data from sensor, calculates RPM, linear speed
 * and distance traveled since last call. Belt ratio is applied uniformly
 * to both mps and delta_distance - no separate conversion needed.
 *
 * @param sensor Pointer to sensor structure
 * @param pulses_per_rev Number of pulses in one complete revolution
 * @param belt_distance_mm Belt circumference in millimeters
 * @param belt_ratio Motor-to-belt gear ratio (1.0 for band sensor / direct drive)
 * @return sensor_result_t with rpm, mps, delta_distance, has_new, force_reset
 */
sensor_result_t speed_sensor_get_rpm_and_delta(speed_sensor_t *sensor, uint32_t pulses_per_rev,
                                                 float belt_distance_mm, float belt_ratio)
{
    sensor_result_t result = {0.0f, 0.0f, 0.0f, false, false};

    if (sensor == NULL) return result;

    // Per-sensor static state (RPM cache + accumulator bookkeeping)
    float *last_rpm;
    uint64_t *last_ts;
    uint64_t *last_sum;

    if (sensor == &s_sensor1) {
        static float s1_rpm = 0.0f;
        static uint64_t s1_ts = 0, s1_sum = 0;
        last_rpm = &s1_rpm; last_ts = &s1_ts; last_sum = &s1_sum;
    } else {
        static float s2_rpm = 0.0f;
        static uint64_t s2_ts = 0, s2_sum = 0;
        last_rpm = &s2_rpm; last_ts = &s2_ts; last_sum = &s2_sum;
    }

    // --- Atomic read of accumulated sensor data ---
    uint32_t used = 0;
    uint64_t dt = 0;
    bool zero_pending = false;

    portENTER_CRITICAL(&s_sensor_spinlock);
    if (sensor->used_periods) {
        used = sensor->sum_used_periods - *last_sum;
        if (used > 0) {
            dt = (*last_ts == 0) ? sensor->period_us
                                 : (sensor->ts_us - *last_ts);
            if (dt == 0 || sensor->period_us == 0)
                zero_pending = true;
        }
    }
    portEXIT_CRITICAL(&s_sensor_spinlock);

    // --- Zero-speed timeout ---
    if (zero_pending) {
        *last_rpm = 0.0f;
        result.has_new = true;
        result.force_reset = true;
        return result;
    }

    // --- New data: calculate RPM, mps, distance ---
    if (used > 0 && dt > 0 && pulses_per_rev > 0) {
        float seconds = (float)dt / (float)CAPTURE_RES_HZ;
        float hz = (float)used / seconds;
        *last_rpm = (hz * 60.0f) / (float)pulses_per_rev;

        result.rpm = *last_rpm;
        result.mps = (belt_distance_mm / 1000.0f) * (*last_rpm * belt_ratio / 60.0f);

        float revolutions = (float)used / (float)pulses_per_rev;
        result.delta_distance = belt_distance_mm * revolutions * belt_ratio / 1000.0f;
        result.has_new = true;

        *last_sum = sensor->sum_used_periods;
        *last_ts  = sensor->ts_us;
    } else {
        // No new data - return cached RPM and mps
        result.rpm = *last_rpm;
        result.mps = (belt_distance_mm / 1000.0f) * (*last_rpm * belt_ratio / 60.0f);
    }
    return result;
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
    uint8_t mode = storedGlobals.SENSOR_SOURCE_MODE;
    bool isBand = (mode == SENSOR_BAND);

    // Primary sensor: band in BAND mode, motor in MOTOR/AUTO mode
    speed_sensor_t *sensor = isBand ? speed_sensor_get_sensor1() : speed_sensor_get_sensor2();
    uint32_t ppr            = isBand ? storedGlobals.PULSES_PER_REV : storedGlobals.MOTOR_PULSES_PER_REV;
    float    ratio          = isBand ? 1.0f : storedGlobals.MOTOR_TO_BELT_RATIO;

    sensor_result_t result = speed_sensor_get_rpm_and_delta(sensor, ppr,
                                storedGlobals.BELT_DISTANCE_MM, ratio);

    if (result.force_reset) {
        metrics.rpm = 0.0f;
        metrics.motorRPM = 0.0f;
        metrics.mps = 0.0f;
        (isBand ? bandFilterResetPending : motorFilterResetPending) = true;
    } else {
        metrics.rpm      = isBand ? result.rpm : 0.0f;
        metrics.motorRPM = isBand ? 0.0f : result.rpm;
        metrics.mps      = result.mps;
        if (result.delta_distance > 0.0f)
            metrics.workoutDistance += result.delta_distance;
    }

    // AUTO mode: also read band sensor for band RPM display
    if (mode == SENSOR_AUTO) {
        sensor_result_t band = speed_sensor_get_rpm_and_delta(
            speed_sensor_get_sensor1(), storedGlobals.PULSES_PER_REV,
            storedGlobals.BELT_DISTANCE_MM, 1.0f);
        metrics.rpm = band.force_reset ? 0.0f : band.rpm;
    }
}

/**
 * Apply speed filtering to metrics
 * 
 * This function should be called from loop() context, NOT from ISR!
 * Applies the configured filter type to smooth the speed measurements.
 * 
 * @param metrics Reference to metrics structure to update
 */
void applySpeedFilter(TreadmillMetrics& metrics) {
    uint8_t mode = storedGlobals.SENSOR_SOURCE_MODE;
    
    // Select appropriate filter based on sensor mode
    SpeedFilter* activeFilter = (mode == SENSOR_BAND) ? &bandFilter : &motorFilter;
    SpeedFilterType filterType = (mode == SENSOR_BAND) 
        ? static_cast<SpeedFilterType>(storedGlobals.BAND_FILTER_TYPE)
        : static_cast<SpeedFilterType>(storedGlobals.MOTOR_FILTER_TYPE);
    
    activeFilter->setFilterType(filterType);
    if (mode == SENSOR_BAND) {
        if (bandFilterResetPending) {
            activeFilter->reset();
            metrics.mpsSmooth = metrics.mps;
            bandFilterResetPending = false;
        }
    } else {
        if (motorFilterResetPending) {
            activeFilter->reset();
            metrics.mpsSmooth = metrics.mps;
            motorFilterResetPending = false;
        }
    }
    bool isDecelerating = (metrics.mps < metrics.mpsSmooth);
    metrics.mpsSmooth = activeFilter->update(metrics.mps, isDecelerating);
}
