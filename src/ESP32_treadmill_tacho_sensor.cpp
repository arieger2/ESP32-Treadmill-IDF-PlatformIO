/******************************************************************************
 * DUAL SPEED SENSOR - High-Precision Tachometer with Hardware Counting
 * 
 * ARCHITECTURE OVERVIEW:
 * ======================
 * Uses two ESP32 hardware peripherals working together:
 * 
 * 1. PCNT (Pulse Counter)
 *    - Hardware pulse counting (zero CPU overhead during counting!)
 *    - Counts pulses in background until target reached (e.g., 100 pulses)
 *    - Triggers callback only when watchpoint (N pulses) is reached
 *    - ISR-safe operations for count reading and watchpoint management
 * 
 * 2. GPTIMER (1 MHz = 1 µs resolution)
 *    - Free-running timestamp timer
 *    - Provides monotonic time reference for calculating elapsed time
 *    - Used in PCNT ISR to measure time between measurements
 * 
 * DUAL SENSOR SUPPORT:
 * ====================
 * - Sensor 1: GPIO 18, dedicated PCNT unit (Band sensor)
 * - Sensor 2: GPIO 19, dedicated PCNT unit (Motor sensor)
 * - Each sensor independently identified by pcnt_unit handle
 * - Shared GPTimer provides timestamp reference
 * 
 * MEASUREMENT OPERATION:
 * ======================
 * - PCNT counts pulses in hardware (no CPU overhead)
 * - Watchpoint set at target pulse count (configured via target_periods)
 * - When watchpoint reached: ISR fires, calculates time delta, publishes result
 * - Result: "N pulses in X microseconds" → converted to RPM
 * - Example: At 1000 RPM with 100 pulse target, callback fires every ~50ms
 * 
 * TIMER RESOLUTION:
 * ==================
 * - GPTimer 1 MHz: For timestamp measurement
 *   → 1 tick = 1 microsecond
 *   → Provides microsecond precision for time deltas
 *   → Free-running counter ensures monotonic timestamps
 * 
 * ISR EXECUTION FLOW:
 * ===================
 * GPIO pulses increment PCNT hardware counter:
 *   1. PCNT counts in hardware (zero CPU usage)
 *   2. When count reaches watchpoint N: triggers on_pcnt_reach_cb ISR
 *   3. ISR reads timestamp, calculates time delta since last measurement
 *   4. Publishes result (pulse count + time elapsed)
 *   5. Clears counter and starts next measurement cycle
 * 
 ******************************************************************************/

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

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

/* ==========================
 * Shared resources
 * ========================== */
gptimer_handle_t s_timestamp_timer = NULL;
portMUX_TYPE s_sensor_spinlock = portMUX_INITIALIZER_UNLOCKED;
TimerHandle_t s_zero_speed_timer = NULL;

// Zero-speed detection timeout (1 second)
#define ZERO_SPEED_TIMEOUT_MS 1000

/* ==========================
 * Sensor instances
 * ========================== */
speed_sensor_t s_sensor1 = {
    .pcnt_unit = NULL,
    .pcnt_chan = NULL,
    .sensor_type = SENSOR_TYPE_BAND,
    .gpio_num = -1,
    .target_periods = 100,
    .t_last = 0,
    .used_periods = 0,
    .period_us = 0,
};

speed_sensor_t s_sensor2 = {
    .pcnt_unit = NULL,
    .pcnt_chan = NULL,
    .sensor_type = SENSOR_TYPE_MOTOR,
    .gpio_num = -1,
    .target_periods = 100,
    .t_last = 0,
    .used_periods = 0,
    .period_us = 0,
};



/* ==========================
 * FreeRTOS Timer Callback - Zero Speed Detection
 * ==========================
 * TRIGGER: Every 1 second (periodic)
 * 
 * PURPOSE:
 * --------
 * Detect when belt has stopped moving (no pulses received)
 * PCNT cannot detect absence of pulses - needs external watchdog
 * 
 * OPERATION:
 * ----------
 * 1. Checks time since last PCNT measurement for each sensor
 * 2. If > 1 second elapsed without new pulses → publish zero speed
 * 3. Clears period_us to signal "no valid data"
 * 
 * RUNS IN: FreeRTOS timer daemon task (NOT ISR)
 * CPU OVERHEAD: ~50-100 cycles every 1 second
 * ========================== */
void zero_speed_check_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    bool sensor1Data = true;
    bool sensor2Data = true;
    uint64_t now;
    gptimer_get_raw_count(s_timestamp_timer, &now);
    
    // Check sensor 1 (band sensor)
    portENTER_CRITICAL(&s_sensor_spinlock);
    uint64_t time_since_last_1 = now - s_sensor1.t_last;
    if (time_since_last_1 > (ZERO_SPEED_TIMEOUT_MS * 1000)) {
        // No pulses for >1 second → belt stopped
        s_sensor1.period_us = 0;  // Signal zero speed (dt = 0 triggers zero_pending)
        s_sensor1.used_periods = 1;  // Must be non-zero to trigger read in get_rpm_and_delta
        sensor1Data = false;
    }
    portEXIT_CRITICAL(&s_sensor_spinlock);
    
    // Check sensor 2 (motor sensor)
    portENTER_CRITICAL(&s_sensor_spinlock);
    uint64_t time_since_last_2 = now - s_sensor2.t_last;
    if (time_since_last_2 > (ZERO_SPEED_TIMEOUT_MS * 1000)) {
        // No pulses for >1 second → motor stopped
        s_sensor2.period_us = 0;  // Signal zero speed (dt = 0 triggers zero_pending)
        s_sensor2.used_periods = 1;  // Must be non-zero to trigger read in get_rpm_and_delta
        sensor2Data = false;
    }
    portEXIT_CRITICAL(&s_sensor_spinlock);
    
    if (sensor2Data == false && sensor1Data == false) {
        metrics.rpm = 0.0f;
        metrics.motorRPM = 0.0f;
        metrics.mps = 0.0f;
        metrics.isRunning = false;
        ESP_LOGD(TAG, "Zero-speed detected: S1 data=%d, S2 data=%d", sensor1Data, sensor2Data);
    } else {
        metrics.isRunning = true;
    }
}

/* ==========================
 * PCNT watchpoint callback
 * ==========================
 * TRIGGER: When PCNT hardware counter reaches exactly N pulses (watchpoint)
 * 
 * EXECUTION SCENARIO:
 * -------------------
 * Example at 1000 RPM with 100 pulse target:
 *   - PCNT counts 1, 2, 3, ... 99, 100 (in hardware, no CPU)
 *   - Reaches 100 → THIS CALLBACK FIRES
 *   - Total time: ~50 milliseconds between measurements
 * 
 * WHAT IT DOES:
 * -------------
 * 1. Reads watchpoint value (number of pulses that triggered callback)
 * 2. Gets current timestamp from free-running GPTimer
 * 3. Calculates time delta since last measurement (now - t_last)
 * 4. Publishes result: pulse count + time elapsed
 * 5. Updates t_last to current timestamp for next measurement
 * 6. Clears PCNT counter to restart counting
 * 
 * PRECISION:
 * ----------
 * - Pulse count: Exact (hardware counted)
 * - Time measured with 1 µs resolution (GPTimer at 1 MHz)
 * - Example: 100 pulses in 50,234 µs = 50.234 ms → precise RPM calculation
 * 
 * SANITY CHECK:
 * -------------
 * - Rejects measurements > 1 second (invalid/stale data)
 * - Prevents corrupted readings from glitches or resets
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

    // Use the dedicated timestamp timer so values stay monotonic
    uint64_t now;
    gptimer_get_raw_count(s_timestamp_timer, &now);   // IRAM-safe
    portENTER_CRITICAL_ISR(&s_sensor_spinlock);
    uint64_t previous = sensor->t_last;
    uint64_t diff = now - previous;
    if (diff > CAPTURE_RES_HZ) { // 1 second sanity check
        sensor->t_last = now;
        portEXIT_CRITICAL_ISR(&s_sensor_spinlock);
        pcnt_unit_clear_count(sensor->pcnt_unit);
        return false;
    }

    sensor->used_periods = used;
    sensor->period_us = diff;
    sensor->t_last = now;
    portEXIT_CRITICAL_ISR(&s_sensor_spinlock);

    // Stop counting, ready for next measurement cycle
    pcnt_unit_clear_count(sensor->pcnt_unit);
    
    // Metrics will be updated in loop() every 200ms (not in ISR!)

    return false;
}

