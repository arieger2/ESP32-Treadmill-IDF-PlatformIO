/**
 * @file web_workout_main_ts.h
 * @brief TypeScript-generated Workout Page
 * 
 * This replaces the old JavaScript-based workout implementation with
 * a type-safe TypeScript version that is compiled and minified during build.
 */

#include <Arduino.h>
#include "ESP32_treadmill_tacho_web.h"

namespace Ui { namespace Workout {

// Include the generated TypeScript bundle
#include "../../build/workout-app.min.js.h"

/**
 * Complete Workout Page HTML
 */
static const char WORKOUT_HTML_TS[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Laufband - Workout</title>
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
    h3 { margin:0 0 15px 0; font-size:1.3rem; border-bottom:2px solid rgba(255,255,255,0.3); padding-bottom:5px; }
    .btn { border:none; border-radius:8px; padding:12px 18px; margin:5px; color:#fff; cursor:pointer; font-weight:bold; font-size:1rem; transition:all 0.3s; }
    .btn:hover { transform:translateY(-2px); }
    .btn.start { background:rgba(46,204,113,0.6); }
    .btn.start:hover { background:rgba(46,204,113,0.8); }
    .btn.pause { background:rgba(241,196,15,0.6); }
    .btn.pause:hover { background:rgba(241,196,15,0.8); }
    .btn.resume { background:rgba(52,152,219,0.6); }
    .btn.resume:hover { background:rgba(52,152,219,0.8); }
    .btn.stop { background:rgba(231,76,60,0.6); }
    .btn.stop:hover { background:rgba(231,76,60,0.8); }
    .btn.pace { background:rgba(255,255,255,0.2); }
    .btn.pace:hover { background:rgba(255,255,255,0.35); }
    .btn.test-btn { background:rgba(230,126,34,0.6); }
    .btn.test-btn:hover { background:rgba(230,126,34,0.8); }
    .btn.test-up { background:rgba(39,174,96,0.6); }
    .btn.test-up:hover { background:rgba(39,174,96,0.8); }
    .btn[disabled] { opacity:0.5; cursor:not-allowed; pointer-events:none; }
    #woCanvas { width:100%; height:280px; border-radius:10px; background:rgba(0,0,0,0.2); }
    .hint { font-size:0.85rem; opacity:0.8; margin-top:10px; }
    .file-btn { display:inline-block; padding:12px 20px; background:rgba(46,204,113,0.5); color:#fff; border-radius:8px; cursor:pointer; font-weight:bold; transition:all 0.3s; }
    .file-btn:hover { background:rgba(46,204,113,0.7); transform:translateY(-2px); }
    #uploadStatus { margin-left:15px; }
    #woState { padding:15px; background:rgba(0,0,0,0.2); border-radius:8px; margin-bottom:15px; font-size:0.95rem; }
    #errorMsg { padding:12px; background:rgba(231,76,60,0.3); border:1px solid rgba(231,76,60,0.5); border-radius:8px; margin-top:15px; display:none; }
    .control-buttons, .test-controls { text-align:center; }
    .pace-control { margin-top:15px; padding-top:15px; border-top:1px solid rgba(255,255,255,0.2); text-align:center; }
    #paceNow { display:inline-block; min-width:80px; font-weight:bold; font-size:1.1rem; }
    .status { padding:10px; border-radius:8px; margin:10px 0; font-weight:bold; display:none; }
    .status.success { background:rgba(46,204,113,0.3); display:block; }
    .status.error { background:rgba(231,76,60,0.3); display:block; }
    @media (max-width:768px) { h2 { font-size:1.5rem; } .card { padding:15px; } .btn { padding:10px 14px; } }
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
    </div>

    <!-- Workout View Section - FIRST so user sees the chart -->
    <section id="workoutViewSection" class="card">
      <h3><i class="fas fa-chart-line"></i> Workout View - Progress & Status</h3>
      <div id="woState">No workout loaded. Upload a .ZWO file below.</div>
      <canvas id="woCanvas" width="860" height="280"></canvas>
      <div class="hint">Top: Speed (km/h), Bottom: Incline (%). Red bar = current position. Drag & drop .zwo file onto chart.</div>
    </section>

    <!-- Control Section -->
    <section id="controlSection" class="card">
      <h3><i class="fas fa-gamepad"></i> Control - Workout Execution</h3>
      <div class="control-buttons">
        <button class="btn start" id="btnStart"><i class="fas fa-play"></i> Start</button>
        <button class="btn pause" id="btnPause"><i class="fas fa-pause"></i> Pause</button>
        <button class="btn resume" id="btnResume"><i class="fas fa-forward"></i> Resume</button>
        <button class="btn stop" id="btnStop"><i class="fas fa-stop"></i> Stop</button>
      </div>
      <div id="errorMsg"></div>
      
      <div class="pace-control">
        <span class="hint">Current step pace:</span>
        <button class="btn pace" id="paceDown"><i class="fas fa-minus"></i> pace</button>
        <span id="paceNow">100%</span>
        <button class="btn pace" id="paceUp"><i class="fas fa-plus"></i> pace</button>
        <span class="hint">(±1% affects all intervals)</span>
        <div style="margin-top:10px;display:flex;align-items:center;gap:8px;justify-content:center;">
          <span class="hint">&#x2753; Threshold:</span>
          <input type="text" id="thresholdInput" maxlength="5" placeholder="6:00"
            style="width:72px;font-family:monospace;font-size:1em;text-align:center;
                   background:rgba(0,0,0,0.3);color:#fff;border:1px solid rgba(255,255,255,0.3);
                   border-radius:4px;padding:4px;" />
          <span class="hint">min/km</span>
          <button class="btn pace" id="btnThreshold"><i class="fas fa-check"></i> Set</button>
          <span class="hint" id="thresholdHint"></span>
        </div>
      </div>
    </section>

    <!-- Input Section -->
    <section id="inputSection" class="card">
      <h3><i class="fas fa-file-upload"></i> Input - Workout File</h3>
      <label for="zwoFile" class="file-btn"><i class="fas fa-upload"></i> Choose .ZWO or Drag&Drop</label>
      <input type="file" id="zwoFile" accept=".zwo,.xml" style="display:none;" />
      <span id="uploadStatus"></span>
      <div class="hint">.ZWO is an XML file (Zwift workout). We parse SteadyState / IntervalsT / Warmup / Cooldown.</div>
    </section>

    <!-- Test Section -->
    <section id="testSection" class="card">
      <h3><i class="fas fa-tools"></i> Test - Manual Controls</h3>
      <div class="test-controls">
        <button class="btn test-btn" id="speedDown"><i class="fas fa-chevron-down"></i> Speed Down</button>
        <button class="btn test-up" id="speedUp"><i class="fas fa-chevron-up"></i> Speed Up</button>
        <button class="btn test-btn" id="inclineDown"><i class="fas fa-chevron-down"></i> Incline Down</button>
        <button class="btn test-up" id="inclineUp"><i class="fas fa-chevron-up"></i> Incline Up</button>
      </div>
      <div class="hint" style="text-align:center;">Test buttons - trigger GPIO pins directly</div>
    </section>

  </div> <!-- end container -->

<script>
%WORKOUT_TS_BUNDLE%
</script>

</body>
</html>
)rawliteral";

/**
 * Template processor for TypeScript-based workout page
 * Note: Large JS bundles may cause truncation issues with send() + template processor
 */
String workoutTemplateProcessorTS(const String& var) {
  if (var == "WORKOUT_TS_BUNDLE") {
    return String((const char*)WORKOUT_TS_BUNDLE);
  }
  return String();
}

/**
 * Register workout page routes
 */
inline void addRoutes(AsyncWebServer& server) {
  // Serve the /workout.js directly (for caching)
  server.on("/workout.js", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send(200, "application/javascript", (const char*)WORKOUT_TS_BUNDLE);
  });
  
  // Serve HTML page with TRUE chunked response - no buffering!
  server.on("/workout", HTTP_GET, [](AsyncWebServerRequest* req){
    const char* html = WORKOUT_HTML_TS;
    const char* js = (const char*)WORKOUT_TS_BUNDLE;
    const char* placeholder = strstr(html, "%WORKOUT_TS_BUNDLE%");
    
    if (!placeholder) {
      req->send(200, "text/html", html);
      return;
    }
    
    size_t htmlBeforeJs = placeholder - html;
    size_t jsLen = strlen(js);
    size_t htmlAfterJsLen = strlen(placeholder + 19); // 19 = strlen("%WORKOUT_TS_BUNDLE%")
    
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

}} // namespace Ui::Workout
