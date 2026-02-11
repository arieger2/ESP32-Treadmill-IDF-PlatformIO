/******************************************************************************
 * DUAL SPEED SENSOR - Public API sensor.h
 * 
 * Provides high-precision tachometer functionality using MCPWM capture,
 * PCNT hardware counting, and GPTimer timeout mechanisms.
 * 
 * Supports two independent sensors with dedicated hardware resources.
 ******************************************************************************/

#ifndef ESP32_TREADMILL_TACHO_SENSOR_NEW_H
#define ESP32_TREADMILL_TACHO_SENSOR_NEW_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/mcpwm_cap.h"
#include "driver/pulse_cnt.h"

// Include Arduino to ensure consistent FreeRTOS headers
#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif
extern portMUX_TYPE s_sensor_spinlock;


/* ==========================
 * Configuration constants
 * ========================== */
#define UPDATE_TIMEOUT_US     200000   // 200 ms max update interval
#define CAPTURE_RES_HZ        1000000  // 1 MHz - ESP32-S3 capture timer is ALWAYS APB_CLK (resolution_hz config ignored)

/* ==========================
 * Sensor type identifier
 * ========================== */
typedef enum {
    SENSOR_TYPE_BAND = 0,   // Belt/band sensor (sensor1)
    SENSOR_TYPE_MOTOR = 1   // Motor sensor (sensor2)
} sensor_type_t;

/* ==========================
 * Sensor result structure
 * ========================== */
typedef struct {
    float rpm;              // Rotational speed in revolutions per minute
    float mps;              // Linear speed in meters per second (belt_ratio already applied)
    float delta_distance;   // Distance traveled in meters since last measurement
    bool  has_new;          // True when fresh data (including forced zero) is available
    bool  force_reset;      // True when filters should reset (zero-speed timeout)
} sensor_result_t;

/* ==========================
 * Sensor structure
 * ========================== */
typedef struct {
    // Hardware handles
    pcnt_unit_handle_t         pcnt_unit;
    pcnt_channel_handle_t      pcnt_chan;
    
    // Configuration
    sensor_type_t sensor_type;   // Identifies if this is band or motor sensor
    int      gpio_num;
    uint32_t target_periods;
    
    // Captured timestamps
    volatile uint64_t t_last;     // last measurement timestamp
    
    // Published result (snapshot)
    volatile uint32_t used_periods; // how many periods used for last result
    volatile uint64_t period_us;
    volatile uint64_t sum_used_periods; // how many periods used for last result
    volatile uint64_t ts_us;
} speed_sensor_t;




/**
 * Initialize sensor 1 (GPIO 18, MCPWM Group 0)
 * 
 * @param initial_periods Number of pulses to count before triggering measurement
 * @param gpio_num GPIO pin number (use -1 to keep default GPIO 18)
 * @return ESP_OK on success, error code otherwise
 * 
 * Example:
 *   speed_sensor1_init(100, 18);  // Count 100 pulses on GPIO 18
 */
esp_err_t speed_sensor1_init(uint32_t initial_periods, int gpio_num);

/**
 * Initialize sensor 2 (GPIO 19, MCPWM Group 1)
 * 
 * @param initial_periods Number of pulses to count before triggering measurement
 * @param gpio_num GPIO pin number (use -1 to keep default GPIO 19)
 * @return ESP_OK on success, error code otherwise
 * 
 * Example:
 *   speed_sensor2_init(100, 19);  // Count 100 pulses on GPIO 19
 */
esp_err_t speed_sensor2_init(uint32_t initial_periods, int gpio_num);

/**
 * Dynamically adjust target pulse count for a sensor
 * 
 * Allows runtime adjustment of measurement precision/update rate tradeoff.
 * 
 * @param sensor Pointer to sensor structure (from speed_sensor_get_sensor1/2())
 * @param periods New target pulse count
 * @return ESP_OK on success, error code otherwise
 * 
 * Example:
 *   // Adjust sensor 1 to count 200 pulses instead of 100
 *   speed_sensor_set_target_pulses(speed_sensor_get_sensor1(), 200);
 */
esp_err_t speed_sensor_set_target_pulses(speed_sensor_t *sensor, uint32_t periods);

/**
 * Get RPM, speed (m/s) and delta distance from sensor
 *
 * Returns RPM, linear speed and distance traveled since last measurement.
 * Belt ratio is applied to both mps and delta_distance.
 *
 * @param sensor Pointer to sensor structure
 * @param pulses_per_rev Number of pulses in one complete revolution
 * @param belt_distance_mm Belt circumference in millimeters
 * @param belt_ratio Motor-to-belt ratio (1.0 for band sensor / direct drive)
 * @return sensor_result_t containing rpm, mps and delta_distance (in meters)
 */
sensor_result_t speed_sensor_get_rpm_and_delta(speed_sensor_t *sensor, uint32_t pulses_per_rev,
                                                 float belt_distance_mm, float belt_ratio);

/**
 * Get pointer to sensor 1 structure (BAND SENSOR)
 * 
 * Sensor 1 is always linked to the band/belt sensor
 * - GPIO 18
 * - MCPWM Group 0
 * - Measures belt movement directly
 * 
 * @return Pointer to sensor 1
 */
speed_sensor_t* speed_sensor_get_sensor1(void);

/**
 * Get pointer to sensor 2 structure (MOTOR SENSOR)
 * 
 * Sensor 2 is always linked to the motor sensor
 * - GPIO 19
 * - MCPWM Group 1
 * - Measures motor shaft rotation
 * 
 * @return Pointer to sensor 2
 */
speed_sensor_t* speed_sensor_get_sensor2(void);

#ifdef __cplusplus
}
#endif

// C++ only functions
#ifdef __cplusplus
struct TreadmillMetrics;
void updateMetrics(TreadmillMetrics& metrics);
void applySpeedFilter(TreadmillMetrics& metrics);
#endif

#endif // ESP32_TREADMILL_TACHO_SENSOR_NEW_H


