/******************************************************************************
 * DUAL SPEED SENSOR - High-Precision Tachometer with Hardware Counting
 * 
 * ARCHITECTURE OVERVIEW:
 * ======================
 * Uses three ESP32 hardware peripherals working together:
 * 
 * 1. MCPWM CAPTURE (10 MHz resolution = 0.1 µs precision)
 *    - Hardware timestamp capture on every GPIO edge
 *    - Provides precise time measurements (t_start, t_last)
 *    - ISR overhead: ~10-20 CPU cycles per pulse (just updates timestamp)
 * 
 * 2. PCNT (Pulse Counter)
 *    - Hardware pulse counting (zero CPU overhead during counting!)
 *    - Counts pulses in background until target reached (e.g., 100 pulses)
 *    - Triggers callback only when watchpoint (N pulses) is reached
 * 
 * 3. GPTIMER (1 MHz = 1 µs resolution)
 *    - Timeout watchdog (200ms periodic check)
 *    - Ensures speed updates even at very low speeds or when stopped
 * 
 * DUAL SENSOR SUPPORT:
 * ====================
 * - Sensor 1: GPIO 18, MCPWM Group 0, dedicated PCNT unit
 * - Sensor 2: GPIO 19, MCPWM Group 1, dedicated PCNT unit
 * - Each sensor independently identified by pcnt_unit handle
 * - Shared GPTimer checks both sensors every 200ms
 * 
 * TWO MEASUREMENT MODES:
 * ======================
 * 
 * MODE 1: Normal/High Speed (PCNT callback triggers first)
 * --------------------------------------------------------
 * - Waits for exactly N pulses (e.g., 100)
 * - Measures time between first and Nth pulse
 * - Result: "100 pulses in 47.3 ms" → very precise
 * - Example: At 1000 RPM, 100 pulses takes ~50ms
 * 
 * MODE 2: Slow/Stopped (Timeout triggers first at 200ms)
 * -------------------------------------------------------
 * - If N pulses not reached in 200ms, timeout fires
 * - Uses whatever pulses were counted (e.g., 8 pulses)
 * - Result: "8 pulses in 200 ms" → less precise but still valid
 * - Prevents infinite waiting at low speeds or when stopped
 * - Example: At 10 RPM, might only get 5 pulses in 200ms
 * 
 * WHY TIMEOUT IS NEEDED:
 * ======================
 * Without timeout:
 * - At low speed (10 RPM): waiting for 100 pulses = several seconds delay
 * - Motor stopped: waiting forever (deadlock)
 * 
 * With timeout:
 * - Always get speed update within 200ms maximum
 * - Low speed: use fewer pulses (less precision but acceptable)
 * - Stopped: detect zero speed quickly
 * 
 * RESOLUTION EXPLAINED:
 * =====================
 * - MCPWM 10 MHz: For PRECISE timestamp measurement (not related to timeout)
 *   → 1 tick = 0.1 microseconds = 100 nanosecond precision
 *   → Example: measure 47,385 ticks = 4.7385 milliseconds (very precise!)
 * 
 * - GPTimer 1 MHz: For timeout counter only (unrelated to measurement precision)
 *   → 1 tick = 1 microsecond
 *   → 200,000 ticks = 200 milliseconds timeout period
 *   → Easy math: alarm_count directly equals microseconds
 * 
 * ISR EXECUTION FLOW:
 * ===================
 * Every pulse on GPIO triggers MCPWM callback:
 *   1. Updates s_t_last (lightweight, ~10 CPU cycles)
 *   2. FIRST pulse only: starts PCNT counting + saves t_start
 *   3. PCNT counts in hardware (zero CPU)
 *   4. When PCNT reaches N: triggers pcnt_reach_cb → publishes result
 *   OR
 *   5. If 200ms timeout: uses partial count → publishes result
 * 
 ******************************************************************************/

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "driver/mcpwm_cap.h"
#include "driver/pulse_cnt.h"
#include "driver/gptimer.h"

static const char *TAG = "speed_sensor";

/* ==========================
 * User configuration
 * ========================== */
#define UPDATE_TIMEOUT_US     200000   // 200 ms max update interval (fallback for low speed)
#define CAPTURE_RES_HZ        10000000  // 10 MHz capture timer resolution => 0.1 µs precision for timestamps

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

/* ==========================
 * Shared resources
 * ========================== */
static gptimer_handle_t s_timeout_timer = NULL;

/* ==========================
 * Sensor instances
 * ========================== */
static speed_sensor_t s_sensor1 = {
    .cap_timer = NULL,
    .cap_chan = NULL,
    .pcnt_unit = NULL,
    .pcnt_chan = NULL,
    .gpio_num = 18,
    .mcpwm_group_id = 0,
    .target_periods = 100,
    .armed = false,
    .running = false,
    .t_start = 0,
    .t_last = 0,
    .new_result = false,
    .used_periods = 0,
    .dt_ticks = 0,
    .mux = portMUX_INITIALIZER_UNLOCKED
};

static speed_sensor_t s_sensor2 = {
    .cap_timer = NULL,
    .cap_chan = NULL,
    .pcnt_unit = NULL,
    .pcnt_chan = NULL,
    .gpio_num = 19,
    .mcpwm_group_id = 1,  // Use different MCPWM group for sensor 2
    .target_periods = 100,
    .armed = false,
    .running = false,
    .t_start = 0,
    .t_last = 0,
    .new_result = false,
    .used_periods = 0,
    .dt_ticks = 0,
    .mux = portMUX_INITIALIZER_UNLOCKED
};

/* ==========================
 * Helper: wrap-safe tick difference
 * ========================== */
static inline uint32_t tick_diff_u32(uint32_t now, uint32_t then) {
    return (uint32_t)(now - then); // unsigned wrap-safe
}

/* ==========================
 * MCPWM Capture callback - Called on EVERY pulse!
 * ==========================
 * EXECUTION: Every rising edge on GPIO triggers this ISR
 * 
 * WHAT HAPPENS ON EACH PULSE:
 * ----------------------------
 * 1. Updates s_t_last (always, very fast ~10 CPU cycles)
 *    - Keeps latest edge timestamp for end-of-measurement
 *    - Used by both PCNT callback and timeout callback
 * 
 * 2. FIRST PULSE ONLY (when armed && !running):
 *    - Captures precise start time (t_start)
 *    - Resets PCNT counter to 0
 *    - Starts PCNT hardware counting
 *    - Sets running=true to prevent re-entry
 * 
 * 3. SUBSEQUENT PULSES:
 *    - Only updates t_last (minimal overhead)
 *    - PCNT counts in hardware (no ISR overhead!)
 * 
 * EFFICIENCY:
 * -----------
 * - First pulse:  ~100-200 CPU cycles (setup PCNT)
 * - Other pulses: ~10-20 CPU cycles (just timestamp update)
 * - Counting: 0 CPU cycles (hardware PCNT does it!)
 * 
 * This design supports high-frequency signals (10+ kHz) with minimal CPU load.
 * ========================== */
static bool IRAM_ATTR on_capture_cb(mcpwm_cap_channel_handle_t chan,
                                   const mcpwm_capture_event_data_t *edata,
                                   void *user_data)
{
    (void)chan;
    speed_sensor_t *sensor = (speed_sensor_t *)user_data;  // Identifies which sensor (1 or 2)

    uint32_t t = edata->cap_value;

    // Always keep "latest edge timestamp" for timeout/end timestamping
    sensor->t_last = t;

    // Arm/start logic: start window exactly on a real edge
    if (sensor->armed && !sensor->running) {
        sensor->t_start = t;

        // Reset & start PCNT counting from this edge onwards
        pcnt_unit_clear_count(sensor->pcnt_unit);
        pcnt_unit_start(sensor->pcnt_unit);

        sensor->running = true;
        sensor->armed = false;
    }

    return false; // no yield
}

/* ==========================
 * PCNT watchpoint callback: MEASUREMENT MODE 1 (Normal/High Speed)
 * ==========================
 * TRIGGER: When PCNT hardware counter reaches exactly N pulses (e.g., 100)
 * 
 * EXECUTION SCENARIO:
 * -------------------
 * Example at 1000 RPM (fast enough):
 *   - Started counting at t_start
 *   - PCNT counts 1, 2, 3, ... 99, 100 (in hardware, no CPU)
 *   - Reaches 100 → THIS CALLBACK FIRES
 *   - Total time: ~50 milliseconds (well before 200ms timeout)
 * 
 * WHAT IT DOES:
 * -------------
 * 1. Reads N (number of pulses that triggered this = watchpoint value)
 * 2. Uses t_last (timestamp of Nth pulse) as end time
 * 3. Calculates dt = t_end - t_start (time for N pulses)
 * 4. Publishes result: "N pulses in dt ticks"
 * 5. Stops PCNT, re-arms for next measurement cycle
 * 
 * PRECISION:
 * ----------
 * - Full N pulses used (e.g., 100) → maximum precision
 * - Time measured with 0.1 µs resolution (10 MHz MCPWM)
 * - Example: 100 pulses in 47,385 ticks = 4.7385 ms → very precise RPM
 * 
 * SENSOR IDENTIFICATION:
 * ----------------------
 * user_data pointer identifies which sensor triggered (sensor1 or sensor2)
 * Each sensor has its own pcnt_unit, so they operate independently
 * ========================== */
static bool IRAM_ATTR on_pcnt_reach_cb(pcnt_unit_handle_t unit,
                                      const pcnt_watch_event_data_t *edata,
                                      void *user_data)
{
    (void)unit;
    speed_sensor_t *sensor = (speed_sensor_t *)user_data;

    // edata->watch_point_value is the count at trigger (target)
    uint32_t used = (uint32_t)edata->watch_point_value;

    // Use the most recent capture timestamp as t_end (hardware-latched on edge)
    uint32_t t_end = sensor->t_last;
    uint32_t dt = tick_diff_u32(t_end, sensor->t_start);

    // Publish result atomically (coarse-grain critical section)
    portENTER_CRITICAL_ISR(&sensor->mux);
    sensor->used_periods = used;
    sensor->dt_ticks = dt;
    sensor->new_result = true;
    portEXIT_CRITICAL_ISR(&sensor->mux);

    // Stop counting until next arm
    pcnt_unit_stop(sensor->pcnt_unit);
    sensor->running = false;

    // Re-arm for next window immediately (next capture edge will restart)
    sensor->armed = true;

    return false;
}

/* ==========================
 * GPTimer timeout callback: MEASUREMENT MODE 2 (Slow/Stopped Fallback)
 * ==========================
 * TRIGGER: Every 200ms (periodic alarm), checks BOTH sensors
 * 
 * PURPOSE: Prevent waiting forever for N pulses at low speeds or when stopped
 * 
 * EXECUTION SCENARIO:
 * -------------------
 * Example at 10 RPM (too slow):
 *   - Started counting at t_start
 *   - Waiting for 100 pulses...
 *   - 200ms timeout fires → only 8 pulses counted so far
 *   - PCNT callback hasn't fired yet (needs 100)
 *   - THIS CALLBACK FIRES INSTEAD
 *   - Publishes: "8 pulses in 200 ms" → still valid, just less precise
 * 
 * WHAT IT DOES (for each sensor):
 * --------------------------------
 * 1. Check if sensor is running (actively measuring)
 *    - If not running: skip (still waiting for first edge)
 * 2. Read current PCNT count (partial count, e.g., 8 pulses)
 * 3. If count > 0: use whatever pulses we have
 *    - Calculate dt = t_last - t_start
 *    - Publish result: "count pulses in dt ticks"
 *    - Stop PCNT, re-arm for next cycle
 * 4. If count = 0: keep waiting (motor might be stopped)
 * 
 * WHY THIS IS NEEDED:
 * -------------------
 * Without timeout:
 *   - At 10 RPM: need to wait 10+ seconds for 100 pulses
 *   - Motor stopped: wait forever (deadlock!)
 * 
 * With timeout:
 *   - Always get speed update within 200ms
 *   - Low speed: acceptable precision with fewer pulses
 *   - Stopped: quickly detect zero speed
 * 
 * TIMER RESOLUTION:
 * -----------------
 * GPTimer configured at 1 MHz (1 µs ticks):
 *   - 200,000 ticks = 200 milliseconds
 *   - Easy conversion: alarm_count = microseconds
 *   - Unrelated to MCPWM measurement precision (that's 10 MHz)
 * ========================== */
static bool IRAM_ATTR on_timeout_cb(gptimer_handle_t timer,
                                   const gptimer_alarm_event_data_t *edata,
                                   void *user_data)
{
    (void)timer; (void)edata; (void)user_data;

    // Check both sensors
    speed_sensor_t *sensors[] = {&s_sensor1, &s_sensor2};
    
    for (int i = 0; i < 2; i++) {
        speed_sensor_t *sensor = sensors[i];
        
        // If not running, nothing to do (still waiting for first edge)
        if (!sensor->running) {
            continue;
        }

        // Read current count (how many periods observed so far)
        int count = 0;
        pcnt_unit_get_count(sensor->pcnt_unit, &count);

        // If we have at least 1 period, publish a result; else keep waiting
        if (count > 0) {
            uint32_t used = (uint32_t)count;
            uint32_t t_end = sensor->t_last;
            uint32_t dt = tick_diff_u32(t_end, sensor->t_start);

            portENTER_CRITICAL_ISR(&sensor->mux);
            sensor->used_periods = used;
            sensor->dt_ticks = dt;
            sensor->new_result = true;
            portEXIT_CRITICAL_ISR(&sensor->mux);

            pcnt_unit_stop(sensor->pcnt_unit);
            sensor->running = false;

            // Re-arm
            sensor->armed = true;
        }
    }

    return false;
}

/* ==========================
 * Private helper: Initialize one sensor
 * ========================== */
static esp_err_t sensor_init_internal(speed_sensor_t *sensor, uint32_t initial_periods)
{
    if (sensor == NULL) return ESP_ERR_INVALID_ARG;

    sensor->target_periods = initial_periods;

    /* ---- MCPWM Capture timer ---- */
    {
        mcpwm_capture_timer_config_t tcfg = {
            .group_id = sensor->mcpwm_group_id,
            .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
            .resolution_hz = CAPTURE_RES_HZ,
        };
        ESP_RETURN_ON_ERROR(mcpwm_new_capture_timer(&tcfg, &sensor->cap_timer), TAG, "new cap timer");

        mcpwm_capture_channel_config_t ccfg = {};
        ccfg.gpio_num = sensor->gpio_num;
        ccfg.prescale = 1;
        ccfg.flags.pos_edge = 1;
        ccfg.flags.neg_edge = 0;
        ccfg.flags.pull_up = 1;
        
        ESP_RETURN_ON_ERROR(mcpwm_new_capture_channel(sensor->cap_timer, &ccfg, &sensor->cap_chan), TAG, "new cap chan");

        mcpwm_capture_event_callbacks_t cbs = {
            .on_cap = on_capture_cb,
        };
        ESP_RETURN_ON_ERROR(mcpwm_capture_channel_register_event_callbacks(sensor->cap_chan, &cbs, sensor), TAG, "cap cbs");

        ESP_RETURN_ON_ERROR(mcpwm_capture_channel_enable(sensor->cap_chan), TAG, "cap chan en");
        ESP_RETURN_ON_ERROR(mcpwm_capture_timer_enable(sensor->cap_timer), TAG, "cap timer en");
        ESP_RETURN_ON_ERROR(mcpwm_capture_timer_start(sensor->cap_timer), TAG, "cap timer start");
    }

    /* ---- PCNT ---- */
    {
        pcnt_unit_config_t ucfg = {
            .low_limit = 0,
            .high_limit = (int)initial_periods,
        };
        ESP_RETURN_ON_ERROR(pcnt_new_unit(&ucfg, &sensor->pcnt_unit), TAG, "new pcnt unit");

        pcnt_chan_config_t chcfg = {
            .edge_gpio_num = sensor->gpio_num,
            .level_gpio_num = -1,
        };
        ESP_RETURN_ON_ERROR(pcnt_new_channel(sensor->pcnt_unit, &chcfg, &sensor->pcnt_chan), TAG, "new pcnt chan");

        // Count rising edges only
        ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(sensor->pcnt_chan,
                                                        PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                        PCNT_CHANNEL_EDGE_ACTION_HOLD), TAG, "edge action");
        ESP_RETURN_ON_ERROR(pcnt_channel_set_level_action(sensor->pcnt_chan,
                                                         PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                         PCNT_CHANNEL_LEVEL_ACTION_KEEP), TAG, "level action");

        // Watchpoint exactly at N
        ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(sensor->pcnt_unit, (int)initial_periods), TAG, "add watchpoint");

        pcnt_event_callbacks_t pcbs = {
            .on_reach = on_pcnt_reach_cb,
        };
        ESP_RETURN_ON_ERROR(pcnt_unit_register_event_callbacks(sensor->pcnt_unit, &pcbs, sensor), TAG, "pcnt cbs");

        ESP_RETURN_ON_ERROR(pcnt_unit_enable(sensor->pcnt_unit), TAG, "pcnt enable");
        pcnt_unit_stop(sensor->pcnt_unit); // start only after first edge
    }

    // Start in "armed" state (wait for first edge to define t_start precisely)
    sensor->armed = true;
    sensor->running = false;

    ESP_LOGI(TAG, "Sensor GPIO%d initialized. Target periods=%u, MCPWM group=%d", 
             sensor->gpio_num, initial_periods, sensor->mcpwm_group_id);
    return ESP_OK;
}

/* ==========================
 * Public API
 * ========================== */

/**
 * Dynamically adjust target pulse count for a sensor
 * 
 * USE CASES:
 * ----------
 * 1. Speed-adaptive measurement:
 *    - High speed: use fewer pulses (50) for faster updates
 *    - Low speed: use more pulses (200) for better precision
 * 
 * 2. Testing/calibration without recompiling
 * 
 * USAGE EXAMPLE:
 * --------------
 * // Adjust sensor 1 to count 200 pulses instead of 100
 * speed_sensor_set_target_pulses(speed_sensor_get_sensor1(), 200);
 * 
 * NOTE: This is a PUBLIC API function designed to be called from your
 * application code, not internally by this module.
 */
esp_err_t speed_sensor_set_target_pulses(speed_sensor_t *sensor, uint32_t periods)
{
    if (sensor == NULL) return ESP_ERR_INVALID_ARG;
    if (periods == 0) return ESP_ERR_INVALID_ARG;
    if (sensor->pcnt_unit == NULL) return ESP_ERR_INVALID_STATE;

    sensor->target_periods = periods;

    // Update watchpoint
    ESP_RETURN_ON_ERROR(pcnt_unit_remove_watch_point(sensor->pcnt_unit, (int)periods), TAG, "rm wp");
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(sensor->pcnt_unit, (int)periods), TAG, "add wp");

    return ESP_OK;
}

esp_err_t speed_sensor1_init(uint32_t initial_periods, int gpio_num)
{
    if (gpio_num >= 0) {
        s_sensor1.gpio_num = gpio_num;
    }
    
    esp_err_t ret = sensor_init_internal(&s_sensor1, initial_periods);
    if (ret != ESP_OK) return ret;

    // Create shared timeout timer on first init
    if (s_timeout_timer == NULL) {
        gptimer_config_t gcfg = {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,      // Use default APB clock (~80 MHz)
            .direction = GPTIMER_COUNT_UP,           // Count upwards: 0→1→2→...→alarm
            .resolution_hz = 1000000,                // 1 MHz = 1 tick per microsecond
                                                     // Math: 80 MHz / 80 = 1 MHz
                                                     // Makes alarm_count = microseconds (easy!)
        };
        ESP_RETURN_ON_ERROR(gptimer_new_timer(&gcfg, &s_timeout_timer), TAG, "new gptimer");

        gptimer_event_callbacks_t tcbs = {
            .on_alarm = on_timeout_cb,
        };
        ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(s_timeout_timer, &tcbs, NULL), TAG, "timer cbs");

        gptimer_alarm_config_t acfg = {};
        acfg.alarm_count = UPDATE_TIMEOUT_US;
        acfg.reload_count = 0;
        acfg.flags.auto_reload_on_alarm = true;
        
        ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(s_timeout_timer, &acfg), TAG, "set alarm");
        ESP_RETURN_ON_ERROR(gptimer_enable(s_timeout_timer), TAG, "timer enable");
        ESP_RETURN_ON_ERROR(gptimer_start(s_timeout_timer), TAG, "timer start");
    }

    return ESP_OK;
}

esp_err_t speed_sensor2_init(uint32_t initial_periods, int gpio_num)
{
    if (gpio_num >= 0) {
        s_sensor2.gpio_num = gpio_num;
    }
    
    esp_err_t ret = sensor_init_internal(&s_sensor2, initial_periods);
    if (ret != ESP_OK) return ret;

    // Create shared timeout timer if not already created
    if (s_timeout_timer == NULL) {
        gptimer_config_t gcfg = {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,
            .direction = GPTIMER_COUNT_UP,
            .resolution_hz = 1000000,
        };
        ESP_RETURN_ON_ERROR(gptimer_new_timer(&gcfg, &s_timeout_timer), TAG, "new gptimer");

        gptimer_event_callbacks_t tcbs = {
            .on_alarm = on_timeout_cb,
        };
        ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(s_timeout_timer, &tcbs, NULL), TAG, "timer cbs");

        gptimer_alarm_config_t acfg = {};
        acfg.alarm_count = UPDATE_TIMEOUT_US;
        acfg.reload_count = 0;
        acfg.flags.auto_reload_on_alarm = true;
        
        ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(s_timeout_timer, &acfg), TAG, "set alarm");
        ESP_RETURN_ON_ERROR(gptimer_enable(s_timeout_timer), TAG, "timer enable");
        ESP_RETURN_ON_ERROR(gptimer_start(s_timeout_timer), TAG, "timer start");
    }

    return ESP_OK;
}

/**
 * Compute RPM using last published result.
 * pulses_per_rev: how many periods correspond to one revolution
 */
float speed_sensor_get_rpm(speed_sensor_t *sensor, uint32_t pulses_per_rev)
{
    if (sensor == NULL) return 0.0f;
    
    static float last_rpm_s1 = 0.0f;
    static float last_rpm_s2 = 0.0f;
    float *last_rpm = (sensor == &s_sensor1) ? &last_rpm_s1 : &last_rpm_s2;

    bool has_new = false;
    uint32_t used = 0;
    uint32_t dt = 0;

    portENTER_CRITICAL(&sensor->mux);
    has_new = sensor->new_result;
    if (has_new) {
        used = sensor->used_periods;
        dt = sensor->dt_ticks;
        sensor->new_result = false;
    }
    portEXIT_CRITICAL(&sensor->mux);

    if (has_new && dt > 0 && pulses_per_rev > 0) {
        // frequency (Hz) = used_periods / (dt_ticks / CAPTURE_RES_HZ)
        // rpm = Hz * 60 / pulses_per_rev
        float seconds = (float)dt / (float)CAPTURE_RES_HZ;
        float hz = (float)used / seconds;
        *last_rpm = (hz * 60.0f) / (float)pulses_per_rev;
    }

    return *last_rpm;
}

// Convenience functions to get sensors
speed_sensor_t* speed_sensor_get_sensor1(void) {
    return &s_sensor1;
}

speed_sensor_t* speed_sensor_get_sensor2(void) {
    return &s_sensor2;
}