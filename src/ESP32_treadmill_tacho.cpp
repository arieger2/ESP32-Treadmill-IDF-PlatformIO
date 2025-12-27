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
#include "ESP32_treadmill_tacho_filters.h"
#include "ESP32_treadmill_tacho_sensor_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <nvs.h>
#include <nvs_flash.h>
#include "driver/gpio.h"

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
TreadmillMetrics metrics;
BLEData bleData;

// Test data simulation counter (only used in test mode)
volatile int64_t test_pulse_count = 0;
volatile bool testdata = false;
volatile bool metrics_need_reset = false;  // Flag to reset metrics after test mode

WorkoutExecutor gWorkout; 
WiFiStatus wifi;
AsyncWebServer server(80);

// Speed filters for band and motor sensors
SpeedFilter bandFilter;
SpeedFilter motorFilter;

// ============================================================================
// FORWARD DECLARATIONS - Functions in separate files
// ============================================================================
void updateMetricsMotor(uint32_t now_ms, uint32_t now_us);
void updateMetricsBand(uint32_t now_ms, uint32_t now_us);
uint8_t sensorSelection(bool init = false);
void enableTestdata(bool on);
void generateTestData();

// ============================================================================
// SETUP AND MAIN LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    
    Serial.println("=== PCNT HARDWARE TREADMILL ===");
    checkAndFixNVS();

    loadSettings();
    printSettings();
    resetWorkout();
    
    // Initialize speed filters based on settings
    bandFilter.setFilterType((SpeedFilterType)storedGlobals.BAND_FILTER_TYPE);
    motorFilter.setFilterType((SpeedFilterType)storedGlobals.MOTOR_FILTER_TYPE);
    Serial.printf("Band filter: %d, Motor filter: %d\r\n", 
                  storedGlobals.BAND_FILTER_TYPE, storedGlobals.MOTOR_FILTER_TYPE);

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
    static uint32_t testdataElapsed = 0;
    static uint32_t speedIncDecElapsed = 0;
    static bool cpuInitialized = false;
    static uint32_t lastCpuUpdate = 0;
    static uint32_t lastTotalRunTime = 0;
    static uint32_t lastIdleRunTime = 0;
    
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

    // Test data
    testdataElapsed += delta;
    if (testdata && testdataElapsed > storedGlobals.TESTDATA_FREQ_MS) {
        generateTestData();
        testdataElapsed = 0;
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
    static uint8_t lastSensorMode = SENSOR_AUTO;  // Track previous sensor mode
    
    if (now - lastMetricsUpdate >= 500) {
        // Capture timestamps IMMEDIATELY to ensure consistent measurement windows
        uint32_t update_ms = millis();
        uint32_t update_us = micros();
        
        lastMetricsUpdate = now;
        wifiLoop();
        
        // Check if sensor mode changed
        uint8_t currentSensorMode = sensorSelection(false);
        if (currentSensorMode != lastSensorMode) {
            // Sensor mode changed - reset both filters and set metrics reset flag
            bandFilter.reset();
            motorFilter.reset();
            metrics_need_reset = true;
            Serial.printf("[SENSOR] Switched from %d to %d, filters and metrics reset\r\n", 
                         lastSensorMode, currentSensorMode);
            lastSensorMode = currentSensorMode;
        }
        
        if (currentSensorMode == SENSOR_MOTOR) 
            updateMetricsMotor(update_ms, update_us);
        else
            updateMetricsBand(update_ms, update_us);
        
        // Clear the metrics reset flag after update is called
        if (metrics_need_reset) {
            metrics_need_reset = false;
        }

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
    
    // CPU usage estimation based on number of running tasks
    if (now - lastCpuUpdate >= 1000) {
        // Get number of tasks in system
        UBaseType_t taskCount = uxTaskGetNumberOfTasks();
        
        // Baseline: ~8-12 tasks idle, 15-20 tasks when busy
        // Map task count to CPU percentage
        // Fewer tasks = lower CPU, more tasks = higher CPU
        float cpuPercent = 0.0f;
        if (taskCount <= 10) {
            cpuPercent = 5.0f;  // Minimal activity
        } else if (taskCount <= 15) {
            cpuPercent = 5.0f + ((taskCount - 10) * 3.0f);  // 5-20%
        } else if (taskCount <= 25) {
            cpuPercent = 20.0f + ((taskCount - 15) * 4.0f);  // 20-60%
        } else {
            cpuPercent = 60.0f + ((taskCount - 25) * 2.0f);  // 60-100%
        }
        
        if (cpuPercent > 100.0f) cpuPercent = 100.0f;
        metrics.cpuUsagePercent = (uint8_t)cpuPercent;
        
        lastCpuUpdate = now;
    }
}
