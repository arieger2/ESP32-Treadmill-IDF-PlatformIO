#include "HardwareSerial.h"
#include <ArduinoJson.h>
#include <TimeLib.h>
#include <new>
#include "esp_netif.h"

#include "ESP32_treadmill_tacho_web.h"
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_workout.h"
#include "ESP32_treadmill_tacho_bootlog.h"
#include "web_monitor/web_monitor_main_ts.h"
#include "web_settings/web_settings_main_ts.h"
#include "web_workout/web_workout_main_ts.h"
#include "ESP32_treadmill_tacho_ui_assets.h"

extern WorkoutExecutor gWorkout;
// extern SpeedFilter bandFilter;  // OLD SYSTEM REMOVED
// extern SpeedFilter motorFilter; // OLD SYSTEM REMOVED

// ============================================================================
// Web Server Initialization and Route Definitions
// Business logic is organized into modular folders:
//  - src/web_monitor/: Monitor page API and visualization
//    - web_api_monitor.cpp: Monitor value getters (getSpeed, getPace, etc.)
//    - web_chart_builders.cpp: HR/RR chart JSON generation
//    - web_validation.cpp: Settings validation logic
//    - web_template.cpp: HTML template variable substitution
//  - src/web_workout/: Workout page components
//    - web_workout_input.cpp: File upload section
//    - web_workout_control.cpp: Control buttons section
//    - web_workout_view.cpp: Visualization section
//    - web_workout_test.cpp: Manual test controls section
//    - web_workout_common.cpp: Shared utilities
//    - web_workout_main.cpp: Page assembly
// ============================================================================

void initWebServer() {
  DefaultHeaders::Instance().addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  DefaultHeaders::Instance().addHeader("Pragma", "no-cache");
  DefaultHeaders::Instance().addHeader("Expires", "0");
  DefaultHeaders::Instance().addHeader("Keep-Alive", "timeout=2, max=10");

  // Split UI pages
  Ui::Main::addRoutes(server);
  Ui::Settings::addRoutes(server);
  Ui::Workout::addRoutes(server);
  Ui::Assets::addRoutes(server);

  // ===== API: Combined monitor endpoint (single request for all values) =====
  server.on("/api/monitor", HTTP_GET, [](AsyncWebServerRequest *r){
    // Build JSON with all monitor values in one response
    String json = "{";
    json += "\"speed\":\"" + getSpeed() + "\",";
    json += "\"pacemin\":\"" + getPaceMin() + "\",";
    json += "\"pacesec\":\"" + getPaceSec() + "\",";
    json += "\"distance\":\"" + getDistance() + "\",";
    json += "\"distanceunit\":\"" + getDistanceUnit() + "\",";
    json += "\"hour\":\"" + getHour() + "\",";
    json += "\"minute\":\"" + getMinute() + "\",";
    json += "\"second\":\"" + getSecond() + "\",";
    json += "\"offset\":\"" + getOffset() + "\",";
    json += "\"rpm\":\"" + getRPM() + "\",";
    json += "\"motorrpm\":\"" + getMotorRPM() + "\",";
    json += "\"heartrate\":\"" + getHeartRate() + "\",";
    json += "\"rr\":\"" + getRR() + "\",";
    json += "\"datetime\":\"" + getDateTime() + "\",";
    json += "\"testdata\":" + String(testdata ? "true" : "false") + ",";
    json += "\"signalquality\":\"" + getSignalQuality() + "\",";
    json += "\"signalcv\":\"" + getSignalCV() + "\",";
    json += "\"signalfreq\":\"" + getSignalFrequency() + "\",";
    json += "\"signalclass\":\"" + getSignalQualityClass() + "\"";
    json += "}";
    r->send(200, "application/json", json);
  });

  // ===== API: Individual endpoints (legacy, still useful) =====
  server.on("/api/speed",      HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getSpeed()); });
  server.on("/api/pacesec",    HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getPaceSec()); });
  server.on("/api/pacemin",    HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getPaceMin()); });
  server.on("/api/motorrpm",   HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getMotorRPM()); });
  server.on("/api/distance",   HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getDistance()); });
  server.on("/api/distanceunit",HTTP_GET,[](AsyncWebServerRequest *r){ r->send(200,"text/plain", getDistanceUnit()); });
  server.on("/api/rpm",        HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getRPM()); });
  server.on("/api/hour",       HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getHour()); });
  server.on("/api/minute",     HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getMinute()); });
  server.on("/api/second",     HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getSecond()); });
  server.on("/api/offset",     HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getOffset()); });
  server.on("/api/heartrate",  HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getHeartRate()); });
  server.on("/api/rr",         HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getRR()); });
  server.on("/api/hr-series",  HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"application/json", buildHeartRateSeriesJson()); });
  server.on("/api/rr-series",  HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"application/json", buildRRSeriesJson()); });
  server.on("/api/datetime",   HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200,"text/plain", getDateTime()); });
  
  // ===== Boot Log API =====
  server.on("/api/bootlog", HTTP_GET, [](AsyncWebServerRequest *r){
    String log = readBootLog();
    r->send(200, "text/plain", log);
  });

  // ===== Buttons (monitor page) =====
  server.on("/offset/up", HTTP_GET, [](AsyncWebServerRequest *req) {
    metrics.mpsOffset += 0.139f;
    req->redirect("/");
  });
  server.on("/offset/down", HTTP_GET, [](AsyncWebServerRequest *req) {
    metrics.mpsOffset = metrics.mpsOffset >= 0.139f ? (metrics.mpsOffset - 0.139f) : 0.0f;
    req->redirect("/");
  });
  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *req) {
    resetWorkout();
    Serial.println("Workout reset via web interface");
    req->redirect("/");
  });
  server.on("/testdata", HTTP_GET, [](AsyncWebServerRequest *req) {
    enableTestdata(!testdata);
    req->redirect("/");
  });

  // ===== Manual Test Controls (Workout Page) =====
  server.on("/api/test/speed/up", HTTP_POST, [](AsyncWebServerRequest *req) {
    writePress(storedGlobals.SPEED_UP_PIN, true);
    req->send(200, "text/plain", "OK");
  });
  server.on("/api/test/speed/down", HTTP_POST, [](AsyncWebServerRequest *req) {
    writePress(storedGlobals.SPEED_DOWN_PIN, true);
    req->send(200, "text/plain", "OK");
  });
  server.on("/api/test/incline/up", HTTP_POST, [](AsyncWebServerRequest *req) {
    writePress(storedGlobals.INCLINE_UP_PIN, true);
    req->send(200, "text/plain", "OK");
  });
  server.on("/api/test/incline/down", HTTP_POST, [](AsyncWebServerRequest *req) {
    writePress(storedGlobals.INCLINE_DOWN_PIN, true);
    req->send(200, "text/plain", "OK");
  });

  // ===== Settings API =====
  server.on("/api/settings", HTTP_POST,
    // finalize
    [](AsyncWebServerRequest *request) {
      String* acc = static_cast<String*>(request->_tempObject);
      request->_tempObject = nullptr;

      if (!acc || acc->isEmpty()) {
        if (acc) delete acc;
        request->send(400, "application/json", "{\"success\":false,\"message\":\"Empty body\"}");
        return;
      }

      String& jsonString = *acc;
      String wifiSSID         = extractJsonValue(jsonString, "wifiSSID");
      String wifiPassword     = extractJsonValue(jsonString, "wifiPassword");
      String bleDeviceName    = extractJsonValue(jsonString, "bleDeviceName");

      int    interruptPin     = extractJsonValue(jsonString, "interruptPin").toInt();
      int    motorInterruptPin= extractJsonValue(jsonString, "motorInterruptPin").toInt();
      int    speedUpPin       = extractJsonValue(jsonString, "speedUpPin").toInt();
      int    speedDownPin     = extractJsonValue(jsonString, "speedDownPin").toInt();
      int    inclineUpPin     = extractJsonValue(jsonString, "inclineUpPin").toInt();
      int    inclineDownPin   = extractJsonValue(jsonString, "inclineDownPin").toInt();

      uint32_t speedIncDecFreq= extractJsonValue(jsonString, "speedIncDecFreq").toInt();
      uint32_t testdataFreq   = extractJsonValue(jsonString, "testdataFreq").toInt();

      long   beltDistance     = extractJsonValue(jsonString, "beltDistance").toInt();
      long   debounceThreshold= extractJsonValue(jsonString, "debounceThreshold").toInt();
      long   maxRevolutionTime= extractJsonValue(jsonString, "maxRevolutionTime").toInt();
      long   pulsesPerRev     = extractJsonValue(jsonString, "pulsesPerRev").toInt();
      long   bandPulseMult    = extractJsonValue(jsonString, "bandPulseMultiplier").toInt();
      long   motorPulsesPerRev= extractJsonValue(jsonString, "motorPulsesPerRev").toInt();
      long   motorPulseMult   = extractJsonValue(jsonString, "motorPulseMultiplier").toInt();
      float  motorToBeltRatio = extractJsonValue(jsonString, "motorToBeltRatio").toFloat();

      int    sensorSourceMode = extractJsonValue(jsonString, "sensorSourceMode").toInt();
      int    bandFilterType   = extractJsonValue(jsonString, "bandFilterType").toInt();
      int    motorFilterType  = extractJsonValue(jsonString, "motorFilterType").toInt();

      float  speedUpRate      = extractJsonValue(jsonString, "speedUpRate").toFloat();
      float  speedDownRate    = extractJsonValue(jsonString, "speedDownRate").toFloat();
      uint32_t inertiaDelay   = extractJsonValue(jsonString, "inertiaDelay").toInt();
      float  overshootFactor  = extractJsonValue(jsonString, "overshootFactor").toFloat();

      if (!validateSettings(wifiSSID, wifiPassword, bleDeviceName, interruptPin, motorInterruptPin,
                            speedUpPin, speedDownPin, inclineUpPin, inclineDownPin,
                            speedIncDecFreq, testdataFreq, beltDistance, debounceThreshold,
                            maxRevolutionTime, pulsesPerRev, motorPulsesPerRev, motorToBeltRatio)) {
        delete acc;
        request->send(200, "application/json", "{\"success\":false,\"message\":\"Invalid settings\"}");
        return;
      }

      storedGlobals.WIFI_SSID     = wifiSSID;
      if (wifiPassword.length() > 0) storedGlobals.WIFI_PASSWORD = wifiPassword;
      storedGlobals.BLE_DEVICE_NAME          = bleDeviceName;
      storedGlobals.INTERRUPT_PIN            = interruptPin;
      storedGlobals.MOTOR_INTERRUPT_PIN      = motorInterruptPin;
      storedGlobals.SPEED_UP_PIN             = speedUpPin;
      storedGlobals.SPEED_DOWN_PIN           = speedDownPin;
      storedGlobals.INCLINE_UP_PIN           = inclineUpPin;
      storedGlobals.INCLINE_DOWN_PIN         = inclineDownPin;
      storedGlobals.SPEED_INC_DEC_FREQ_MS    = speedIncDecFreq;
      storedGlobals.TESTDATA_FREQ_MS         = testdataFreq;
      storedGlobals.BELT_DISTANCE_MM         = beltDistance;
      // Clamp debounce to hardware limits (1-13 µs)
      if (debounceThreshold < 1) debounceThreshold = 1;
      if (debounceThreshold > 13) debounceThreshold = 13;
      storedGlobals.DEBOUNCE_THRESHOLD_US    = debounceThreshold;
      storedGlobals.MAX_REVOLUTION_TIME_MS   = maxRevolutionTime;
      storedGlobals.PULSES_PER_REV           = pulsesPerRev;
      // Clamp pulse multipliers (1-100)
      bandPulseMult  = (bandPulseMult < 1 || bandPulseMult > 100) ? 1 : bandPulseMult;
      motorPulseMult = (motorPulseMult < 1 || motorPulseMult > 100) ? 1 : motorPulseMult;
      storedGlobals.BAND_PULSE_MULTIPLIER    = bandPulseMult;
      storedGlobals.MOTOR_PULSES_PER_REV     = motorPulsesPerRev;
      storedGlobals.MOTOR_PULSE_MULTIPLIER   = motorPulseMult;
      storedGlobals.MOTOR_TO_BELT_RATIO      = motorToBeltRatio;
      sensorSourceMode = (sensorSourceMode < 0 || sensorSourceMode > 2) ? 0 : sensorSourceMode;
      
      // Check if sensor source mode changed - if so, reset filters
      bool sensorModeChanged = (storedGlobals.SENSOR_SOURCE_MODE != (uint8_t)sensorSourceMode);
      
      storedGlobals.SENSOR_SOURCE_MODE       = (uint8_t)sensorSourceMode;
      
      // Clamp filter types to valid range (0-3)
      bandFilterType  = (bandFilterType < 0 || bandFilterType > 3) ? 1 : bandFilterType;
      motorFilterType = (motorFilterType < 0 || motorFilterType > 3) ? 1 : motorFilterType;
      
      storedGlobals.BAND_FILTER_TYPE  = (uint8_t)bandFilterType;
      storedGlobals.MOTOR_FILTER_TYPE = (uint8_t)motorFilterType;
      
      // Calibration parameters with validation
      Serial.printf("[WEB] Received calibration values: UP=%.3f, DOWN=%.3f\n", speedUpRate, speedDownRate);
      if (speedUpRate > 0.0f && speedUpRate < 3.0f) {
        storedGlobals.SPEED_UP_RATE = speedUpRate;
        Serial.printf("[WEB] SPEED_UP_RATE set to: %.3f\n", storedGlobals.SPEED_UP_RATE);
      } else {
        Serial.printf("[WEB] WARNING: speedUpRate %.3f out of range, keeping %.3f\n", 
                      speedUpRate, storedGlobals.SPEED_UP_RATE);
      }
      if (speedDownRate > 0.0f && speedDownRate < 3.0f) {
        storedGlobals.SPEED_DOWN_RATE = speedDownRate;
        Serial.printf("[WEB] SPEED_DOWN_RATE set to: %.3f\n", storedGlobals.SPEED_DOWN_RATE);
      } else {
        Serial.printf("[WEB] WARNING: speedDownRate %.3f out of range, keeping %.3f\n", 
                      speedDownRate, storedGlobals.SPEED_DOWN_RATE);
      }
      if (inertiaDelay >= 100 && inertiaDelay <= 10000) 
        storedGlobals.INERTIA_DELAY_MS = inertiaDelay;
      if (overshootFactor >= 1.0f && overshootFactor <= 2.0f) 
        storedGlobals.OVERSHOOT_FACTOR = overshootFactor;
      
      // Update filter settings immediately
      // OLD SYSTEM REMOVED: Filter configuration no longer used
      // bandFilter.setFilterType((SpeedFilterType)bandFilterType);
      // motorFilter.setFilterType((SpeedFilterType)motorFilterType);
      
      // Reset filters when switching sensor source to prevent stale data
      if (sensorModeChanged) {
        // OLD SYSTEM REMOVED: Filter reset no longer needed
        // bandFilter.reset();
        // motorFilter.reset();
        Serial.printf("[SENSOR] Mode changed (filters removed in NEW system)\r\n");
      }

      String details = saveSettings();
      delete acc;

      if (details.isEmpty()) {
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Settings saved successfully\"}");
      } else {
        request->send(200, "application/json", "{\"success\":false,\"message\":\"" + details + "\"}");
      }
      Serial.println("Settings updated via web interface");
    },
    // onUpload (unused)
    nullptr,
    // onBody: accumulate JSON
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      String* acc = static_cast<String*>(request->_tempObject);
      if (index == 0) {
        acc = new String();
        if (acc && total > 0 && total < 64*1024) acc->reserve(total);
        request->_tempObject = acc;
      }
      if (acc && len) acc->concat((const char*)data, len);
    }
  );

  server.on("/api/settings/defaults", HTTP_POST, [](AsyncWebServerRequest *req){
    loadDefaultSettings(); saveSettings();
    req->send(200,"application/json","{\"success\":true,\"message\":\"Default settings loaded\"}");
    Serial.println("Default settings loaded via web interface");
  });

  server.on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest *req){
    factoryReset(); loadDefaultSettings();
    req->send(200,"application/json","{\"success\":true,\"message\":\"Factory reset complete\"}");
    Serial.println("Factory reset performed via web interface");
    delay(1000); ESP.restart();
  });

  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *req){
    req->send(200,"application/json","{\"success\":true,\"message\":\"Rebooting device\"}");
    Serial.println("Reboot requested via web interface");
    delay(1000); ESP.restart();
  });

  // ===== Calibration API =====
  server.on("/api/calibrate/start", HTTP_POST, [](AsyncWebServerRequest *req){
    startSpeedCalibration();
    req->send(200,"application/json","{\"success\":true,\"message\":\"Calibration started\"}");
  });

  server.on("/api/calibrate/status", HTTP_GET, [](AsyncWebServerRequest *req){
    String status = getCalibrationStatus();
    req->send(200,"application/json", status);
  });

  // ===== Workout: state =====
  server.on("/api/workout/state", HTTP_GET, [](AsyncWebServerRequest* r){
    JsonDocument doc;
    WorkoutProgress pr = gWorkout.getProgress();
    const char* st =
      pr.state == WorkoutState::Idle     ? "Idle" :
      pr.state == WorkoutState::Running  ? "Running" :
      pr.state == WorkoutState::Paused   ? "Paused" :
      pr.state == WorkoutState::Finished ? "Finished" : "Error";

    doc["state"]            = st;
    doc["total_s"]          = pr.total_s;
    doc["elapsed_s"]        = pr.elapsed_s;
    doc["step_index"]       = pr.step_index;
    doc["step_elapsed_s"]   = pr.step_elapsed_s;
    doc["step_remaining_s"] = pr.step_remaining_s;
    doc["speed_kph"]        = pr.current_speed_kph * gWorkout.getSpeedScale();
    doc["incline_pct"]      = pr.current_incline_pct;
    doc["label"]            = pr.current_label;
    if (pr.error.length())  doc["error"] = pr.error;

    // Only include the full workout structure if explicitly requested
    if (r->hasParam("full") && r->getParam("full")->value() == "1") {
      JsonArray steps = doc["steps"].to<JsonArray>();
      for (const auto& s : gWorkout.steps()) {
        JsonObject o = steps.add<JsonObject>();
        o["d"] = s.duration_s;
        o["v"] = s.speed_kph * gWorkout.getSpeedScale();
        o["i"] = s.incline_pct;
        o["l"] = s.label;
      }
    }
    String out; serializeJson(doc, out);
    r->send(200, "application/json", out);
  });

  // ===== Workout: control =====
  server.on("/api/workout/control", HTTP_POST, [](AsyncWebServerRequest* req){
    if (!req->hasParam("action", true)) { req->send(400, "text/plain", "Missing action"); return; }
    String action = req->getParam("action", true)->value();
    if      (action == "start")  gWorkout.start();
    else if (action == "pause")  gWorkout.pause();
    else if (action == "resume") gWorkout.resume();
    else if (action == "stop")   gWorkout.stop();
    else { req->send(400, "text/plain", "Unknown action"); return; }
    req->send(200, "text/plain", "OK");
  });

  // ===== Workout: upload (.zwo) =====
  server.on("/api/workout/upload", HTTP_POST,
    // finalize
    [](AsyncWebServerRequest* request){
      Serial.println("[UPLOAD] finalize");
      String* acc = static_cast<String*>(request->_tempObject);
      request->_tempObject = nullptr;
      if (!acc || acc->length() == 0) {
        Serial.printf("[UPLOAD] empty body (contentLength=%d)\r\r\n", (int)request->contentLength());
        if (acc) delete acc;
        request->send(400, "text/plain", "Missing or empty body");
        return;
      }
      Serial.printf("[UPLOAD] received %u bytes\r\r\n", (unsigned)acc->length());
      bool ok = gWorkout.loadFromZwoString(*acc);
      delete acc;
      if (!ok) {
        auto p = gWorkout.getProgress();
        Serial.printf("[UPLOAD] parse error: %s\r\r\n", p.error.c_str());
        request->send(400, "text/plain", "ZWO parse error: " + p.error);
      } else {
        Serial.println("[UPLOAD] OK");
        request->send(200, "text/plain", "OK");
      }
    },
    // multipart chunks
    [](AsyncWebServerRequest* request, const String& filename, size_t index,
       uint8_t* data, size_t len, bool final)
    {
      if (index == 0) {
        Serial.printf("[UPLOAD] begin file='%s' total=%d\r\n", filename.c_str(), (int)request->contentLength());
        String* acc = new String();
        if (acc && request->contentLength() > 0 && request->contentLength() < 512*1024)
          acc->reserve(request->contentLength());
        request->_tempObject = acc;
      }
      String* acc = static_cast<String*>(request->_tempObject);
      if (acc && len) acc->concat((const char*)data, len);
      Serial.printf("[UPLOAD] chunk idx=%u len=%u acc=%u final=%d\r\n",
                    (unsigned)index, (unsigned)len, acc ? (unsigned)acc->length() : 0, final ? 1 : 0);
    },
    // onBody (unused; multipart goes to onUpload)
    nullptr
  );

  // ===== Workout: pace scaling (global) =====
  server.on("/api/workout/scale", HTTP_GET, [](AsyncWebServerRequest* r){
    String out = String(gWorkout.getSpeedScale(), 4);
    r->send(200, "text/plain", out);
  });

  server.on("/api/workout/scale/nudge", HTTP_POST, [](AsyncWebServerRequest* req){
    if (!req->hasParam("rel", true)) { req->send(400, "text/plain", "Missing rel"); return; }
    float rel = req->getParam("rel", true)->value().toFloat();
    float factor = 1.0f + rel;
    if (factor <= 0.0f) { req->send(400, "text/plain", "Bad factor"); return; }
    gWorkout.scaleAllSpeeds(factor);
    // Keep threshold in sync: threshold is s/km (inverse of speed), so divide by factor
    float curThr = gWorkout.getThreshold();
    if (curThr > 0.0f) gWorkout.setThreshold(curThr / factor);
    req->send(200, "text/plain", "OK");
  });

  // Threshold: GET returns current s/km; POST val=<s/km> rescales steps and stores new threshold
  server.on("/api/workout/threshold", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "text/plain", String(gWorkout.getThreshold(), 1));
  });

  server.on("/api/workout/threshold", HTTP_POST, [](AsyncWebServerRequest* req){
    if (!req->hasParam("val", true)) { req->send(400, "text/plain", "Missing val"); return; }
    float newThr = req->getParam("val", true)->value().toFloat();
    if (newThr <= 0.0f || newThr > 3600.0f) { req->send(400, "text/plain", "Bad value"); return; }
    float oldThr = gWorkout.getThreshold();
    if (oldThr > 0.0f && fabsf(newThr - oldThr) > 0.01f) {
      gWorkout.scaleAllSpeeds(oldThr / newThr); // rescale steps proportionally
    }
    gWorkout.setThreshold(newThr);
    req->send(200, "text/plain", "OK");
  });

  server.on("/api/workout/upload", HTTP_OPTIONS, [](AsyncWebServerRequest* r){ r->send(204); });

  // Favicon handler - simple 16x16 pixel gear icon
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* r){
    const uint8_t favicon_ico[] = {
      0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x10, 0x10, 0x10, 0x00, 0x01, 0x00, 0x04, 0x00, 0x28, 0x01,
      0x00, 0x00, 0x16, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x20, 0x00,
      0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x80, 0x80, 0x80, 0x00, 0xC0, 0xC0, 0xC0, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x11, 0x00, 0x01, 0x22, 0x21, 0x10,
      0x12, 0x22, 0x22, 0x21, 0x12, 0x11, 0x11, 0x21, 0x21, 0x11, 0x11, 0x12, 0x21, 0x22, 0x22, 0x12,
      0x12, 0x22, 0x22, 0x21, 0x01, 0x22, 0x22, 0x10, 0x00, 0x12, 0x21, 0x00, 0x00, 0x01, 0x10, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    AsyncWebServerResponse* response = r->beginResponse(200, "image/x-icon", favicon_ico, sizeof(favicon_ico));
    response->addHeader("Cache-Control", "public, max-age=31536000");
    r->send(response);
  });

  server.onNotFound([](AsyncWebServerRequest* r){
    Serial.printf("[404] %s %s\r\n", r->methodToString(), r->url().c_str());
    r->send(404);
  });

  ElegantOTA.begin(&server);
  ElegantOTA.setAuth("admin", "admin");
  server.begin();

  // Print server URLs
  if (wifi.apMode) {
    Serial.println("Web server: http://192.168.4.1");
    Serial.println("Settings page: http://192.168.4.1/settings");
  } else if (wifi.connected) {
    esp_netif_ip_info_t ip_info;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
      Serial.printf("Web server: http://" IPSTR "\r\n", IP2STR(&ip_info.ip));
      Serial.printf("Settings page: http://" IPSTR "/settings\r\n", IP2STR(&ip_info.ip));
    }
  } else {
    Serial.println("Web server started (no IP yet)");
  }
  testdata = false;
}
