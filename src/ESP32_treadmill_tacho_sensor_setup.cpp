/******************************************************************************
 * DUAL SPEED SENSOR - Initialization and Setup
 * 
 * This file contains hardware initialization for the dual speed sensor system.
 * Separated from the main sensor logic for better code organization.
 * 
 * INITIALIZATION FLOW:
 * ====================
 * 1. sensor_init_internal() - Sets up MCPWM capture + PCNT for one sensor
 * 2. speed_sensor1_init() / speed_sensor2_init() - Public API functions
 * 3. Creates shared GPTimer for timeout monitoring (created once)
 * 
 * HARDWARE RESOURCES INITIALIZED:
 * ================================
 * - MCPWM Capture Timer (10 MHz resolution for timestamps)
 * - MCPWM Capture Channel (GPIO edge detection)
 * - PCNT Unit (hardware pulse counter)
 * - PCNT Channel (connected to GPIO)
 * - GPTimer (1 MHz resolution, shared 200ms timeout watchdog)
 * 
 * MCPWM GROUP ALLOCATION:
 * =======================
 * - Sensor 1: MCPWM Group 0
 * - Sensor 2: MCPWM Group 1
 * This prevents resource conflicts between sensors
 ******************************************************************************/

#include "driver/pulse_cnt.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "ESP32_treadmill_tacho_sensor.h"

static const char *TAG = "SENSOR_SETUP";

// External reference to global settings (defined in tacho_setup.cpp)
extern "C" {
    struct TreadmillStoredGlobals {
        uint8_t SENSOR_SOURCE_MODE;
        uint32_t DEBOUNCE_THRESHOLD_US;
        // ... other fields not needed here
    };
    extern TreadmillStoredGlobals storedGlobals;
}

// External references to sensors and timer (defined in sensor_new.cpp)
extern speed_sensor_t s_sensor1;
extern speed_sensor_t s_sensor2;
extern gptimer_handle_t s_timestamp_timer;

// External callback references (defined in sensor_new.cpp)
extern bool on_pcnt_reach_cb(pcnt_unit_handle_t unit,
                             const pcnt_watch_event_data_t *edata,
                             void *user_data);

/* ==========================
 * Private helper: Initialize one sensor
 * 
 * WHAT THIS DOES:
 * ---------------
 * 1. Creates MCPWM capture timer (10 MHz precision)
 * 2. Creates MCPWM capture channel (GPIO edge detection)
 * 3. Registers on_capture_cb callback (updates timestamps on every edge)
 * 4. Creates PCNT unit (hardware pulse counter)
 * 5. Creates PCNT channel (connected to same GPIO)
 * 6. Configures PCNT to count rising edges
 * 7. Sets watchpoint at N pulses (triggers on_pcnt_reach_cb)
 * 8. Enables all hardware and starts capture timer
 * 
 * PARAMETERS:
 * -----------
 * sensor - Pointer to sensor struct (s_sensor1 or s_sensor2)
 * initial_periods - Number of pulses to count before callback (e.g., 100)
 * 
 * RETURN:
 * -------
 * ESP_OK on success, ESP_ERR_* on failure
 * ========================== */
static esp_err_t sensor_init_internal(speed_sensor_t *sensor, uint32_t initial_periods)
{
    if (sensor == NULL) return ESP_ERR_INVALID_ARG;

    sensor->target_periods = initial_periods;

    // Validate target pulse count to avoid invalid PCNT configuration ranges
    if (sensor->target_periods == 0 || sensor->target_periods > 32767) {
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- Shared timestamp timer (free-running) ---- */
    if (s_timestamp_timer == NULL) {
        gptimer_config_t tcfg = {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,
            .direction = GPTIMER_COUNT_UP,
            .resolution_hz = CAPTURE_RES_HZ,
        };
        ESP_RETURN_ON_ERROR(gptimer_new_timer(&tcfg, &s_timestamp_timer), TAG, "new ts timer");
        ESP_RETURN_ON_ERROR(gptimer_enable(s_timestamp_timer), TAG, "ts enable");
        ESP_RETURN_ON_ERROR(gptimer_start(s_timestamp_timer), TAG, "ts start");
        ESP_LOGI(TAG, "Shared timestamp timer started (free-running)");
    }

    /* ---- PCNT (Pulse Counter) ---- */
    {
        // PCNT on ESP32-S3 expects a symmetric counting window (low < 0 < high)
        // even if we only increment on positive edges. Give the counter a small
        // negative range so the driver accepts the configuration while the
        // channel setup prevents ever going below zero.
        pcnt_unit_config_t ucfg = {
            .low_limit = -(int)sensor->target_periods,
            .high_limit = (int)sensor->target_periods,
        };
        ESP_RETURN_ON_ERROR(pcnt_new_unit(&ucfg, &sensor->pcnt_unit), TAG, "new pcnt unit");

        // Configure glitch filter (hardware debounce) - clamp to hardware max (12787 ns)
        uint32_t debounce_ns = storedGlobals.DEBOUNCE_THRESHOLD_US * 1000;
        if (debounce_ns > 12787) debounce_ns = 12787;
        pcnt_glitch_filter_config_t filter_config = {
            .max_glitch_ns = debounce_ns,
        };
        ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(sensor->pcnt_unit, &filter_config), TAG, "glitch filter");

        pcnt_chan_config_t chcfg = {
            .edge_gpio_num = sensor->gpio_num,
            .level_gpio_num = -1,  // Not used
        };
        ESP_RETURN_ON_ERROR(pcnt_new_channel(sensor->pcnt_unit, &chcfg, &sensor->pcnt_chan), TAG, "new pcnt chan");

        // Count only rising edges so pulses_per_rev matches physical magnets
        ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(sensor->pcnt_chan,
                    PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                    PCNT_CHANNEL_EDGE_ACTION_HOLD), TAG, "edge action");
        ESP_RETURN_ON_ERROR(pcnt_channel_set_level_action(sensor->pcnt_chan,
                                                         PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                         PCNT_CHANNEL_LEVEL_ACTION_KEEP), TAG, "level action");

        // Set watchpoint to trigger callback when count reaches N
        ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(sensor->pcnt_unit, (int)initial_periods), TAG, "add watchpoint");

        pcnt_event_callbacks_t pcbs = {
            .on_reach = on_pcnt_reach_cb,
        };
        ESP_RETURN_ON_ERROR(pcnt_unit_register_event_callbacks(sensor->pcnt_unit, &pcbs, sensor), TAG, "pcnt cbs");

        ESP_RETURN_ON_ERROR(pcnt_unit_enable(sensor->pcnt_unit), TAG, "pcnt enable");
        pcnt_unit_start(sensor->pcnt_unit); // Start counting immediately
    }


    ESP_LOGI(TAG, "Sensor GPIO%d initialized. Target periods=%u", 
             sensor->gpio_num, initial_periods);
    return ESP_OK;
}

/* ==========================
 * Public API: Initialize Sensor 1
 * 
 * USAGE:
 * ------
 * speed_sensor1_init(100, 18);  // Count 100 pulses on GPIO 18
 * 
 * PARAMETERS:
 * -----------
 * initial_periods - Number of pulses to count (e.g., 100)
 * gpio_num - GPIO pin number (e.g., 18), or -1 to use default
 * 
 * CREATES SHARED TIMEOUT TIMER:
 * ------------------------------
 * First call to either sensor1_init() or sensor2_init() creates
 * the shared GPTimer that monitors both sensors for timeouts.
 * Subsequent calls reuse the existing timer.
 * ========================== */
esp_err_t speed_sensor1_init(uint32_t initial_periods, int gpio_num)
{
    s_sensor1.sensor_type = SENSOR_TYPE_BAND;  // Mark as band sensor
    
    if (gpio_num >= 0) {
        s_sensor1.gpio_num = gpio_num;
    }
    
    esp_err_t ret = sensor_init_internal(&s_sensor1, initial_periods);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

/* ==========================
 * Public API: Initialize Sensor 2
 * 
 * USAGE:
 * ------
 * speed_sensor2_init(100, 19);  // Count 100 pulses on GPIO 19
 * 
 * PARAMETERS:
 * -----------
 * initial_periods - Number of pulses to count (e.g., 100)
 * gpio_num - GPIO pin number (e.g., 19), or -1 to use default
 * 
 * SHARES TIMEOUT TIMER:
 * ---------------------
 * Uses the same GPTimer as sensor1 for timeout monitoring.
 * If timer doesn't exist yet, creates it.
 * ========================== */
esp_err_t speed_sensor2_init(uint32_t initial_periods, int gpio_num)
{
    s_sensor2.sensor_type = SENSOR_TYPE_MOTOR;  // Mark as motor sensor
    
    if (gpio_num >= 0) {
        s_sensor2.gpio_num = gpio_num;
    }
    
    esp_err_t ret = sensor_init_internal(&s_sensor2, initial_periods);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

/* ==========================
 * Public API: Adjust target pulse count dynamically
 * 
 * USAGE:
 * ------
 * // Change sensor 1 to count 200 pulses instead of 100
 * speed_sensor_set_target_pulses(speed_sensor_get_sensor1(), 200);
 * 
 * USE CASES:
 * ----------
 * - Speed-adaptive measurement (fewer pulses at high speed)
 * - Testing different configurations without recompiling
 * 
 * PARAMETERS:
 * -----------
 * sensor - Pointer to sensor (from speed_sensor_get_sensor1/2())
 * periods - New pulse count target (must be > 0)
 * 
 * RETURN:
 * -------
 * ESP_OK on success
 * ESP_ERR_INVALID_ARG if sensor is NULL or periods is 0
 * ESP_ERR_INVALID_STATE if PCNT unit not initialized
 * ========================== */
esp_err_t speed_sensor_set_target_pulses(speed_sensor_t *sensor, uint32_t periods)
{
    if (sensor == NULL) return ESP_ERR_INVALID_ARG;
    if (periods == 0) return ESP_ERR_INVALID_ARG;
    if (sensor->pcnt_unit == NULL) return ESP_ERR_INVALID_STATE;

    sensor->target_periods = periods;

    // Update PCNT watchpoint (remove old, add new)
    ESP_RETURN_ON_ERROR(pcnt_unit_remove_watch_point(sensor->pcnt_unit, (int)sensor->target_periods), TAG, "rm wp");
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(sensor->pcnt_unit, (int)periods), TAG, "add wp");

    ESP_LOGI(TAG, "Sensor GPIO%d target pulses updated to %u", sensor->gpio_num, periods);
    return ESP_OK;
}

/* ==========================
 * Sensor Selection Function
 * 
 * Returns the configured sensor mode from global settings.
 * With PCNT, both sensors are always counting in hardware.
 * This function reports which sensor the software will use for speed calculations.
 * 
 * PARAMETERS:
 * -----------
 * init - Unused, kept for API compatibility
 * 
 * RETURN:
 * -------
 * SENSOR_BAND (0), SENSOR_MOTOR (1), or SENSOR_AUTO (2)
 * ========================== */
uint8_t sensorSelection(bool init) {
    (void)init;  // Unused parameter, kept for API compatibility
    return storedGlobals.SENSOR_SOURCE_MODE;
}
