// ============================================================================
// Web UI & Inline Getters (documented units, UI notes)
// ============================================================================
/*
Enabling Async Mode
Please follow the steps based on your platform:
When using Arduino IDE, you will have to enable async mode by editing a line inside ElegantOTA library as Arduino IDE doesn’t currently provide us with any way to set build flags.
    Go to your Arduino libraries directory
    Open ElegantOTA folder and then open src folder
    Locate the ELEGANTOTA_USE_ASYNC_WEBSERVER macro in the ElegantOTA.h file, and set it to 1:
    #define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
    Save the changes to the ElegantOTA.h file.
    You are now ready to use ElegantOTA in async mode for your OTA updates, utilizing the ESPAsyncWebServer library.
*/

#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
// enable ElegantOTA async mode for ESPAsyncWebServer
#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#include <ElegantOTA.h>

// Declared elsewhere (usually in your .ino)
extern AsyncWebServer server;

// Web init
void initWebServer();

// Template processor for %PLACEHOLDERS%
String processTemplate(const String& var);

// Validation (signature matches usage in /api/settings)
bool validateSettings(const String& wifiSSID, const String& wifiPassword, const String& bleDeviceName,
    int interruptPin, int motorInterruptPin, int speedUpPin, int speedDownPin,
    int inclineUpPin, int inclineDownPin, uint32_t speedIncDecFreq,
    uint32_t testdataFreq, long beltDistance, long debounceThreshold,
    long maxRevolutionTime, long pulsesPerRev, long motorPulsesPerRev, float motorToBeltRatio);

// Simple JSON getter used in /api/settings
String extractJsonValue(const String& json, const String& key);

// Chart JSON builders (src/web/web_chart_builders.cpp)
String buildHeartRateSeriesJson();
String buildRRSeriesJson();

// Exposed getters used by processTemplate()
String getSpeed();
String getPaceMin();
String getPaceSec();
String getCpuUsage();
String getMotorRPM();
String getDistance();
String getDistanceUnit();
String getHour();
String getMinute();
String getSecond();
String getOffset();
String getRPM();
String getTotalRevs();
String getMotorTotalRevs();
String getHeartRate();
String getRR();
String getDateTime();
String getTestDataButtonText();
void resetWorkoutTimer();
void resetMonitorViewState();