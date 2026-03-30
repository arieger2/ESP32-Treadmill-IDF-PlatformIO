/**
 * @file web_settings_main_ts.h
 * @brief TypeScript-powered Settings page
 */

#pragma once
#ifndef WEB_SETTINGS_MAIN_TS_H
#define WEB_SETTINGS_MAIN_TS_H

#include <Arduino.h>
#include "ESP32_treadmill_tacho_web.h"
#include "../build/settings-app.min.js.h"

String processTemplate(const String& var);

namespace Ui { namespace Settings {

static const char SETTINGS_HTML_TS[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Laufband - Settings</title>
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.0.0/css/all.min.css">
  <style>
    * { margin:0; padding:0; box-sizing:border-box; }
    body { font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif; background:linear-gradient(135deg,#667eea,#764ba2); color:#fff; min-height:100vh; padding:10px; padding-top:60px; }
    .nav { background:rgba(102,126,234,0.95); backdrop-filter:blur(10px); border-radius:0 0 10px 10px; padding:10px; display:flex; flex-wrap:wrap; gap:8px; justify-content:center; position:fixed; top:0; left:0; right:0; z-index:1000; box-shadow:0 2px 10px rgba(0,0,0,0.3); }
    .nav a { color:#fff; text-decoration:none; padding:10px 15px; border-radius:5px; background:rgba(255,255,255,0.1); transition:all 0.3s; white-space:nowrap; }
    .nav a:hover { background:rgba(255,255,255,0.25); transform:translateY(-2px); }
    .container { max-width:900px; margin:0 auto; }
    .card { background:rgba(255,255,255,0.15); backdrop-filter:blur(10px); border-radius:15px; padding:20px; margin-bottom:20px; box-shadow:0 8px 32px rgba(0,0,0,0.2); }
    h2 { margin-bottom:15px; font-size:1.8rem; text-shadow:2px 2px 4px rgba(0,0,0,0.3); }
    h3 { margin:20px 0 10px 0; font-size:1.3rem; border-bottom:2px solid rgba(255,255,255,0.3); padding-bottom:5px; }
    .section { margin-bottom:25px; }
    .form-group { margin-bottom:15px; display:flex; flex-direction:column; gap:5px; }
    label { font-weight:bold; font-size:0.95rem; }
    input,select { padding:10px; border:none; border-radius:8px; background:rgba(255,255,255,0.9); font-size:1rem; }
    .unit { font-size:0.85rem; opacity:0.8; }
    .help-text { font-size:0.85rem; opacity:0.9; padding:8px; background:rgba(255,255,255,0.1); border-radius:5px; margin-top:10px; }
    .button { padding:12px 20px; background:rgba(255,255,255,0.25); color:#fff; border:none; border-radius:8px; font-weight:bold; cursor:pointer; transition:all 0.3s; font-size:1rem; margin:5px; }
    .button:hover { background:rgba(255,255,255,0.35); transform:translateY(-2px); }
    .button-save { background:rgba(46,204,113,0.4); }
    .button-save:hover { background:rgba(46,204,113,0.6); }
    .button-danger { background:rgba(231,76,60,0.4); }
    .button-danger:hover { background:rgba(231,76,60,0.6); }
    .button-secondary { background:rgba(52,152,219,0.4); }
    .button-secondary:hover { background:rgba(52,152,219,0.6); }
    .status { padding:10px; border-radius:8px; margin:10px 0; font-weight:bold; display:none; }
    .status.success { background:rgba(46,204,113,0.3); display:block; }
    .status.error { background:rgba(231,76,60,0.3); display:block; }
    .settings-actions { text-align:center; }
    @media (max-width: 768px) { h2 { font-size:1.5rem; } .card { padding:15px; } }
  </style>
</head>
<body>
  <div class="nav">
    <a href="/"><i class="fas fa-tachometer-alt"></i> Monitor</a>
    <a href="/workout"><i class="fas fa-running"></i> Workout</a>
    <a href="/settings"><i class="fas fa-cogs"></i> Settings</a>
  </div>

  <div class="container">
    <div class="card">
      <h2><i class="fas fa-cogs"></i> Settings</h2>
      
      <form id="settingsForm">
        <!-- WiFi Section -->
        <div class="section">
          <h3><i class="fas fa-wifi"></i> WiFi Configuration</h3>
          <div class="form-group">
            <label>WiFi SSID:</label>
            <input type="text" id="wifiSSID" name="wifiSSID" value="%WIFI_SSID%" maxlength="32">
          </div>
          <div class="form-group">
            <label>WiFi Password:</label>
            <input type="password" id="wifiPassword" name="wifiPassword" value="%WIFI_PASSWORD%" maxlength="64">
          </div>
        </div>

        <!-- BLE Section -->
        <div class="section">
          <h3><i class="fas fa-bluetooth"></i> Bluetooth Configuration</h3>
          <div class="form-group">
            <label>BLE Device Name:</label>
            <input type="text" id="bleDeviceName" name="bleDeviceName" value="%BLE_DEVICE_NAME%" maxlength="30">
          </div>
        </div>

        <!-- GPIO Section -->
        <div class="section">
          <h3><i class="fas fa-microchip"></i> GPIO Pin Configuration</h3>
          <div class="form-group">
            <label>Band Sensor Pin:</label>
            <input type="number" id="interruptPin" name="interruptPin" value="%INTERRUPT_PIN%" min="0" max="39">
          </div>
          <div class="form-group">
            <label>Motor Sensor Pin:</label>
            <input type="number" id="motorInterruptPin" name="motorInterruptPin" value="%MOTOR_INTERRUPT_PIN%" min="0" max="39">
          </div>
          <div class="form-group">
            <label>Speed Up Pin:</label>
            <input type="number" id="speedUpPin" name="speedUpPin" value="%SPEED_UP_PIN%" min="0" max="39">
          </div>
          <div class="form-group">
            <label>Speed Down Pin:</label>
            <input type="number" id="speedDownPin" name="speedDownPin" value="%SPEED_DOWN_PIN%" min="0" max="39">
          </div>
          <div class="form-group">
            <label>Incline Up Pin:</label>
            <input type="number" id="inclineUpPin" name="inclineUpPin" value="%INCLINE_UP_PIN%" min="0" max="39">
          </div>
          <div class="form-group">
            <label>Incline Down Pin:</label>
            <input type="number" id="inclineDownPin" name="inclineDownPin" value="%INCLINE_DOWN_PIN%" min="0" max="39">
          </div>
        </div>

        <!-- Timing Section -->
        <div class="section">
          <h3><i class="fas fa-clock"></i> Timing Configuration</h3>
          <div class="form-group">
            <label>Test Data Frequency:</label>
            <input type="number" id="testdataFreq" name="testdataFreq" value="%TESTDATA_FREQ%" min="1" max="1000">
            <span class="unit">milliseconds</span>
          </div>
        </div>

        <!-- Sensor Configuration -->
        <div class="section">
          <h3><i class="fas fa-tachometer-alt"></i> Sensor Configuration</h3>
          <div class="form-group">
            <label>Belt Distance:</label>
            <input type="number" id="beltDistance" name="beltDistance" value="%BELT_DISTANCE%" min="100" max="500">
            <span class="unit">mm per revolution</span>
          </div>
          <div class="form-group">
            <label>Debounce Threshold:</label>
            <input type="number" id="debounceThreshold" name="debounceThreshold" value="%DEBOUNCE_THRESHOLD%" min="1" max="13">
            <span class="unit">microseconds</span>
          </div>
          <div class="form-group">
            <label>Max Revolution Time:</label>
            <input type="number" id="maxRevolutionTime" name="maxRevolutionTime" value="%MAX_REVOLUTION_TIME%" min="100" max="500000">
            <span class="unit">milliseconds</span>
          </div>
          <div class="form-group">
            <label>Band Pulses per Revolution:</label>
            <input type="number" id="pulsesPerRev" name="pulsesPerRev" value="%PULSES_PER_REV%" min="1" max="128">
          </div>
          <div class="form-group">
            <label>Band max Pulses for measurement (Band Pulses × value):</label>
            <input type="number" id="bandPulseMultiplier" name="bandPulseMultiplier" value="%BAND_PULSE_MULTIPLIER%" min="1" max="100">
            <span class="unit">multiplier (1-100)</span>
          </div>
          <div class="form-group">
            <label>Motor Pulses per Revolution:</label>
            <input type="number" id="motorPulsesPerRev" name="motorPulsesPerRev" value="%MOTOR_PULSES_PER_REV%" min="1" max="256">
          </div>
          <div class="form-group">
            <label>Motor max Pulses for measurement (Motor Pulses × value):</label>
            <input type="number" id="motorPulseMultiplier" name="motorPulseMultiplier" value="%MOTOR_PULSE_MULTIPLIER%" min="1" max="100">
            <span class="unit">multiplier (1-100)</span>
          </div>
          <div class="form-group">
            <label>Motor to Belt Ratio:</label>
            <input type="number" id="motorToBeltRatio" name="motorToBeltRatio" step="0.000001" value="%MOTOR_TO_BELT_RATIO%" min="0.01" max="10.0">
          </div>
          <div class="form-group">
            <label>Sensor Source Mode:</label>
            <select id="sensorSourceMode" name="sensorSourceMode" data-value="%SENSOR_SOURCE_MODE%">
              <option value="0">Hybrid (Automatic)</option>
              <option value="1">Band Sensor</option>
              <option value="2">Motor Sensor</option>
            </select>
          </div>
        </div>

        <!-- Filter Configuration -->
        <div class="section">
          <h3><i class="fas fa-filter"></i> Filter Configuration</h3>
          <div class="form-group">
            <label>Band Sensor Filter:</label>
            <select id="bandFilterType" name="bandFilterType" data-value="%BAND_FILTER_TYPE%">
              <option value="0">None (Raw Data)</option>
              <option value="1">EMA (Exponential Moving Avg)</option>
              <option value="2">Kalman (Optimal)</option>
              <option value="3">Median (Noise Rejection)</option>
            </select>
          </div>
          <div class="form-group">
            <label>Motor Sensor Filter:</label>
            <select id="motorFilterType" name="motorFilterType" data-value="%MOTOR_FILTER_TYPE%">
              <option value="0">None (Raw Data)</option>
              <option value="1">EMA (Exponential Moving Avg)</option>
              <option value="2">Kalman (Optimal)</option>
              <option value="3">Median (Noise Rejection)</option>
            </select>
          </div>
        </div>

        <!-- Speed Control Calibration -->
        <div class="section">
          <h3><i class="fas fa-tachometer-alt"></i> Speed Control Calibration</h3>
          <div class="form-group">
            <label>Inertia Delay:</label>
            <input type="number" id="inertiaDelay" name="inertiaDelay" step="50" min="100" max="5000" value="%INERTIA_DELAY%">
            <span class="unit">milliseconds</span>
          </div>
          <div class="help-text">
            <i class="fas fa-info-circle"></i> Auto-calibration: Start treadmill at low speed, then click "Start Calibration".
            System will ramp up to 15 km/h measuring acceleration rates at different speed ranges.
          </div>
          <button type="button" class="button button-secondary" onclick="SettingsApp.startCalibration()">
            <i class="fas fa-play-circle"></i> Start Calibration
          </button>
          <div id="calibrationStatus" class="status"></div>
        </div>

        <!-- PIDAJ Controller Tuning -->
        <div class="section">
          <h3><i class="fas fa-sliders-h"></i> PIDAJ Controller Tuning</h3>
          <div class="form-group">
            <label>Kp (Proportional):</label>
            <input type="number" id="pidKp" name="pidKp" step="0.1" min="0.1" max="10.0" value="%PID_KP%">
          </div>
          <div class="form-group">
            <label>Ki (Integral):</label>
            <input type="number" id="pidKi" name="pidKi" step="0.01" min="0.0" max="2.0" value="%PID_KI%">
          </div>
          <div class="form-group">
            <label>Ka (Acceleration damping):</label>
            <input type="number" id="pidKa" name="pidKa" step="0.1" min="0.0" max="10.0" value="%PID_KA%">
          </div>
          <div class="form-group">
            <label>Kj (Jerk prediction):</label>
            <input type="number" id="pidKj" name="pidKj" step="0.1" min="0.0" max="5.0" value="%PID_KJ%">
          </div>
          <div class="form-group">
            <label>Dead Zone:</label>
            <input type="number" id="pidDeadZone" name="pidDeadZone" step="0.01" min="0.05" max="1.0" value="%PID_DEAD_ZONE%">
            <span class="unit">km/h</span>
          </div>
          <div class="form-group">
            <label>Long Press Threshold:</label>
            <input type="number" id="pidLongPressThresh" name="pidLongPressThresh" step="0.1" min="0.3" max="5.0" value="%PID_LONG_PRESS_THRESH%">
            <span class="unit">km/h</span>
          </div>
          <div class="form-group">
            <label>Integral Clamp:</label>
            <input type="number" id="pidIClamp" name="pidIClamp" step="0.5" min="0.5" max="20.0" value="%PID_I_CLAMP%">
          </div>
          <div class="form-group">
            <label>Pulse Cooldown:</label>
            <input type="number" id="pidPulseCooldown" name="pidPulseCooldown" step="50" min="100" max="2000" value="%PID_PULSE_COOLDOWN%">
            <span class="unit">ms</span>
          </div>
          <div class="form-group">
            <label>Long Press Max:</label>
            <input type="number" id="pidLongPressMax" name="pidLongPressMax" step="1000" min="3000" max="30000" value="%PID_LONG_PRESS_MAX%">
            <span class="unit">ms</span>
          </div>
          <div class="form-group">
            <label>Coast Threshold:</label>
            <input type="number" id="pidCoastThreshold" name="pidCoastThreshold" step="0.005" min="0.01" max="0.2" value="%PID_COAST_THRESHOLD%">
            <span class="unit">m/s²</span>
          </div>
          <div class="help-text">
            <i class="fas fa-info-circle"></i> P=error, I=drift correction, A=acceleration damping, J=jerk prediction.
            Large error (&gt;Long Press Threshold) uses long press, small error uses single pulses with cooldown.
          </div>
        </div>

        <!-- Actions -->
        <div class="section settings-actions">
          <button type="submit" class="button button-save"><i class="fas fa-save"></i> Save Settings</button>
          <div id="status" class="status"></div>
          <button type="button" class="button button-secondary" onclick="SettingsApp.loadDefaults()"><i class="fas fa-undo"></i> Load Defaults</button>
          <button type="button" class="button button-danger" onclick="SettingsApp.factoryReset()"><i class="fas fa-trash"></i> Factory Reset</button>
          <button type="button" class="button button-secondary" onclick="SettingsApp.rebootDevice()"><i class="fas fa-power-off"></i> Reboot Device</button>
          <button type="button" class="button button-secondary" onclick="SettingsApp.goToOTA()"><i class="fas fa-upload"></i> Upload Firmware</button>
          <button type="button" class="button button-info" onclick="SettingsApp.viewBootLog()" style="background:#17a2b8;border-color:#17a2b8;"><i class="fas fa-file-alt"></i> View Boot Log</button>
        </div>
      </form>
    </div>
  </div>

  <script src="/settings.js"></script>
</body>
</html>
)rawliteral";

inline void addRoutes(AsyncWebServer& server) {
  // Serve JavaScript separately (for caching)
  server.on("/settings.js", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send(200, "application/javascript", (const char*)SETTINGS_TS_BUNDLE);
  });
  
  // Serve HTML page with template processor for value placeholders
  // Settings HTML is small enough (~6KB without JS) to use template processor safely
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send(200, "text/html", SETTINGS_HTML_TS, processTemplate);
  });
}

}} // namespace Ui::Settings

#endif
