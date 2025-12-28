// ============================================================================
// Global types & cross-module flags
// Contains BLE UUIDs, metric structs, ISR timing variables.
// ============================================================================
#ifndef CONFIG_H
#define CONFIG_H
#pragma once
#include "esp_timer.h"
#include <Arduino.h>      // for String, Serial, etc.
#include <Preferences.h>  // for Preferences
#include <ESPmDNS.h>
#include <CircularBuffer.hpp>

// ============================================================================
// CONSTANTS AND DEFINES
// ============================================================================

// DUAL BLE SERVICES - RSC for data, FTMS for control
#define RSC_SERVICE_UUID BLEUUID((uint16_t)0x1814)
#define RSC_MEASUREMENT_UUID BLEUUID((uint16_t)0x2A53)
#define RSC_FEATURE_UUID BLEUUID((uint16_t)0x2A54)

#define FTMS_SERVICE_UUID BLEUUID((uint16_t)0x1826)
#define FTMS_FEATURE_UUID BLEUUID((uint16_t)0x2ACC)
#define TREADMILL_DATA_UUID BLEUUID((uint16_t)0x2ACD)
#define FTMS_CONTROL_POINT_UUID BLEUUID((uint16_t)0x2AD9)
#define FTMS_STATUS_UUID BLEUUID((uint16_t)0x2ADA)
#define SUPPORTED_SPEED_RANGE_UUID BLEUUID((uint16_t)0x2AD4)
#define SUPPORTED_INCLINATION_RANGE_UUID BLEUUID((uint16_t)0x2AD5)

// FTMS Control Point Operation Codes
#define FTMS_REQUEST_CONTROL        0x00
#define FTMS_RESET                  0x01
#define FTMS_SET_TARGET_SPEED       0x02
#define FTMS_SET_TARGET_INCLINATION 0x03
#define FTMS_START_RESUME           0x07
#define FTMS_STOP_PAUSE             0x08

// FTMS Response Codes
#define FTMS_SUCCESS                0x01
#define FTMS_NOT_SUPPORTED          0x02
#define FTMS_INVALID_PARAMETER      0x03

// ============================================================================
// DATA STRUCTURES
// ============================================================================
// ==== Sensor source selection ====
enum SensorSourceMode : uint8_t {
    SENSOR_AUTO  = 0,   // compare both sensors, select most stable (smallest RPM change)
    SENSOR_BAND  = 1,   // force belt sensor
    SENSOR_MOTOR = 2    // force motor sensor
};

// RR-Interval structure
struct RRValue {
  uint32_t timestamp;
  uint16_t rrValue;
};
// Heart Rate structure
struct HRValue {
  uint32_t timestamp;
  uint16_t hrValue;
};

struct TreadmillStoredGlobals {
    String WIFI_SSID = "Hawaii";
    String WIFI_PASSWORD = "catfood5";
    String BLE_DEVICE_NAME = "Rieger_Treatmill";
    
    // Hardware pins
    int INTERRUPT_PIN = 4;
    int MOTOR_INTERRUPT_PIN = 5;
    int LED_PIN = 2;
    int SPEED_UP_PIN = 10;
    int SPEED_DOWN_PIN = 11;
    int INCLINE_UP_PIN = 12;
    int INCLINE_DOWN_PIN = 13;
    
    // Timing settings
    uint32_t SPEED_INC_DEC_FREQ_MS = 100;  // in milliseconds
    uint32_t TESTDATA_FREQ_MS = 50;
    bool FORCE_USE_MOTOR = false;
    
    // Speed control calibration (learned from treadmill behavior)
    float SPEED_UP_RATE = 0.3f;      // km/h increase per second of button press
    float SPEED_DOWN_RATE = 0.3f;    // km/h decrease per second of button press
    uint32_t INERTIA_DELAY_MS = 2000; // Time for treadmill to stabilize after button release
    float OVERSHOOT_FACTOR = 1.1f;   // Typical overshoot multiplier (1.0 = no overshoot)
    
    // Treadmill mechanics
    uint32_t BELT_DISTANCE_MM = 200;         // Belt distance per revolution in mm
    uint32_t DEBOUNCE_THRESHOLD_US = 13;     // Minimum time between valid interrupts (adjustable via web UI)
    uint32_t MAX_REVOLUTION_TIME_MS = 2000; // Max time for valid revolution (1 second)
    uint32_t PULSES_PER_REV = 2;
    uint32_t MOTOR_PULSES_PER_REV = 12;   
    float MOTOR_TO_BELT_RATIO = 0.413793;     
    uint8_t SENSOR_SOURCE_MODE = SENSOR_AUTO;  // persisted
    uint8_t BAND_FILTER_TYPE = 1;     // Filter for band sensor: 0=None, 1=EMA, 2=Kalman, 3=Median
    uint8_t MOTOR_FILTER_TYPE = 1;    // Filter for motor sensor: 0=None, 1=EMA, 2=Kalman, 3=Median
}; 

struct TreadmillMetrics {
    float mpsSmooth = 0.0f;
    float rpm = 0;
    float motorRPM = 0.0f;
    float mps = 0;
    float mpsOffset = 0;
    uint8_t cpuUsagePercent = 0;   // CPU usage percentage (0-100)
    float workoutDistance = 0.0f;  // in meters (changed from mm)
    unsigned long workoutStartTime = 0;
    unsigned long lastUpdate = 0;
    
    // FTMS control metrics
    float targetSpeed = 0.0;        // km/h set by fitness apps
    float targetInclination = 0.0;  // percentage 
    float currentInclination = 0.0; // percentage
    uint16_t heartRateBpm = 0;      // latest heart-rate sample (BPM)
    uint32_t rrLastMs = 0;           // last RR interval in milliseconds
    CircularBuffer<RRValue, 5000> rrBuffer; // Ringpuffer für RR-Intervalle
    CircularBuffer<HRValue, 5000> hrBuffer; // Ringpuffer für Herzfrequenzwerte
    bool heartRateConnected = false;
    uint32_t heartRateLastUpdate = 0;
    uint16_t heartRateEnergy = 0;
    bool isRunning = false;
    bool isPaused = false;
    bool controlRequested = false;
    unsigned long sessionStartTime = 0;
};

struct BLEData {
    bool clientConnected = false;
    uint16_t instSpeed = 0;
    uint8_t instCadence = 85;
    uint16_t instStrideLength = 80;
    uint32_t totalDistance = 0;
    byte fakePos[1] = {1};
};


// ============================================================================
// NVS Settings API (load/save/print) + WiFi/mDNS helpers
// Central registry of NVS keys.
// ============================================================================
// Single instances (defined in the .cpp)
extern Preferences prefs;
extern TreadmillStoredGlobals storedGlobals;

// NVS keys centralized in one place
namespace NVSKeys {
    extern const char* NS;

    extern const char* WIFI_PASS;
    extern const char* WIFI_SSID;
    extern const char* BLE_NAME;

    extern const char* INT_PIN;
    extern const char* MOTOR_INT_PIN;
    extern const char* LED_PIN;
    extern const char* SPEED_UP_PIN;
    extern const char* SPEED_DN_PIN;
    extern const char* INCL_UP_PIN;
    extern const char* INCL_DN_PIN;

    extern const char* SPD_CTL_MS;
    extern const char* TESTDATA_MS;

    extern const char* BELT_MM;
    extern const char* DEBOUNCE_US;
    extern const char* MAXREV_MS;
    extern const char* PPR_BELT;
    extern const char* FORCE_MOTOR;
    extern const char* PPR_MOTOR;
    extern const char* RATIO;
    extern const char* SENSOR_MODE;
    extern const char* BAND_FILTER;
    extern const char* MOTOR_FILTER;
    
    extern const char* SPEED_UP_RATE;
    extern const char* SPEED_DN_RATE;
    extern const char* INERTIA_MS;
    extern const char* OVERSHOOT;
};

// ============================================================================
// EXTERNAL DECLARATIONS
// ============================================================================

// Global variables (defined in main file)
extern TreadmillMetrics metrics;
extern BLEData bleData;
extern TreadmillStoredGlobals storedGlobals;

// Parameter for GPIO switch
const uint32_t PULSE_US   = 50000;  // 50 ms Tastendruck
const uint32_t GAP_US     = 60000; // 120 ms Mindestabstand
const float    DEADBAND   = 0.05f;  // km/h – kleine Toleranz gegen Überschwingen

// WiFi status structure
struct WiFiStatus {
    bool connected = false;
    bool apMode = false;      // Track if we're in Access Point mode
    unsigned long lastCheck = 0;
    unsigned long lastAttempt = 0;
    int attempts = 0;
};

extern WiFiStatus wifi;

// Control variables
extern volatile bool testdata;

// Settings functions
String saveSettings();
void loadSettings();
void loadDefaultSettings();
void factoryReset();
void initTachometer();

// Utility functions
void enableTestdata(bool on);
void processPendingTestMetrics();
void resetWorkout();
void adjustOffsetUp();
void adjustOffsetDown();

// asyncron wait until automatic pin down
extern esp_timer_handle_t speedUpTimer;
extern esp_timer_handle_t speedDownTimer;
extern esp_timer_handle_t inclineUpTimer;
extern esp_timer_handle_t inclineDownTimer;
extern volatile bool speedUpBusy;
extern volatile bool speedUownBusy;
extern volatile bool inclineUpBusy;
extern volatile bool inclineDownBusy;

// API

String saveSettings();
void loadSettings();
void loadDefaultSettings();
void printSettings();
void checkAndFixNVS();

// WiFi connection functions
void connectToWiFi(bool init = false);
void checkWiFiConnection();
void startAccessPoint();

void resetWorkout();
uint8_t sensorSelection(bool init);

void writePress(uint8_t pin, bool pressed);
void writePressForDuration(uint8_t pin, uint32_t duration_ms);
void startSpeedCalibration();
void updateCalibration();
String getCalibrationStatus();


#endif // CONFIG_H