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

#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_sensor.h"

static const char *TAG = "speed_sensor";

// External metrics reference
extern TreadmillMetrics metrics;

/* ==========================
 * User configuration
 * ========================== */
#define UPDATE_TIMEOUT_US     200000   // 200 ms max update interval (fallback for low speed)
#define CAPTURE_RES_HZ        10000000  // 10 MHz capture timer resolution => 0.1 µs precision for timestamps

/* ==========================
 * Shared resources
 * ========================== */
gptimer_handle_t s_timeout_timer = NULL;

/* ==========================
 * Sensor instances
 * ========================== */
speed_sensor_t s_sensor1 = {
    .cap_timer = NULL,
    .cap_chan = NULL,
    .pcnt_unit = NULL,
    .pcnt_chan = NULL,
    .sensor_type = SENSOR_TYPE_BAND,
    .gpio_num = -1,
    .mcpwm_group_id = 0,
    .target_periods = 100,
    .running = false,
    .t_start = 0,
    .t_last = 0,
    .used_periods = 0,
    .dt_ticks = 0,
    .mux = portMUX_INITIALIZER_UNLOCKED
};

speed_sensor_t s_sensor2 = {
    .cap_timer = NULL,
    .cap_chan = NULL,
    .pcnt_unit = NULL,
    .pcnt_chan = NULL,
    .sensor_type = SENSOR_TYPE_MOTOR,
    .gpio_num = -1,
    .mcpwm_group_id = 1,  // Use different MCPWM group for sensor 2
    .target_periods = 100,
    .running = false,
    .t_start = 0,
    .t_last = 0,
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
 * 2. FIRST PULSE ONLY (when  !running):
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
bool IRAM_ATTR on_capture_cb(mcpwm_cap_channel_handle_t chan,
                                   const mcpwm_capture_event_data_t *edata,
                                   void *user_data)
{
    (void)chan;
    speed_sensor_t *sensor = (speed_sensor_t *)user_data;  // Identifies which sensor (1 or 2)

    uint32_t t = edata->cap_value;

    // Always keep "latest edge timestamp" for timeout/end timestamping
    sensor->t_last = t;

    // Start logic: start measurement window on first edge when ready
    if (!sensor->running) {
        sensor->t_start = t;

        // Reset & start PCNT counting from this edge onwards
        pcnt_unit_clear_count(sensor->pcnt_unit);
        pcnt_unit_start(sensor->pcnt_unit);

        sensor->running = true;
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
bool IRAM_ATTR on_pcnt_reach_cb(pcnt_unit_handle_t unit,
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
    sensor->used_periods += used;
    sensor->dt_ticks += dt;
    portEXIT_CRITICAL_ISR(&sensor->mux);

    // Stop counting, ready for next measurement cycle
    pcnt_unit_stop(sensor->pcnt_unit);
    sensor->running = false;

    // Metrics will be updated in loop() every 200ms (not in ISR!)

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
bool IRAM_ATTR on_timeout_cb(gptimer_handle_t timer,
                                   const gptimer_alarm_event_data_t *edata,
                                   void *user_data)
{
    (void)timer; (void)edata; (void)user_data;

    // TEST MODE: Use simulated data instead of real sensors
    extern volatile bool testdata;
    if (testdata) {
        extern void updateTestMetrics();
        updateTestMetrics();
        return false;
    }

    // NORMAL MODE: Check both real sensors
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
            sensor->used_periods += used;
            sensor->dt_ticks += dt;
            portEXIT_CRITICAL_ISR(&sensor->mux);

            pcnt_unit_stop(sensor->pcnt_unit);
            sensor->running = false;
            
            // Metrics will be updated in loop() every 200ms (not in ISR!)
        }
    }

    return false;
}

