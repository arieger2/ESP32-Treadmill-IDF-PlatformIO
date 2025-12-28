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

/* ==========================
 * Configuration constants
 * ========================== */
#define UPDATE_TIMEOUT_US     200000   // 200 ms max update interval
#define CAPTURE_RES_HZ        10000000  // 10 MHz capture resolution

/* ==========================
 * Sensor structure
 * ========================== */
typedef struct {
    // Hardware handles
    mcpwm_cap_timer_handle_t   cap_timer;
    mcpwm_cap_channel_handle_t cap_chan;
    pcnt_unit_handle_t         pcnt_unit;
    pcnt_channel_handle_t      pcnt_chan;
    
    // Configuration
    int      gpio_num;
    int      mcpwm_group_id;     // 0 or 1 for ESP32-S3
    uint32_t target_periods;
    
    // Measurement control
    volatile bool     armed;      // waiting for first edge to start measurement window
    volatile bool     running;    // measurement active (pcnt counting)
    
    // Captured timestamps (hardware latched)
    volatile uint32_t t_start;    // capture tick at first edge (window start)
    volatile uint32_t t_last;     // last capture tick seen (always updated on cap event)
    
    // Published result (snapshot)
    volatile bool     new_result;
    volatile uint32_t used_periods; // how many periods used for last result
    volatile uint32_t dt_ticks;     // delta time in capture ticks for last result
    
    portMUX_TYPE mux;
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
 * Compute RPM from last measurement
 * 
 * @param sensor Pointer to sensor structure
 * @param pulses_per_rev Number of pulses per complete revolution
 * @return RPM value (0.0 if no new data available)
 * 
 * Example:
 *   float rpm = speed_sensor_get_rpm(speed_sensor_get_sensor1(), 48);
 */
float speed_sensor_get_rpm(speed_sensor_t *sensor, uint32_t pulses_per_rev);

/**
 * Convert RPM to meters per second (m/s)
 * 
 * Converts rotational speed (RPM) to linear speed (m/s) based on
 * belt/wheel circumference and motor-to-belt ratio.
 * 
 * @param rpm Rotational speed in revolutions per minute
 * @param belt_distance_mm Belt/wheel circumference in millimeters
 * @param motor_to_belt_ratio Gear ratio compensation (1.0 for direct drive)
 * @return Linear speed in meters per second
 * 
 * Example:
 *   // Band sensor (direct drive, ratio = 1.0)
 *   float mps = speed_sensor_get_mps(100.0f, 1600.0f, 1.0f);
 *   
 *   // Motor sensor (with gear ratio compensation)
 *   float mps = speed_sensor_get_mps(motorRPM, 1600.0f, 0.5f);
 */
float speed_sensor_get_mps(float rpm, float belt_distance_mm, float motor_to_belt_ratio);

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

#endif // ESP32_TREADMILL_TACHO_SENSOR_NEW_H


