// ============================================================================
// Project: ESP32 Treadmill Tacho (Band + Motor)
// File   : ESP32_treadmill_tacho.cpp
// Purpose: Main runtime – Setup, loop, and utility functions
// Note   : Sensor calculations split into separate files
// ============================================================================
// use this for compliling !!!!  Partition Scheme: "8M with spiffs (3MB APP/1.5MB SPIFFS)"

#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_web.h"
#include "ESP32_treadmill_tacho_wifi.h"
#include "ESP32_treadmill_tacho_ble.h"
#include "ESP32_treadmill_tacho_workout.h"
#include "ESP32_treadmill_hr_client.h"
// #include "ESP32_treadmill_tacho_filters.h"  // OLD SYSTEM REMOVED
// #include "ESP32_treadmill_tacho_sensor_common.h"  // OLD SYSTEM REMOVED
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <nvs.h>
#include <nvs_flash.h>
#include "driver/gpio.h"
#include <cstring>

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
TreadmillMetrics metrics;
BLEData bleData;

// Test mode flag (accessed by ISR callback)
volatile bool testdata = false;

WorkoutExecutor gWorkout; 
WiFiStatus wifi;
AsyncWebServer server(80);

// Speed filters (DEPRECATED - no longer used, will be removed)
// SpeedFilter bandFilter;
// SpeedFilter motorFilter;

// ============================================================================
// FORWARD DECLARATIONS - Functions in separate files
// ============================================================================
// OLD SYSTEM - DEPRECATED (updateMetrics() now called from ISR)
// void updateMetricsMotor(uint32_t now_ms, uint32_t now_us);
// void updateMetricsBand(uint32_t now_ms, uint32_t now_us);
uint8_t sensorSelection(bool init = false);
void enableTestdata(bool on);

// ============================================================================
// SETUP AND MAIN LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    
    // Reduce ESP-IDF component log verbosity (nvs, wifi, nimble, etc)
    esp_log_level_set("*", ESP_LOG_WARN);  // Set all components to WARN or higher
    esp_log_level_set("Preferences", ESP_LOG_NONE);  // Suppress NVS "NOT_FOUND" errors on first boot
    esp_log_level_set("NimBLE", ESP_LOG_NONE);       // Suppress BLE scan advertiser logs
    esp_log_level_set("NimBLEScan", ESP_LOG_NONE);   // Suppress "New advertiser" messages
    
    Serial.println("=== PCNT HARDWARE TREADMILL ===");
    checkAndFixNVS();

    loadSettings();
    printSettings();
    resetWorkout();
    
    // Speed filters removed - no longer using filtering (mpsSmooth = mps directly)
    // bandFilter.setFilterType((SpeedFilterType)storedGlobals.BAND_FILTER_TYPE);
    // motorFilter.setFilterType((SpeedFilterType)storedGlobals.MOTOR_FILTER_TYPE);

    metrics.workoutStartTime = millis();
    metrics.sessionStartTime = millis();
    metrics.lastUpdate = millis();
    metrics.cpuUsagePercent = 0;
    
    // Initialize WiFi Manager
    wifiInit();

    // Initialize web server. It will become accessible when WiFi connects.
    initWebServer();

    // Then initialize BLE services
    initFTMS_BLE();
    initBLE_RSC();
    initHR_BLE_Server();  // Heart Rate broadcasting service
    BLEAdvertising_init();
    FMTS_check();
    initHeartRateClient();  // Heart Rate client (receives from belt)

    // Initialize remaining peripherals
    initTachometer();
    
    Serial.println("Setup complete");
}

void loop() {
    static uint32_t lastUpdate = 0;
    static uint32_t speedIncDecElapsed = 0;
    static bool cpuInitialized = false;
    static uint32_t lastCpuUpdate = 0;
    static uint64_t lastTotalRunTime = 0;
    static uint64_t lastIdleRunTime = 0;
    
    uint32_t now = millis();
    
    // Initialize CPU timing on first loop
    if (!cpuInitialized) {
        lastCpuUpdate = now;
        cpuInitialized = true;
    }
    uint32_t delta = now - lastUpdate;
    lastUpdate = now;

    ElegantOTA.loop();   // required for OTA updates
    gWorkout.update(); // tick often
    heartRateClientLoop();

    processPendingTestMetrics();

    // TEST MODE: Now handled automatically by on_timeout_cb ISR (no loop code needed)
    // Debug: Log test mode metrics every 5 seconds
    extern volatile uint32_t test_isr_call_count;
    static uint32_t lastTestDebug = 0;
    static uint32_t lastCallCount = 0;
    if (testdata && (now - lastTestDebug >= 5000)) {
        lastTestDebug = now;
        float kmh = metrics.mpsSmooth * 3.6f;
        uint32_t calls = test_isr_call_count - lastCallCount;
        lastCallCount = test_isr_call_count;
        Serial.printf("[TEST-LOOP] Speed: %.2f km/h, Distance: %.2f m, ISR calls: %u (total: %u)\n", 
                      kmh, metrics.workoutDistance, calls, (unsigned)test_isr_call_count);
    }

    // FTMS status
    static uint32_t connectAt = 0;
    if (isNotificationEnabled(pFTMSStatus) && connectAt != UINT32_MAX) {
        if (connectAt == 0) connectAt = now;
        if (now - connectAt >= 1000) {
            sendFTMSStatusUpdate(0x01);
            connectAt = UINT32_MAX;
        }
    } else if (!isNotificationEnabled(pFTMSStatus)) {
        connectAt = 0;
    }

    // Speed control - ONLY when workout is active
    if (metrics.mps > 0 && metrics.isRunning && !metrics.isPaused) {
        if (speedIncDecElapsed > storedGlobals.SPEED_INC_DEC_FREQ_MS ) {
            physicalSpeedControl(metrics.targetSpeed, metrics.mps);
            speedIncDecElapsed = 0;
        }
        speedIncDecElapsed += delta;
    }
    
    // Calibration state machine (non-blocking)
    updateCalibration();

    // Update every 500ms
    static uint32_t lastMetricsUpdate = 0;
    
    if (now - lastMetricsUpdate >= 500) {
        lastMetricsUpdate = now;
        wifiLoop();
        
        // OLD SYSTEM REMOVED - metrics now updated directly from ISR callbacks
        // (on_pcnt_reach_cb and on_timeout_cb in ESP32_treadmill_tacho_sensor.cpp)
        
        if (bleData.clientConnected) {
            sendFTMS_BLE_Data();
            sendHR_BLE_Data();  // Broadcast HR from belt to Zwift
        }
                
        // Debug every 30 seconds
        static uint32_t lastDebug = 0;
        if (now - lastDebug >= 30000) {
            lastDebug = now;
            float kmh = metrics.mps * 3.6f;
            Serial.printf(" kmh=%.2f, CPU: %d%%\r\n", kmh, metrics.cpuUsagePercent);
            Serial.println();
        }
    }
    
    // CPU usage calculation using FreeRTOS runtime stats (falls back to heuristic if disabled)
    if (now - lastCpuUpdate >= 1000) {
#if (configUSE_TRACE_FACILITY == 1) && (configGENERATE_RUN_TIME_STATS == 1)
        UBaseType_t taskCount = uxTaskGetNumberOfTasks();
        UBaseType_t arraySize = taskCount + 4;  // cushion for tasks created between calls
        TaskStatus_t *statusArray = static_cast<TaskStatus_t *>(pvPortMalloc(arraySize * sizeof(TaskStatus_t)));
        if (statusArray != nullptr) {
            uint32_t totalRuntime32 = 0;
            UBaseType_t retrievedTasks = uxTaskGetSystemState(statusArray, arraySize, &totalRuntime32);
            if (retrievedTasks > 0 && totalRuntime32 > 0) {
                uint64_t totalRuntime = static_cast<uint64_t>(totalRuntime32);
                uint64_t idleRuntime = 0;
                for (UBaseType_t i = 0; i < retrievedTasks; ++i) {
                    const char *name = statusArray[i].pcTaskName;
                    if (name != nullptr && strncmp(name, "IDLE", 4) == 0) {
                        idleRuntime += static_cast<uint64_t>(statusArray[i].ulRunTimeCounter);
                    }
                }

                if (lastTotalRunTime != 0 && totalRuntime > lastTotalRunTime) {
                    uint64_t totalDelta = totalRuntime - lastTotalRunTime;
                    uint64_t idleDelta = (idleRuntime > lastIdleRunTime) ? (idleRuntime - lastIdleRunTime) : 0ULL;
                    if (idleDelta > totalDelta) {
                        idleDelta = totalDelta;
                    }

                    if (totalDelta > 0) {
                        uint64_t activeDelta = totalDelta - idleDelta;
                        uint32_t cpuPercent = static_cast<uint32_t>((activeDelta * 100ULL) / totalDelta);
                        if (cpuPercent > 100U) {
                            cpuPercent = 100U;
                        }
                        metrics.cpuUsagePercent = static_cast<uint8_t>(cpuPercent);
                    }
                }

                lastTotalRunTime = totalRuntime;
                lastIdleRunTime = idleRuntime;
            }

            vPortFree(statusArray);
        }
#else
        // Fallback heuristic when runtime statistics are disabled
        UBaseType_t taskCount = uxTaskGetNumberOfTasks();
        float cpuPercent = 0.0f;
        if (taskCount <= 10) {
            cpuPercent = 5.0f;
        } else if (taskCount <= 15) {
            cpuPercent = 5.0f + ((taskCount - 10) * 3.0f);
        } else if (taskCount <= 25) {
            cpuPercent = 20.0f + ((taskCount - 15) * 4.0f);
        } else {
            cpuPercent = 60.0f + ((taskCount - 25) * 2.0f);
        }

        if (cpuPercent > 100.0f) {
            cpuPercent = 100.0f;
        }
        metrics.cpuUsagePercent = static_cast<uint8_t>(cpuPercent);
#endif

        lastCpuUpdate = now;
    }
    
    // Yield to prevent watchdog timeout on CPU 1
    delay(1);
}
