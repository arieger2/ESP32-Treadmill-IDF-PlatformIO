// ============================================================================
// NVS Settings Implementation
// saveSettings/loadSettings/loadDefaultSettings/printSettings
// ============================================================================
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_sensor.h"
#include "ESP32_treadmill_tacho_web.h"
#include "ESP32_treadmill_tacho_bootlog.h"
#include <nvs.h>
#include <nvs_flash.h>
#include "esp_system.h"
#include "esp_chip_info.h"
#include "spi_flash_mmap.h"

// Define the single instances
Preferences prefs;
TreadmillStoredGlobals storedGlobals;

// Define timer handles and busy flags
esp_timer_handle_t speedUpTimer = nullptr;
esp_timer_handle_t speedDownTimer = nullptr;
esp_timer_handle_t inclineUpTimer = nullptr;
esp_timer_handle_t inclineDownTimer = nullptr;
volatile bool speedUpBusy = false;
volatile bool speedUownBusy = false;
volatile bool inclineUpBusy = false;
volatile bool inclineDownBusy = false;

// Define key strings exactly once here
const char* NVSKeys::NS            = "treadmill";

const char* NVSKeys::WIFI_PASS     = "wifiPass";
const char* NVSKeys::WIFI_SSID     = "wifiSSID";
const char* NVSKeys::BLE_NAME      = "bleDeviceName";     // 13 (OK)

const char* NVSKeys::INT_PIN       = "interruptPin";      // 12 (OK)
const char* NVSKeys::MOTOR_INT_PIN = "mIntPin";           // was "motorInterruptPin"
const char* NVSKeys::SPEED_UP_PIN  = "speedUpPin";        // 11 (OK)
const char* NVSKeys::SPEED_DN_PIN  = "speedDownPin";      // 13 (OK)
const char* NVSKeys::INCL_UP_PIN   = "inclUpPin";         // was "inclineUpPin"
const char* NVSKeys::INCL_DN_PIN   = "inclDnPin";         // was "inclineDownPin"

const char* NVSKeys::SPD_CTL_MS    = "spdCtlMs";          // was "speedIncDecFreqMs"
const char* NVSKeys::TESTDATA_MS   = "testdataFreqMs";    // 14 (OK)

const char* NVSKeys::BELT_MM       = "beltDistanceMM";    // 14 (OK)
const char* NVSKeys::DEBOUNCE_US   = "debThUs";           // was "debounceThresholdUs"
const char* NVSKeys::MAXREV_MS     = "maxRevMs";          // was "maxRevolutionTimeUs"
const char* NVSKeys::PPR_BELT      = "pprBelt";           // was "pulsePerRevision"
const char* NVSKeys::PPR_MULT_BAND = "pprMultB";          // Band pulse multiplier
const char* NVSKeys::PPR_MULT_MTR  = "pprMultM";          // Motor pulse multiplier
const char* NVSKeys::FORCE_MOTOR   = "forceMtr";          // FORCE_USE_MOTOR flag
const char* NVSKeys::PPR_MOTOR     = "pprMotor";          // was "motorPulsesPerRev"

const char* NVSKeys::RATIO         = "ratio";             // was "motorToBeltRatio"
const char* NVSKeys::SENSOR_MODE   = "sensorSrc";         // NEW  <= short key like your others
const char* NVSKeys::BAND_FILTER   = "bandFlt";           // Band sensor filter type
const char* NVSKeys::MOTOR_FILTER  = "motorFlt";          // Motor sensor filter type

const char* NVSKeys::SPEED_UP_RATE = "spdUpRate";         // km/h per second button press
const char* NVSKeys::SPEED_DN_RATE = "spdDnRate";         // km/h per second button press
const char* NVSKeys::INERTIA_MS    = "inertiaMs";         // Stabilization delay after release
const char* NVSKeys::OVERSHOOT     = "overshoot";         // Overshoot multiplier factor

// Template function for putOrReplace - handles float, int, int32_t, uint32_t, int64_t, long
template<typename T>
static size_t putOrReplace(Preferences& p, const char* key, T value) {
  size_t written = 0;
  if (std::is_same<T, float>::value) {
    written = p.putFloat(key, static_cast<float>(value));
    if (written == 0) { p.remove(key); written = p.putFloat(key, static_cast<float>(value)); }
  } else if (std::is_same<T, int>::value || std::is_same<T, int32_t>::value) {
    written = p.putInt(key, static_cast<int32_t>(value));
    if (written == 0) { p.remove(key); written = p.putInt(key, static_cast<int32_t>(value)); }
  } else if (std::is_same<T, uint32_t>::value) {
    written = p.putUInt(key, static_cast<uint32_t>(value));
    if (written == 0) { p.remove(key); written = p.putUInt(key, static_cast<uint32_t>(value)); }
  } else if (std::is_same<T, int64_t>::value || std::is_same<T, long>::value) {
    written = p.putLong(key, static_cast<int64_t>(value));
    if (written == 0) { p.remove(key); written = p.putLong(key, static_cast<int64_t>(value)); }
  }
  return written;
}

// Specialization for String type
template<>
size_t putOrReplace<String>(Preferences& p, const char* key, String value) {
  size_t written = p.putString(key, value);
  if (written == 0) { p.remove(key); written = p.putString(key, value); }
  return written;
}

void systemInfo() {
  delay(1000);
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  logPrint("=== ESP32 Chip Info ===\n");
  logPrintf("Chip model: %s\r\n", ESP.getChipModel());
  logPrintf("Chip revision: %d\r\n", ESP.getChipRevision());
  logPrintf("Cores: %d\r\n", ESP.getChipCores());
  logPrintf("CPU Freq: %u MHz\r\n", ESP.getCpuFreqMHz());
  logPrintf("Flash size: %u MB\r\n", ESP.getFlashChipSize() / (1024 * 1024));
  logPrintf("Flash speed: %u Hz\r\n", ESP.getFlashChipSpeed());
  logPrintf("PSRAM size: %u MB\r\n", ESP.getPsramSize() / (1024 * 1024));
  logPrintf("Heap size: %u KB\r\n", ESP.getHeapSize() / 1024);
  logPrintf("Free heap: %u KB\r\n", ESP.getFreeHeap() / 1024);
  logPrintf("Sketch size: %u KB\r\n", ESP.getSketchSize() / 1024);
  logPrintf("Free sketch space: %u KB\r\n", ESP.getFreeSketchSpace() / 1024);
  logPrintf("SDK Version: %s\r\n", ESP.getSdkVersion());

  logPrint("\r\nFeatures:\n");
  if (chip_info.features & CHIP_FEATURE_EMB_FLASH) logPrint(" - Embedded Flash\n");
  if (chip_info.features & CHIP_FEATURE_WIFI_BGN)  logPrint(" - WiFi BGN\n");
  if (chip_info.features & CHIP_FEATURE_BLE)       logPrint(" - BLE\n");
  if (chip_info.features & CHIP_FEATURE_BT)        logPrint(" - Classic BT\n");
}

// ============================================================================
// SETTINGS FUNCTIONS (unchanged)
// ============================================================================
String saveSettings() {
  prefs.begin(NVSKeys::NS, false);

  struct Row { const char* k; size_t w; };
  Row r[] = {
    {NVSKeys::RATIO,         putOrReplace(prefs, NVSKeys::RATIO,        storedGlobals.MOTOR_TO_BELT_RATIO)},
    {NVSKeys::WIFI_PASS,     putOrReplace(prefs, NVSKeys::WIFI_PASS,    storedGlobals.WIFI_PASSWORD)},
    {NVSKeys::WIFI_SSID,     putOrReplace(prefs, NVSKeys::WIFI_SSID,    storedGlobals.WIFI_SSID)},
    {NVSKeys::BLE_NAME,      putOrReplace(prefs, NVSKeys::BLE_NAME,     storedGlobals.BLE_DEVICE_NAME)},
    {NVSKeys::INT_PIN,       putOrReplace(prefs, NVSKeys::INT_PIN,      storedGlobals.INTERRUPT_PIN)},
    {NVSKeys::MOTOR_INT_PIN, putOrReplace(prefs, NVSKeys::MOTOR_INT_PIN,storedGlobals.MOTOR_INTERRUPT_PIN)},
    {NVSKeys::SPEED_UP_PIN,  putOrReplace(prefs, NVSKeys::SPEED_UP_PIN, storedGlobals.SPEED_UP_PIN)},
    {NVSKeys::SPEED_DN_PIN,  putOrReplace(prefs, NVSKeys::SPEED_DN_PIN, storedGlobals.SPEED_DOWN_PIN)},
    {NVSKeys::INCL_UP_PIN,   putOrReplace(prefs, NVSKeys::INCL_UP_PIN,  storedGlobals.INCLINE_UP_PIN)},
    {NVSKeys::INCL_DN_PIN,   putOrReplace(prefs, NVSKeys::INCL_DN_PIN,  storedGlobals.INCLINE_DOWN_PIN)},
    {NVSKeys::SPD_CTL_MS,    putOrReplace(prefs, NVSKeys::SPD_CTL_MS,   storedGlobals.SPEED_INC_DEC_FREQ_MS)},
    {NVSKeys::TESTDATA_MS,   putOrReplace(prefs, NVSKeys::TESTDATA_MS,  storedGlobals.TESTDATA_FREQ_MS)},
    {NVSKeys::BELT_MM,       putOrReplace(prefs, NVSKeys::BELT_MM,      storedGlobals.BELT_DISTANCE_MM)},
    {NVSKeys::DEBOUNCE_US,   putOrReplace(prefs, NVSKeys::DEBOUNCE_US,  storedGlobals.DEBOUNCE_THRESHOLD_US)},
    {NVSKeys::MAXREV_MS,     putOrReplace(prefs, NVSKeys::MAXREV_MS,    storedGlobals.MAX_REVOLUTION_TIME_MS)},
    {NVSKeys::PPR_BELT,      putOrReplace(prefs, NVSKeys::PPR_BELT,     storedGlobals.PULSES_PER_REV)},
    {NVSKeys::PPR_MULT_BAND, putOrReplace(prefs, NVSKeys::PPR_MULT_BAND,storedGlobals.BAND_PULSE_MULTIPLIER)},
    {NVSKeys::PPR_MULT_MTR,  putOrReplace(prefs, NVSKeys::PPR_MULT_MTR, storedGlobals.MOTOR_PULSE_MULTIPLIER)},
    {NVSKeys::FORCE_MOTOR,   putOrReplace(prefs, NVSKeys::FORCE_MOTOR,  (int32_t)storedGlobals.FORCE_USE_MOTOR)},
    {NVSKeys::PPR_MOTOR,     putOrReplace(prefs, NVSKeys::PPR_MOTOR,    storedGlobals.MOTOR_PULSES_PER_REV)},
    {NVSKeys::SENSOR_MODE,   putOrReplace(prefs, NVSKeys::SENSOR_MODE,  (int32_t)storedGlobals.SENSOR_SOURCE_MODE)},
    {NVSKeys::BAND_FILTER,   putOrReplace(prefs, NVSKeys::BAND_FILTER,  (int32_t)storedGlobals.BAND_FILTER_TYPE)},
    {NVSKeys::MOTOR_FILTER,  putOrReplace(prefs, NVSKeys::MOTOR_FILTER, (int32_t)storedGlobals.MOTOR_FILTER_TYPE)},
    {NVSKeys::SPEED_UP_RATE, putOrReplace(prefs, NVSKeys::SPEED_UP_RATE, storedGlobals.SPEED_UP_RATE)},
    {NVSKeys::SPEED_DN_RATE, putOrReplace(prefs, NVSKeys::SPEED_DN_RATE, storedGlobals.SPEED_DOWN_RATE)},
    {NVSKeys::INERTIA_MS,    putOrReplace(prefs, NVSKeys::INERTIA_MS,    storedGlobals.INERTIA_DELAY_MS)},
    {NVSKeys::OVERSHOOT,     putOrReplace(prefs, NVSKeys::OVERSHOOT,     storedGlobals.OVERSHOOT_FACTOR)},
  };

  String out = "Settings saved:\r\n";
  String ret = "";
  for (auto &x: r) {
    out += (x.w > 0) ? "[OK]   " : "[WARN] ";
    out += x.k; out += " -> "; out += x.w; out += " bytes\r\n";
    if (x.w == 0) ret = "not saved - programming error"; 
  }
  
  prefs.end();  // Close AFTER verification loop
  logPrint(out.c_str());
  return ret;
}

void loadSettings() {
  prefs.begin(NVSKeys::NS, false);

  storedGlobals.WIFI_PASSWORD   = prefs.getString(NVSKeys::WIFI_PASS, "");
  storedGlobals.WIFI_SSID       = prefs.getString(NVSKeys::WIFI_SSID,  "");
  storedGlobals.BLE_DEVICE_NAME = prefs.getString(NVSKeys::BLE_NAME,   "Rieger_Treatmill");

  storedGlobals.INTERRUPT_PIN        = prefs.getInt (NVSKeys::INT_PIN,       4);
  storedGlobals.MOTOR_INTERRUPT_PIN  = prefs.getInt (NVSKeys::MOTOR_INT_PIN, 6);
  storedGlobals.SPEED_UP_PIN         = prefs.getInt (NVSKeys::SPEED_UP_PIN, 10);
  storedGlobals.SPEED_DOWN_PIN       = prefs.getInt (NVSKeys::SPEED_DN_PIN, 11);
  storedGlobals.INCLINE_UP_PIN       = prefs.getInt (NVSKeys::INCL_UP_PIN,  12);
  storedGlobals.INCLINE_DOWN_PIN     = prefs.getInt (NVSKeys::INCL_DN_PIN,  13);

  storedGlobals.SPEED_INC_DEC_FREQ_MS = prefs.getUInt (NVSKeys::SPD_CTL_MS, 100);
  storedGlobals.TESTDATA_FREQ_MS      = prefs.getUInt (NVSKeys::TESTDATA_MS, 10);
  storedGlobals.BELT_DISTANCE_MM      = prefs.getLong (NVSKeys::BELT_MM, 200);
  storedGlobals.DEBOUNCE_THRESHOLD_US = prefs.getLong (NVSKeys::DEBOUNCE_US, 13);
  storedGlobals.MAX_REVOLUTION_TIME_MS= prefs.getLong (NVSKeys::MAXREV_MS, 2000);
  storedGlobals.PULSES_PER_REV        = prefs.getLong (NVSKeys::PPR_BELT, 2);
  storedGlobals.BAND_PULSE_MULTIPLIER = prefs.getUInt (NVSKeys::PPR_MULT_BAND, 1);
  storedGlobals.MOTOR_PULSE_MULTIPLIER= prefs.getUInt (NVSKeys::PPR_MULT_MTR, 1);
  storedGlobals.MOTOR_PULSES_PER_REV  = prefs.getLong (NVSKeys::PPR_MOTOR, 12);
  storedGlobals.MOTOR_TO_BELT_RATIO   = prefs.getFloat(NVSKeys::RATIO, 0.413793);
  storedGlobals.FORCE_USE_MOTOR       = prefs.getBool (NVSKeys::FORCE_MOTOR, false);
  storedGlobals.SENSOR_SOURCE_MODE = (uint8_t)prefs.getInt(NVSKeys::SENSOR_MODE, (int32_t)SENSOR_AUTO);
  storedGlobals.BAND_FILTER_TYPE   = (uint8_t)prefs.getInt(NVSKeys::BAND_FILTER, 1);   // Default: EMA
  storedGlobals.MOTOR_FILTER_TYPE  = (uint8_t)prefs.getInt(NVSKeys::MOTOR_FILTER, 1);  // Default: EMA

  storedGlobals.SPEED_UP_RATE      = prefs.getFloat(NVSKeys::SPEED_UP_RATE, 0.3f);
  storedGlobals.SPEED_DOWN_RATE    = prefs.getFloat(NVSKeys::SPEED_DN_RATE, 0.3f);
  storedGlobals.INERTIA_DELAY_MS   = prefs.getUInt (NVSKeys::INERTIA_MS, 2000);
  storedGlobals.OVERSHOOT_FACTOR   = prefs.getFloat(NVSKeys::OVERSHOOT, 1.1f);

  logPrintf("[NVS] Loaded calibration: UP=%.3f, DOWN=%.3f\n", 
                storedGlobals.SPEED_UP_RATE, storedGlobals.SPEED_DOWN_RATE);

  prefs.end();

  // Clamp to valid ranges
  if (storedGlobals.DEBOUNCE_THRESHOLD_US < 1) storedGlobals.DEBOUNCE_THRESHOLD_US = 1;
  if (storedGlobals.DEBOUNCE_THRESHOLD_US > 13) storedGlobals.DEBOUNCE_THRESHOLD_US = 13;
  
  if (storedGlobals.SENSOR_SOURCE_MODE > SENSOR_MOTOR) {
      storedGlobals.SENSOR_SOURCE_MODE = SENSOR_AUTO;
  }
  
  // Clamp filter types to valid range (0-3)
  if (storedGlobals.BAND_FILTER_TYPE > 3) storedGlobals.BAND_FILTER_TYPE = 1;
  if (storedGlobals.MOTOR_FILTER_TYPE > 3) storedGlobals.MOTOR_FILTER_TYPE = 1;
  
  // Clamp pulse multipliers to valid range (1-100)
  if (storedGlobals.BAND_PULSE_MULTIPLIER < 1) storedGlobals.BAND_PULSE_MULTIPLIER = 1;
  if (storedGlobals.BAND_PULSE_MULTIPLIER > 100) storedGlobals.BAND_PULSE_MULTIPLIER = 100;
  if (storedGlobals.MOTOR_PULSE_MULTIPLIER < 1) storedGlobals.MOTOR_PULSE_MULTIPLIER = 1;
  if (storedGlobals.MOTOR_PULSE_MULTIPLIER > 100) storedGlobals.MOTOR_PULSE_MULTIPLIER = 100;
}

void loadDefaultSettings() {
    storedGlobals.WIFI_SSID = "";
    storedGlobals.WIFI_PASSWORD = "";
    storedGlobals.BLE_DEVICE_NAME = "Rieger_Treatmill";
    storedGlobals.INTERRUPT_PIN = 4;
    storedGlobals.MOTOR_INTERRUPT_PIN = 6;
    storedGlobals.SPEED_UP_PIN = 10;
    storedGlobals.SPEED_DOWN_PIN = 11;
    storedGlobals.INCLINE_UP_PIN = 12;
    storedGlobals.INCLINE_DOWN_PIN = 13;
    storedGlobals.SPEED_INC_DEC_FREQ_MS = 100;
    storedGlobals.TESTDATA_FREQ_MS = 10;
    storedGlobals.BELT_DISTANCE_MM = 200;
    storedGlobals.DEBOUNCE_THRESHOLD_US = 13;
    storedGlobals.MAX_REVOLUTION_TIME_MS = 2000;
    storedGlobals.PULSES_PER_REV = 2;
    storedGlobals.BAND_PULSE_MULTIPLIER = 1;
    storedGlobals.MOTOR_PULSE_MULTIPLIER = 1;
    storedGlobals.MOTOR_PULSES_PER_REV = 12;
    storedGlobals.MOTOR_TO_BELT_RATIO  = 0.413793;
    storedGlobals.FORCE_USE_MOTOR = true;
    storedGlobals.SENSOR_SOURCE_MODE = SENSOR_AUTO;
}

// Call this after loadSettings() / saveSettings()
void printSettings() {
  logPrint("===== TREADMILL SETTINGS (loaded) =====\n");
  logPrintf("WiFi SSID             : %s\r\n", storedGlobals.WIFI_SSID.c_str());
  logPrintf("BLE Device Name       : %s\r\n", storedGlobals.BLE_DEVICE_NAME.c_str());
  logPrint("\n");

  logPrint("Pins:\r\n"); 
  logPrintf("  Band Interrupt Pin  : %d\r\n", storedGlobals.INTERRUPT_PIN);
  logPrintf("  Motor Interrupt Pin : %d\r\n", storedGlobals.MOTOR_INTERRUPT_PIN);
  logPrintf("  Speed Up Pin        : %d\r\n", storedGlobals.SPEED_UP_PIN); 
  logPrintf("  Speed Down Pin      : %d\r\n", storedGlobals.SPEED_DOWN_PIN);
  logPrintf("  Incline Up Pin      : %d\r\n", storedGlobals.INCLINE_UP_PIN); 
  logPrintf("  Incline Down Pin    : %d\r\n", storedGlobals.INCLINE_DOWN_PIN);
  logPrint("\n");

  logPrint("Timing:\r\n"); 
  logPrintf("  Speed Ctl Freq (ms) : %u\r\n", storedGlobals.SPEED_INC_DEC_FREQ_MS); 
  logPrintf("  Testdata Freq (ms)  : %u\r\n", storedGlobals.TESTDATA_FREQ_MS); 
  logPrint("\n");

  logPrint("Mechanics:\r\n"); 
  logPrintf("  Belt Distance (mm)  : %lu\r\n", (unsigned long)storedGlobals.BELT_DISTANCE_MM); 
  logPrintf("  Debounce Thresh (us): %lu\r\n", (unsigned long)storedGlobals.DEBOUNCE_THRESHOLD_US); 
  logPrintf("  Max Rev Time (ms)   : %lu\r\n", (unsigned long)storedGlobals.MAX_REVOLUTION_TIME_MS); 
  logPrintf("  Pulses/Rev (belt)   : %lu\r\n", (unsigned long)storedGlobals.PULSES_PER_REV); 
  logPrintf("  Pulses/Rev (motor)  : %lu\r\n", (unsigned long)storedGlobals.MOTOR_PULSES_PER_REV); 
  logPrintf("  Motor→Belt Ratio    : %.6f\r\n", storedGlobals.MOTOR_TO_BELT_RATIO); 
  logPrintf("  Force Use Motor     : %s\r\n", storedGlobals.FORCE_USE_MOTOR ? "YES" : "NO"); 
  
  const char* src =
    (storedGlobals.SENSOR_SOURCE_MODE == SENSOR_BAND)  ? "BAND"  :
    (storedGlobals.SENSOR_SOURCE_MODE == SENSOR_MOTOR) ? "MOTOR" : "AUTO";
  logPrintf("  Speed Sensor Source : %s (%u)\r\n", src, storedGlobals.SENSOR_SOURCE_MODE);
  logPrint("=======================================\r\n\n");
}

// Add this function to check and fix NVS
void checkAndFixNVS() {
    logPrint("=== Borad Infos ===\n");
    systemInfo();
    logPrint("\n");
    logPrint("=== NVS DIAGNOSTIC ===\n");
    size_t sz = ESP.getFlashChipSize();  // bytes
    logPrintf("Flash size: %u bytes (%.2f MB)\r\n", (unsigned)sz, sz / (1024.0*1024.0));
    logPrint("\n");
    
    // Check NVS initialization
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        logPrint("NVS partition was truncated and needs to be erased\n");
        // NVS partition was truncated and needs to be erased
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
        logPrint("NVS erased and reinitialized\n");
    }
    ESP_ERROR_CHECK(err);
    
    // Get NVS statistics
    nvs_stats_t nvs_stats;
    err = nvs_get_stats(NULL, &nvs_stats);
    if (err == ESP_OK) {
        logPrint("NVS Statistics:\r\n");
        logPrintf("  Used entries: %d\r\n", nvs_stats.used_entries);
        logPrintf("  Free entries: %d\r\n", nvs_stats.free_entries);
        logPrintf("  Total entries: %d\r\n", nvs_stats.total_entries);
        logPrintf("  Namespace count: %d\r\n", nvs_stats.namespace_count);
    } else {
        logPrintf("Error getting NVS stats: %s\r\n", esp_err_to_name(err));
    }
    
    // Test preferences access
    Preferences testPrefs;
    bool canOpen = testPrefs.begin("test", false);
    if (canOpen) {
        logPrint("✅ Preferences can be opened for write\n");
        testPrefs.putString("test", "working");
        String result = testPrefs.getString("test", "failed");
        logPrintf("Write/Read test: %s\r\n", result.c_str()); 
        logPrint("\n");
        testPrefs.end();
    } else {
        logPrint("❌ Cannot open preferences for write\n");
    }
    
    // Test your treadmill namespace specifically
    bool canOpenTreadmill = testPrefs.begin("treadmill", false);
    if (canOpenTreadmill) {
        logPrint("✅ Treadmill namespace accessible\n");
        size_t freeEntries = testPrefs.freeEntries();
        logPrintf("Free entries in treadmill namespace: %d\r\n", freeEntries);
        testPrefs.end();
    } else {
        logPrint("❌ Cannot access treadmill namespace\n");
    }
    
    logPrint("======================\r\n\n");
}

// ============================================================================
// PIN AND HARDWARE SETUP
// ============================================================================
void pinModeSetup() {
    // Configure interrupt pins using native ESP-IDF
    // NO internal pull-up/down - using external stronger pull-up for long cable (1.5m)
    // Note: For PCNT, we don't need interrupts, just input mode
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;  // PCNT doesn't need GPIO interrupts
    io_conf.pin_bit_mask = (1ULL << storedGlobals.INTERRUPT_PIN) | (1ULL << storedGlobals.MOTOR_INTERRUPT_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;     // Enable internal pull-up
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    
    // Reset GPIO to ensure clean state before PCNT takes over
    gpio_reset_pin((gpio_num_t)storedGlobals.INTERRUPT_PIN);
    gpio_reset_pin((gpio_num_t)storedGlobals.MOTOR_INTERRUPT_PIN);
    
    // Reconfigure after reset
    gpio_config(&io_conf);

    // Configure output pins for speed and incline control with ESP-IDF
    gpio_config_t out_conf = {};
    out_conf.intr_type = GPIO_INTR_DISABLE;
    out_conf.mode = GPIO_MODE_OUTPUT;
    out_conf.pin_bit_mask = (1ULL << storedGlobals.SPEED_UP_PIN) | 
                            (1ULL << storedGlobals.SPEED_DOWN_PIN) |
                            (1ULL << storedGlobals.INCLINE_UP_PIN) | 
                            (1ULL << storedGlobals.INCLINE_DOWN_PIN);
    out_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&out_conf);
    
    // Set to HIGH (relay inactive) immediately after config
    gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 1);
    gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 1);
    gpio_set_level((gpio_num_t)storedGlobals.INCLINE_UP_PIN, 1);
    gpio_set_level((gpio_num_t)storedGlobals.INCLINE_DOWN_PIN, 1);
}

// Timer-Callbacks laufen im Timer-Task → Pin sicher loslassen
static void speedUpReleaseCb(void*) {
    speedUpBusy = false;
    gpio_set_level((gpio_num_t)storedGlobals.SPEED_UP_PIN, 1);  // HIGH = relay inactive
}

static void speedDownReleaseCb(void*) {
    speedUownBusy = false;
    gpio_set_level((gpio_num_t)storedGlobals.SPEED_DOWN_PIN, 1); // HIGH = relay inactive
}

static void inclineUpReleaseCb(void*) {
    inclineUpBusy = false;
    gpio_set_level((gpio_num_t)storedGlobals.INCLINE_UP_PIN, 1);  // HIGH = relay inactive
}

static void inclineDownReleaseCb(void*) {
    inclineDownBusy = false;
    gpio_set_level((gpio_num_t)storedGlobals.INCLINE_DOWN_PIN, 1); // HIGH = relay inactive
}

void pinTimerSetup() {
    esp_timer_create_args_t upArgs   = {.callback = &speedUpReleaseCb,   .arg=nullptr, .dispatch_method=ESP_TIMER_TASK, .name="upRel"};
    esp_timer_create_args_t dnArgs   = {.callback = &speedDownReleaseCb, .arg=nullptr, .dispatch_method=ESP_TIMER_TASK, .name="dnRel"};
    esp_timer_create_args_t incUpArgs   = {.callback = &inclineUpReleaseCb,   .arg=nullptr, .dispatch_method=ESP_TIMER_TASK, .name="incUpRel"};
    esp_timer_create_args_t incDnArgs   = {.callback = &inclineDownReleaseCb, .arg=nullptr, .dispatch_method=ESP_TIMER_TASK, .name="incDnRel"};
    esp_timer_create(&upArgs, &speedUpTimer);
    esp_timer_create(&dnArgs, &speedDownTimer);
    esp_timer_create(&incUpArgs, &inclineUpTimer);
    esp_timer_create(&incDnArgs, &inclineDownTimer);
}

void initTachometer() {
    pinModeSetup();
    pinTimerSetup();
    // Initialize new MCPWM + PCNT sensor system so GPTimer timeout callback runs
    esp_err_t err = speed_sensor1_init(storedGlobals.PULSES_PER_REV * storedGlobals.BAND_PULSE_MULTIPLIER,
                                       storedGlobals.INTERRUPT_PIN);
    if (err != ESP_OK) {
        logPrintf("[INIT][ERR] speed_sensor1_init failed: %s\r\n",
                      esp_err_to_name(err));
    }

    err = speed_sensor2_init(storedGlobals.MOTOR_PULSES_PER_REV * storedGlobals.MOTOR_PULSE_MULTIPLIER,
                             storedGlobals.MOTOR_INTERRUPT_PIN);
    if (err != ESP_OK) {
        logPrintf("[INIT][ERR] speed_sensor2_init failed: %s\r\n",
                      esp_err_to_name(err));
    }

    logPrintf("[INIT] Using new MCPWM+PCNT sensor system (GPIO %d, %d)\r\n",
                  storedGlobals.INTERRUPT_PIN, storedGlobals.MOTOR_INTERRUPT_PIN);
    logPrint("Hall sensor ready: Hardware ISR-based counting enabled\n");
    testdata = false;
}

void factoryReset() {
    // Fully erase the NVS partition
    esp_err_t err;
    err = nvs_flash_erase();
    Serial.printf("nvs_flash_erase: %s\r\n", esp_err_to_name(err));

    // Re-initialize NVS
    err = nvs_flash_init();
    Serial.printf("nvs_flash_init: %s\r\n", esp_err_to_name(err));
}

void resetWorkout() {
    uint32_t now = millis();

    metrics.mps = 0.0f;
    metrics.mpsSmooth = 0.0f;
    metrics.rpm = 0.0f;
    metrics.motorRPM = 0.0f;
    metrics.workoutDistance = 0.0f;

    metrics.targetSpeed = 0.0f;
    metrics.targetInclination = 0.0f;
    metrics.currentInclination = 0.0f;

    metrics.heartRateBpm = 0;
    metrics.rrLastMs = 0;
    metrics.heartRateEnergy = 0;
    metrics.heartRateLastUpdate = 0;
    metrics.rrBuffer.clear();
    metrics.hrBuffer.clear();

    metrics.isRunning = false;
    metrics.isPaused = false;
    metrics.controlRequested = false;

    metrics.sessionStartTime = now;
    metrics.workoutStartTime = now;
    metrics.lastUpdate = now;

    testdata = false;

    resetWorkoutTimer();
    resetMonitorViewState();

    Serial.println("Workout reset (global)");
}

