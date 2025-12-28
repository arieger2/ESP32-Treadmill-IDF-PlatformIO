/**
 * @file web_monitor_main_ts.h
 * @brief TypeScript-powered Monitor (Main) page
 */

#pragma once
#ifndef ESP32_TREADMILL_TACHO_UI_MAIN_TS_H
#define ESP32_TREADMILL_TACHO_UI_MAIN_TS_H

#include <Arduino.h>
#include "ESP32_treadmill_tacho_web.h"
#include "../build/monitor-app.min.js.h"

String processTemplate(const String& var);

namespace Ui { namespace Main {

static const char MONITOR_HTML_TS[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>ESP32 Laufband Monitor</title>
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.0.0/css/all.min.css">
  <style>
    * { margin:0; padding:0; box-sizing:border-box; }
    body { font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif; background:linear-gradient(135deg,#667eea,#764ba2); color:#fff; padding:10px; padding-top:60px; }
    .nav { background:rgba(102,126,234,0.95); backdrop-filter:blur(10px); border-radius:0 0 10px 10px; padding:10px; display:flex; flex-wrap:wrap; gap:8px; justify-content:center; position:fixed; top:0; left:0; right:0; z-index:1000; box-shadow:0 2px 10px rgba(0,0,0,0.3); }
    .nav a { color:#fff; text-decoration:none; padding:10px 15px; border-radius:5px; background:rgba(255,255,255,0.1); transition:all 0.3s; white-space:nowrap; }
    .nav a:hover { background:rgba(255,255,255,0.25); transform:translateY(-2px); }
    .container { max-width:900px; margin:0 auto; }
    .card { background:rgba(255,255,255,0.15); backdrop-filter:blur(10px); border-radius:15px; padding:20px; margin-bottom:20px; box-shadow:0 8px 32px rgba(0,0,0,0.2); clear:both; }
    h2 { margin-bottom:15px; font-size:1.8rem; text-shadow:2px 2px 4px rgba(0,0,0,0.3); }
    .metric-grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(150px,1fr)); gap:15px; margin-bottom:20px; }
    .metric { background:rgba(255,255,255,0.1); border-radius:10px; padding:15px; text-align:center; min-height:80px; }
    .metric-label { font-size:0.9rem; opacity:0.9; margin-bottom:5px; }
    .metric-value { font-size:2rem; font-weight:bold; text-shadow:2px 2px 4px rgba(0,0,0,0.3); }
    .speed-row { display:flex; gap:10px; justify-content:space-between; align-items:stretch; margin-bottom:15px; }
    .speed-row .metric { flex:1; }
    .chart-box { margin-top:15px; padding:15px; background:rgba(255,255,255,0.1); border-radius:10px; display:block !important; visibility:visible !important; min-height:150px; }
    .chart-container { display:flex; justify-content:center; align-items:center; margin-top:10px; width:100%; height:120px; }
    .chart-container svg { width:100%% !important; height:120px !important; display:block; }
    .duration-row { display:flex; gap:10px; justify-content:space-between; align-items:stretch; }
    .duration-left, .duration-right { flex:1; display:flex; gap:10px; }
    .button { display:inline-block; padding:12px 20px; background:rgba(255,255,255,0.25); color:#fff; text-decoration:none; border-radius:8px; font-weight:bold; transition:all 0.3s; cursor:pointer; border:none; font-size:1rem; }
    .button:hover { background:rgba(255,255,255,0.35); transform:translateY(-2px); box-shadow:0 4px 12px rgba(0,0,0,0.2); }
    .button:active { transform:translateY(0); }
    .button.danger { background:rgba(220,53,69,0.3); }
    .button.danger:hover { background:rgba(220,53,69,0.5); }
    .button-red { background:rgba(220,53,69,0.5); }
    .button-red:hover { background:rgba(220,53,69,0.7); }
    .button-green { background:rgba(40,167,69,0.3); }
    .button-green:hover { background:rgba(40,167,69,0.5); }
    @media (max-width: 768px) {
      h2 { font-size:1.5rem; }
      .metric-value { font-size:1.5rem; }
      .card { padding:15px; }
      .chart-box { padding:10px; }
      .chart-container { height:100px; }
      .chart-container svg { width:100%% !important; height:100px !important; }
      .duration-row { flex-direction:column; gap:0.6rem; }
    }
    @media (max-width: 600px) {
      .nav { padding:8px; }
      .nav a { display:inline-block; margin:4px 3px; padding:8px 12px; font-size:14px; }
      p .button { display:inline-block; width:calc(50%% - 10px); margin:6px 5px; }
    }
    @media (max-width: 380px) {
      p .button { display:block; width:100%%; margin:8px 0; }
    }
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
      <h2><i class="fas fa-running"></i> Workout</h2>
      <div class="speed-row">
        <div class="metric">
          <div class="metric-label">Geschwindigkeit</div>
          <div class="metric-value"><span id="speed">0.0</span> <span class="units">km/h</span></div>
        </div>
        <div class="metric">
          <div class="metric-label">Tempo (min/km)</div>
          <div class="metric-value"><span id="pacemin">0</span>:<span id="pacesec">00</span></div>
        </div>
        <div class="metric">
          <div class="metric-label">Distanz</div>
          <div class="metric-value"><span id="distance">0.00</span> <span id="distanceunit">km</span></div>
        </div>
      </div>
      <div class="duration-row">
        <div class="duration-left">
          <div class="metric">
            <div class="metric-label">Dauer</div>
            <div class="metric-value"><span id="hour">00</span>:<span id="minute">00</span>:<span id="second">00</span></div>
          </div>
        </div>
        <div class="duration-right">
          <div class="metric">
            <div class="metric-label">Zeit</div>
            <div class="metric-value"><span id="datetime">--:--:--</span></div>
          </div>
        </div>
      </div>
    </div>

    <div class="card">
      <h2><i class="fas fa-heartbeat"></i> Herzfrequenz</h2>
      <div class="metric-grid">
        <div class="metric">
          <div class="metric-label">Herzfrequenz</div>
          <div class="metric-value"><span id="heartrate">--</span> bpm</div>
        </div>
        <div class="metric">
          <div class="metric-label">RR Intervall</div>
          <div class="metric-value"><span id="rr">--</span> ms</div>
        </div>
      </div>
      <div class="chart-box">
        <div class="metric-label">Herzfrequenz Verlauf</div>
        <div class="chart-container">
          <svg id="hr-chart" width="100%" height="120"></svg>
        </div>
      </div>
      <div class="chart-box">
        <div class="metric-label">RR Intervall Verlauf</div>
        <div class="chart-container">
          <svg id="rr-chart" width="100%" height="120"></svg>
        </div>
      </div>
    </div>

    <div class="card">
      <h2><i class="fas fa-chart-line"></i> Workout Status</h2>
      <div class="metric">
        <div class="metric-label">Offset</div>
        <div class="metric-value"><span id="offset">0.0</span> km/h</div>
      </div>
      <p style="margin-top:15px;">
        <a href="/offset/up" class="button"><i class="fas fa-arrow-up"></i> Offset +</a>
        <a href="/offset/down" class="button"><i class="fas fa-arrow-down"></i> Offset -</a>
        <a href="/reset" class="button danger" onclick="return confirm('Workout wirklich zurücksetzen?');"><i class="fas fa-redo"></i> Reset</a>
      </p>
    </div>

    <div class="card">
      <h2><i class="fas fa-cog"></i> System</h2>
      <div class="metric">
        <div class="metric-label">CPU Nutzung</div>
        <div class="metric-value"><span id="cpuusage">0</span>%</div>
      </div>
      <div class="metric" style="margin-top:15px;">
        <div class="metric-label">Motor RPM</div>
        <div class="metric-value"><span id="motorrpm">0</span></div>
      </div>
      <p style="margin-top:15px;">
        <button id="testdata-btn" class="button" onclick="toggleTestMode(); return false;"><i class="fas fa-vial"></i> <span id="testdata-text">Test Mode</span></button>
      </p>
    </div>
  </div>

  <script>
%MONITOR_TS_BUNDLE%
</script>
</body>
</html>
)rawliteral";

inline void addRoutes(AsyncWebServer& server) {
  // Serve JavaScript separately (for caching)
  server.on("/monitor.js", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send(200, "application/javascript", (const char*)MONITOR_TS_BUNDLE);
  });
  
  // Serve HTML page with TRUE chunked response - no buffering!
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
    const char* html = MONITOR_HTML_TS;
    const char* js = (const char*)MONITOR_TS_BUNDLE;
    const char* placeholder = strstr(html, "%MONITOR_TS_BUNDLE%");
    
    if (!placeholder) {
      req->send(200, "text/html", html);
      return;
    }
    
    size_t htmlBeforeJs = placeholder - html;
    size_t jsLen = strlen(js);
    size_t htmlAfterJsLen = strlen(placeholder + 19); // 19 = strlen("%MONITOR_TS_BUNDLE%")
    
    AsyncWebServerResponse *response = req->beginChunkedResponse("text/html", 
      [html, js, htmlBeforeJs, jsLen, htmlAfterJsLen, placeholder](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        
        size_t totalLen = htmlBeforeJs + jsLen + htmlAfterJsLen;
        if (index >= totalLen) return 0;
        
        size_t toWrite = 0;
        
        if (index < htmlBeforeJs) {
          toWrite = min(maxLen, htmlBeforeJs - index);
          memcpy(buffer, html + index, toWrite);
        } 
        else if (index < htmlBeforeJs + jsLen) {
          size_t jsIndex = index - htmlBeforeJs;
          toWrite = min(maxLen, jsLen - jsIndex);
          memcpy(buffer, js + jsIndex, toWrite);
        } 
        else {
          size_t afterIndex = index - htmlBeforeJs - jsLen;
          const char* afterJs = placeholder + 19;
          toWrite = min(maxLen, htmlAfterJsLen - afterIndex);
          memcpy(buffer, afterJs + afterIndex, toWrite);
        }
        
        return toWrite;
      });
    
    response->addHeader("Cache-Control", "no-cache");
    req->send(response);
  });
}

}} // namespace Ui::Main

#endif
