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
 * User configuration Alex
 * ========================== */
#define SPEED_GPIO            18
#define UPDATE_TIMEOUT_US     200000   // 200 ms max update interval
#define CAPTURE_RES_HZ        10000000  // 10 MHz capture timer resolution => 0.1 us ticks

/* If your hall signal is clean, keep filter small or off.
 * This filter rejects pulses shorter than width_ticks in capture timer ticks.
 * At 10 MHz: 50 ticks = 5 us
 */
#define CAP_GPIO_FILTER_TICKS 50

/* ==========================
 * Handles / state
 * ========================== */
static mcpwm_cap_timer_handle_t   s_cap_timer = NULL;
static mcpwm_cap_channel_handle_t s_cap_chan  = NULL;

static pcnt_unit_handle_t         s_pcnt_unit = NULL;
static pcnt_channel_handle_t      s_pcnt_chan = NULL;

static gptimer_handle_t           s_timeout_timer = NULL;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Measurement control */
static volatile bool     s_armed = false;     // waiting for first edge to start measurement window
static volatile bool     s_running = false;   // measurement active (pcnt counting)
static volatile uint32_t s_target_periods = 100; // desired number of periods (N)

/* Captured timestamps (hardware latched) */
static volatile uint32_t s_t_start = 0;   // capture tick at first edge (window start)
static volatile uint32_t s_t_last  = 0;   // last capture tick seen (always updated on cap event)

/* Published result (snapshot) */
static volatile bool     s_new_result = false;
static volatile uint32_t s_used_periods = 0; // how many periods used for last result
static volatile uint32_t s_dt_ticks = 0;     // delta time in capture ticks for last result

/* ==========================
 * Helper: wrap-safe tick difference
 * ========================== */
static inline uint32_t tick_diff_u32(uint32_t now, uint32_t then) {
    return (uint32_t)(now - then); // unsigned wrap-safe
}

/* ==========================
 * MCPWM Capture callback
 * ========================== */
static bool IRAM_ATTR on_capture_cb(mcpwm_cap_channel_handle_t chan,
                                   const mcpwm_capture_event_data_t *edata,
                                   void *user_data)
{
    (void)chan; (void)user_data;

    uint32_t t = edata->cap_value;

    // Always keep "latest edge timestamp" for timeout/end timestamping
    s_t_last = t;

    // Arm/start logic: start window exactly on a real edge
    if (s_armed && !s_running) {
        s_t_start = t;

        // Reset & start PCNT counting from this edge onwards
        // NOTE: These driver calls are not guaranteed IRAM-safe in all IDF versions.
        // If you need strict IRAM-only ISR, move this logic into a high-priority task:
        // set a flag here and do pcnt_clear+start in task context.
        pcnt_unit_clear_count(s_pcnt_unit);
        pcnt_unit_start(s_pcnt_unit);

        s_running = true;
        s_armed = false;
    }

    return false; // no yield
}

/* ==========================
 * PCNT watchpoint callback: reached N edges (periods)
 * ========================== */
static bool IRAM_ATTR on_pcnt_reach_cb(pcnt_unit_handle_t unit,
                                      const pcnt_watch_event_data_t *edata,
                                      void *user_data)
{
    (void)unit; (void)user_data;

    // edata->watch_point_value is the count at trigger (target)
    uint32_t used = (uint32_t)edata->watch_point_value;

    // Use the most recent capture timestamp as t_end (hardware-latched on edge)
    uint32_t t_end = s_t_last;
    uint32_t dt = tick_diff_u32(t_end, s_t_start);

    // Publish result atomically (coarse-grain critical section)
    portENTER_CRITICAL_ISR(&s_mux);
    s_used_periods = used;
    s_dt_ticks = dt;
    s_new_result = true;
    portEXIT_CRITICAL_ISR(&s_mux);

    // Stop counting until next arm
    // Same caveat re: IRAM-safety as above; can be deferred to task if needed.
    pcnt_unit_stop(s_pcnt_unit);
    s_running = false;

    // Re-arm for next window immediately (next capture edge will restart)
    s_armed = true;

    return false;
}

/* ==========================
 * GPTimer timeout callback: max 200ms without N reached
 * ========================== */
static bool IRAM_ATTR on_timeout_cb(gptimer_handle_t timer,
                                   const gptimer_alarm_event_data_t *edata,
                                   void *user_data)
{
    (void)timer; (void)edata; (void)user_data;

    // If not running, nothing to do (still waiting for first edge)
    if (!s_running) {
        return false;
    }

    // Read current count (how many periods observed so far)
    int count = 0;
    // Caveat: may not be IRAM safe; if concerned, set a flag and read in task.
    pcnt_unit_get_count(s_pcnt_unit, &count);

    // If we have at least 1 period, publish a result; else keep waiting
    if (count > 0) {
        uint32_t used = (uint32_t)count;
        uint32_t t_end = s_t_last;
        uint32_t dt = tick_diff_u32(t_end, s_t_start);

        portENTER_CRITICAL_ISR(&s_mux);
        s_used_periods = used;
        s_dt_ticks = dt;
        s_new_result = true;
        portEXIT_CRITICAL_ISR(&s_mux);

        pcnt_unit_stop(s_pcnt_unit);
        s_running = false;

        // Re-arm
        s_armed = true;
    }

    return false;
}

/* ==========================
 * Public API
 * ========================== */

esp_err_t speed_sensor_set_target_pulses(uint32_t periods)
{
    if (periods == 0) return ESP_ERR_INVALID_ARG;

    s_target_periods = periods;

    // Update watchpoint
    ESP_RETURN_ON_ERROR(pcnt_unit_remove_watch_point(s_pcnt_unit, (int)periods), TAG, "rm wp");
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(s_pcnt_unit, (int)periods), TAG, "add wp");

    return ESP_OK;
}

esp_err_t speed_sensor_init(uint32_t initial_periods)
{
    ESP_RETURN_ON_ERROR(speed_sensor_set_target_pulses(initial_periods), TAG, "set pulses pre-init");

    /* ---- MCPWM Capture timer ---- */
    {
        mcpwm_capture_timer_config_t tcfg = {
            .group_id = 0,
            .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
            .resolution_hz = CAPTURE_RES_HZ,  // explicit resolution; do NOT assume 80MHz
        };
        ESP_RETURN_ON_ERROR(mcpwm_new_capture_timer(&tcfg, &s_cap_timer), TAG, "new cap timer");

        mcpwm_capture_channel_config_t ccfg = {};
        ccfg.gpio_num = SPEED_GPIO;
        ccfg.prescale = 1;                 // do NOT use this as "every Nth pulse". Keep 1.
        ccfg.flags.pos_edge = 1;
        ccfg.flags.neg_edge = 0;
        ccfg.flags.pull_up = 1;            // depending on your wiring; otherwise 0
        
        ESP_RETURN_ON_ERROR(mcpwm_new_capture_channel(s_cap_timer, &ccfg, &s_cap_chan), TAG, "new cap chan");

        // Note: GPIO filter API may not be available in all IDF versions
        // Hardware glitch filtering handled by PCNT if needed

        mcpwm_capture_event_callbacks_t cbs = {
            .on_cap = on_capture_cb,
        };
        ESP_RETURN_ON_ERROR(mcpwm_capture_channel_register_event_callbacks(s_cap_chan, &cbs, NULL), TAG, "cap cbs");

        ESP_RETURN_ON_ERROR(mcpwm_capture_channel_enable(s_cap_chan), TAG, "cap chan en");
        ESP_RETURN_ON_ERROR(mcpwm_capture_timer_enable(s_cap_timer), TAG, "cap timer en");
        ESP_RETURN_ON_ERROR(mcpwm_capture_timer_start(s_cap_timer), TAG, "cap timer start");
    }

    /* ---- PCNT ---- */
    {
        pcnt_unit_config_t ucfg = {
            .low_limit = 0,
            .high_limit = (int)initial_periods,
        };
        ESP_RETURN_ON_ERROR(pcnt_new_unit(&ucfg, &s_pcnt_unit), TAG, "new pcnt unit");

        pcnt_chan_config_t chcfg = {
            .edge_gpio_num = SPEED_GPIO,
            .level_gpio_num = -1, // not used
        };
        ESP_RETURN_ON_ERROR(pcnt_new_channel(s_pcnt_unit, &chcfg, &s_pcnt_chan), TAG, "new pcnt chan");

        // Count rising edges only
        ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(s_pcnt_chan,
                                                        PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                        PCNT_CHANNEL_EDGE_ACTION_HOLD), TAG, "edge action");
        ESP_RETURN_ON_ERROR(pcnt_channel_set_level_action(s_pcnt_chan,
                                                         PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                         PCNT_CHANNEL_LEVEL_ACTION_KEEP), TAG, "level action");

        // Optional glitch filter in PCNT (separate from capture filter). With clean signal, keep minimal/off.
        // Example: reject pulses shorter than N ns; depends on IDF/SoC. Keep disabled unless needed.

        // Watchpoint exactly at N
        ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(s_pcnt_unit, (int)initial_periods), TAG, "add watchpoint");

        pcnt_event_callbacks_t pcbs = {
            .on_reach = on_pcnt_reach_cb,
        };
        ESP_RETURN_ON_ERROR(pcnt_unit_register_event_callbacks(s_pcnt_unit, &pcbs, NULL), TAG, "pcnt cbs");

        ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_pcnt_unit), TAG, "pcnt enable");
        pcnt_unit_stop(s_pcnt_unit); // start only after first edge
    }

    /* ---- Timeout GPTimer (200ms periodic alarm) ---- */
    {
        gptimer_config_t gcfg = {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,
            .direction = GPTIMER_COUNT_UP,
            .resolution_hz = 1000000, // 1MHz => 1us ticks
        };
        ESP_RETURN_ON_ERROR(gptimer_new_timer(&gcfg, &s_timeout_timer), TAG, "new gptimer");

        gptimer_event_callbacks_t tcbs = {
            .on_alarm = on_timeout_cb,
        };
        ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(s_timeout_timer, &tcbs, NULL), TAG, "timer cbs");

        gptimer_alarm_config_t acfg = {};
        acfg.alarm_count = UPDATE_TIMEOUT_US; // since 1us resolution
        acfg.reload_count = 0;
        acfg.flags.auto_reload_on_alarm = true;
        
        ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(s_timeout_timer, &acfg), TAG, "set alarm");

        ESP_RETURN_ON_ERROR(gptimer_enable(s_timeout_timer), TAG, "timer enable");
        ESP_RETURN_ON_ERROR(gptimer_start(s_timeout_timer), TAG, "timer start");
    }

    // Start in "armed" state (wait for first edge to define t_start precisely)
    s_armed = true;
    s_running = false;

    // Set initial N (watchpoint must exist already; adjust properly)
    // Remove+add watchpoint to ensure correct value
    // (some IDF versions require removing old watchpoint)
    pcnt_unit_remove_watch_point(s_pcnt_unit, (int)initial_periods);
    pcnt_unit_add_watch_point(s_pcnt_unit, (int)initial_periods);
    s_target_periods = initial_periods;

    ESP_LOGI(TAG, "Initialized. Target periods=%u, timeout=%dus", initial_periods, UPDATE_TIMEOUT_US);
    return ESP_OK;
}

/**
 * Compute RPM using last published result.
 * pulses_per_rev: how many periods correspond to one revolution
 */
float speed_sensor_get_rpm(uint32_t pulses_per_rev)
{
    static float last_rpm = 0.0f;

    bool has_new = false;
    uint32_t used = 0;
    uint32_t dt = 0;

    portENTER_CRITICAL(&s_mux);
    has_new = s_new_result;
    if (has_new) {
        used = s_used_periods;
        dt = s_dt_ticks;
        s_new_result = false;
    }
    portEXIT_CRITICAL(&s_mux);

    if (has_new && dt > 0 && pulses_per_rev > 0) {
        // frequency (Hz) = used_periods / (dt_ticks / CAPTURE_RES_HZ)
        // rpm = Hz * 60 / pulses_per_rev
        float seconds = (float)dt / (float)CAPTURE_RES_HZ;
        float hz = (float)used / seconds;
        last_rpm = (hz * 60.0f) / (float)pulses_per_rev;
    }

    return last_rpm;
}