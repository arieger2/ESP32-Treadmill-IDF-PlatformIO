// ============================================================================
// Project: ESP32 Treadmill Tacho (Band + Motor)
// File   : ESP32_treadmill_tacho.cpp
// Purpose: Main runtime – Setup, loop, and utility functions
// Note   : Sensor calculations split into separate files
// ============================================================================
// use this for compliling !!!!  Partition Scheme: "8M with spiffs (3MB APP/1.5MB SPIFFS)"

#include <Arduino.h>
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_web.h"
#include "ESP32_treadmill_tacho_wifi.h"
#include "ESP32_treadmill_tacho_ble.h"
#include "ESP32_treadmill_tacho_workout.h"
#include "ESP32_treadmill_hr_client.h"
#include "ESP32_treadmill_tacho_bootlog.h"
#include "ESP32_treadmill_tacho_sensor.h"
#include "esp_coexist.h"
#include <nvs.h>
#include <nvs_flash.h>
#include "driver/gpio.h"
#include <cstring>

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
TreadmillMetrics metrics;
BLEData bleData;

// Test mode flag (accessed by ISR callback) - protected by critical section
static portMUX_TYPE testModeMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool testdata = false;

WorkoutExecutor gWorkout; 
WiFiStatus wifi;
AsyncWebServer server(80);

// ============================================================================
// TEST MODE PROTECTION (Issue #3)
// ============================================================================
void setTestMode(bool enable) {
    portENTER_CRITICAL(&testModeMux);
    testdata = enable;
    portEXIT_CRITICAL(&testModeMux);
}

bool getTestMode() {
    bool result;
    portENTER_CRITICAL(&testModeMux);
    result = testdata;
    portEXIT_CRITICAL(&testModeMux);
    return result;
}

// ============================================================================
// FORWARD DECLARATIONS - Functions in separate files
// ============================================================================
uint8_t sensorSelection(bool init = false);
void enableTestdata(bool on);

// ============================================================================
// INITIALIZATION STATUS TRACKING (Issue #8)
// ============================================================================
struct InitStatus {
    bool nvs = false;
    bool config = false;
    bool ble_ftms = false;
    bool ble_rsc = false;
    bool ble_hr_server = false;
    bool ble_adv = false;
    bool ble_hr_client = false;
    bool wifi = false;
    bool web = false;
    bool tachometer = false;
    
    void print() const {
        Serial.println("\n=== Init Status ===");
        Serial.printf("  NVS:           %s\n", nvs ? "✓" : "✗");
        Serial.printf("  Config:        %s\n", config ? "✓" : "✗");
        Serial.printf("  BLE FTMS:      %s\n", ble_ftms ? "✓" : "✗");
        Serial.printf("  BLE RSC:       %s\n", ble_rsc ? "✓" : "✗");
        Serial.printf("  BLE HR Server: %s\n", ble_hr_server ? "✓" : "✗");
        Serial.printf("  BLE Adv:       %s\n", ble_adv ? "✓" : "✗");
        Serial.printf("  BLE HR Client: %s\n", ble_hr_client ? "✓" : "✗");
        Serial.printf("  WiFi:          %s\n", wifi ? "✓" : "✗");
        Serial.printf("  Web Server:    %s\n", web ? "✓" : "✗");
        Serial.printf("  Tachometer:    %s\n", tachometer ? "✓" : "✗");
        Serial.println("===================\n");
    }
    
    bool critical() const {
        return nvs && config && tachometer;
    }
} initStatus;

// ============================================================================
// SETUP AND MAIN LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(100);  // Let serial stabilize
    
    // Initialize boot log capture FIRST to capture all subsequent output
    initBootLog();
    
    // Reduce ESP-IDF component log verbosity (nvs, wifi, nimble, etc)
    esp_log_level_set("*", ESP_LOG_WARN);  // Set all components to WARN or higher
    esp_log_level_set("Preferences", ESP_LOG_NONE);  // Suppress NVS "NOT_FOUND" errors on first boot
    esp_log_level_set("NimBLE", ESP_LOG_NONE);       // Suppress BLE scan advertiser logs
    esp_log_level_set("NimBLEScan", ESP_LOG_NONE);   // Suppress "New advertiser" messages
    
    logPrint("=== PCNT HARDWARE TREADMILL ===\n");
    
    // WiFi/BLE coexistence preference - BALANCED mode
    esp_err_t coex_err = esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    if (coex_err == ESP_OK) {
        logPrint("✓ Coexistence: BALANCED (WiFi and BLE equal priority)\n");
    } else {
        logPrintf("⚠ Coexistence setup failed: %s\n", esp_err_to_name(coex_err));
    }
    
    logPrint("Init NVS... ");
    checkAndFixNVS();
    initStatus.nvs = true;
    logPrint("✓\n");

    logPrint("Load config... ");
    loadSettings();
    initStatus.config = true;
    logPrint("✓\n");
    
    logPrint("\n--- Settings ---\n");
    printSettings();
    logPrint("---------------\n\n");
    resetWorkout();

    metrics.workoutStartTime = millis();
    metrics.sessionStartTime = millis();
    metrics.lastUpdate = millis();
    
    // Initialize BLE services FIRST (priority for Zwift connection)
    logPrint("Init BLE FTMS... ");
    initFTMS_BLE();
    initStatus.ble_ftms = true;
    logPrint("✓\n");
    
    logPrint("Init BLE RSC... ");
    initBLE_RSC();
    initStatus.ble_rsc = true;
    logPrint("✓\n");
    
    logPrint("Init BLE HR Server... ");
    initHR_BLE_Server();
    initStatus.ble_hr_server = true;
    logPrint("✓\n");
    
    logPrint("Init BLE Advertising... ");
    BLEAdvertising_init();
    initStatus.ble_adv = true;
    logPrint("✓\n");
    
    FMTS_check();
    
    logPrint("Init BLE HR Client... ");
    initHeartRateClient();
    initStatus.ble_hr_client = true;
    logPrint("✓\n");
    
    // Initialize WiFi AFTER BLE (lower priority)
    logPrint("Init WiFi... ");
    wifiInit();
    initStatus.wifi = true;
    logPrint("✓\n");

    // Initialize web server. It will become accessible when WiFi connects.
    logPrint("Init Web Server... ");
    initWebServer();
    initStatus.web = true;
    logPrint("✓\n");

    // Initialize remaining peripherals
    logPrint("Init Tachometer... ");
    initTachometer();
    initStatus.tachometer = true;
    logPrint("✓\n");
    
    initStatus.print();
    
    // Issue #1: Verify task affinity (Arduino runs loop on CPU1 by default)
    logPrint("=== Task Affinity ===\n");
    logPrintf("  Loop Task: CPU%d (Arduino default)\n", xPortGetCoreID());
    logPrint("  WiFi/BLE: CPU0 (ESP-IDF)\n");
    logPrint("=====================\n\n");
    
    if (initStatus.critical()) {
        logPrint("✓✓✓ CRITICAL SYSTEMS READY ✓✓✓\n");
    } else {
        logPrint("⚠⚠⚠ CRITICAL INIT FAILED ⚠⚠⚠\n");
    }
    
    logPrint("Setup complete\n");
    
    // Stop boot log capture now that setup is complete
    stopBootLog();
}

void loop() {
    static uint32_t lastUpdate = 0;
    static uint32_t speedIncDecElapsed = 0;
    
    uint32_t now = millis();
    uint32_t delta = now - lastUpdate;
    lastUpdate = now;

    ElegantOTA.loop();   // required for OTA updates
    gWorkout.update(); // tick often
    heartRateClientLoop();

    processPendingTestMetrics();

    // Update metrics from sensor data every 200ms (process ISR-collected data)
    static uint32_t lastSensorUpdate = 0;
    if (now - lastSensorUpdate >= 200) {
        lastSensorUpdate = now;
        
        // Check both sensors for new data and update metrics
        if (speed_sensor_get_sensor1()->new_result) {
            updateMetrics(metrics, speed_sensor_get_sensor1());
        }
        if (speed_sensor_get_sensor2()->new_result) {
            updateMetrics(metrics, speed_sensor_get_sensor2());
        }
    }

    // TEST MODE: Now handled automatically by on_timeout_cb ISR (no loop code needed)
    // Debug: Log test mode metrics every 5 seconds
    extern volatile uint32_t test_isr_call_count;
    static uint32_t lastTestDebug = 0;
    static uint32_t lastCallCount = 0;
    if (getTestMode() && (now - lastTestDebug >= 5000)) {
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
        
        // Apply speed filter in loop context (not ISR!)
        applySpeedFilter(metrics);
        
        if (bleData.clientConnected) {
            sendFTMS_BLE_Data();
            sendHR_BLE_Data();  // Broadcast HR from belt to Zwift
        }
                
        // Debug every 30 seconds
        static uint32_t lastDebug = 0;
        static float lastMps = 0.0f;
        if (now - lastDebug >= 30000 && fabsf(metrics.mps - lastMps) >= 0.1f) {
            lastDebug = now;
            lastMps = metrics.mps;
            float kmh = metrics.mps * 3.6f;
            Serial.printf(" kmh=%.2f\r\n", kmh);
            Serial.println();
        }
    }
    
    // Issue #2: Use FreeRTOS task delay for explicit yield
    vTaskDelay(pdMS_TO_TICKS(1));
}
