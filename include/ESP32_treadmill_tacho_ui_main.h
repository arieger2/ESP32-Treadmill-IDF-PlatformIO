#pragma once
#ifndef ESP32_TREADMILL_TACHO_UI_MAIN_H
#define ESP32_TREADMILL_TACHO_UI_MAIN_H

#include <Arduino.h>
#include "ESP32_treadmill_tacho_web.h"  // must expose AsyncWebServer server
// Forward the template processor implemented in your web .cpp
String processTemplate(const String& var);

// ---------- Monitor (Main) Page ----------
namespace Ui { namespace Main {

static const char MAIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <link rel="stylesheet" href="https://use.fontawesome.com/releases/v5.7.2/css/all.css">
  <script>
    // After OTA update device reboots - if user manually goes to landing page, ensure clean history
    if (document.referrer && document.referrer.indexOf('/update') >= 0) {
      if (window.history.replaceState) {
        window.history.replaceState(null, '', '/');
      }
    }
  </script>
  <style>
    html { font-family: Arial; display: inline-block; margin: 0 auto; text-align: center; }
    h2 { font-size: 3.0rem; }
    p { font-size: 3.0rem; margin: 0.6rem 0; }
    .units { font-size: 1.2rem; }
    .sensor-speed { color: #666; font-size: 2.0rem; }
    .speed-row {
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 0.6rem;
      max-width: 600px;
      margin: 0.6rem auto;
      text-align: center;
    }
    .speed-row .speed-left {
      display: inline-flex;
      align-items: baseline;
      gap: 0.4rem;
      font-size: 2.5rem;
    }
    .speed-row .speed-left .units {
      font-size: 0.9rem;
      color: #666;
    }
    .hr-row {
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 1.2rem;
      margin: 0.6rem auto;
    }
    .hr-row .metric {
      display: flex;
      align-items: baseline;
      gap: 0.4rem;
      font-size: 2.5rem;
      color: #cc1f3a;
    }

    .chart-container {
      width: 90%%;
      max-width: 800px;
      margin: 1.5rem auto;
      background: #f7f7f7;
      border-radius: 8px;
      padding: 1rem;
      box-shadow: 0 2px 6px rgba(0,0,0,0.08);
    }
    .chart-container svg {
      width: 100%% !important;
      height: 70px !important;
      display: block;
    }
    .hr-row .metric.rr {
      color: #0070c0;
    }
    .hr-row .metric .units {
      font-size: 0.9rem;
      color: #666;
    }
    .duration-row {
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 1.5rem;
      flex-wrap: wrap;
    }
    .duration-row .duration-left {
      display: flex;
      align-items: center;
      gap: 0.4rem;
    }
    .duration-row .duration-right {
      display: flex;
      align-items: center;
      gap: 0.4rem;
      font-size: inherit;
      color: #555;
    }
    .button { border: none; color: white; padding: 15px 32px; text-align: center;
              text-decoration: none; display: inline-block; font-size: 16px;
              margin: 4px 2px; cursor: pointer; border-radius: 5px; }
    .nav { background-color: #333; padding: 10px; margin-bottom: 20px; }
    .nav a { color: white; text-decoration: none; padding: 10px 20px; margin: 0 5px;
             background-color: #555; border-radius: 5px; }
    .nav a:hover { background-color: #777; }
    .button1 { background-color: #4CAF50; }
    .button2 { background-color: #008CBA; }
    .button3 { background-color: #cc0000; }
    .button-green { background-color: #4CAF50; } /* green */
    .button-red   { background-color: #cc0000; } /* red */
        /* --- Buttons: better touch targets & feedback --- */
    .button {
      min-width: 120px;            /* keep desktop look */
      min-height: 44px;            /* WCAG-recommended tap size */
      touch-action: manipulation;  /* faster clicks on mobile */
      -webkit-tap-highlight-color: transparent;
      user-select: none;
    }
    .button:focus-visible {
      outline: 3px solid rgba(255,255,255,0.75);
      outline-offset: 2px;
    }
    .button:active {
      transform: translateY(1px);
      filter: brightness(0.95);
    }

    /* --- Responsive type scale for small screens (readability) --- */
    @media (max-width: 600px) {
      h2 { font-size: 2.2rem; }
      p  { font-size: 2.2rem; }
      .units { font-size: 1.1rem; }
      .sensor-speed { font-size: 1.6rem; }
      .hr-row {
        flex-direction: column;
        gap: 0.6rem;
      }
      .hr-row .metric {
        font-size: 2.2rem;
      }
      .chart-container {
        width: 95%%;
        padding: 0.8rem;
      }
      .chart-container svg {
        height: 60px !important;
      }
      .duration-row {
        flex-direction: column;
        gap: 0.6rem;
      }
      .duration-row .duration-right {
        font-size: inherit;
      }
    }

    /* --- Button layout becomes two columns on phones --- */
    @media (max-width: 600px) {
      /* Up/Down and other buttons inside <p> blocks */
      p .button {
        display: inline-block;
  width: calc(50%% - 10px);   /* two buttons side-by-side */
        margin: 6px 5px;
      }
    }

    /* --- Very small screens: stack buttons full-width --- */
    @media (max-width: 380px) {
      p .button {
        display: block;
  width: 100%%;
        margin: 8px 0;
      }
    }

    .speed-row .units {
      font-size: 0.9rem;
    }

    /* --- Navigation adjusts spacing on small screens --- */
    @media (max-width: 600px) {
      .nav { padding: 8px; }
      .nav a {
        display: inline-block;
        margin: 4px 3px;
        padding: 8px 12px;
        font-size: 14px;
      }
    }

    /* --- Reduce motion if user prefers --- */
    @media (prefers-reduced-motion: reduce) {
      .button:active { transform: none; }
    }
  </style>
</head>
<body>
  <div class="nav">
    <a href="/"><i class="fas fa-tachometer-alt"></i> Monitor</a>
    <a href="/settings"><i class="fas fa-cogs"></i> Settings</a>
    <a href="/workout"><i class="fas fa-running"></i> Workout</a>
  </div>

  <h2>Treadmill Monitor</h2>

  <p class="speed-row">
    <span class="speed-left">
      <i class="fas fa-running" style="color:#059E8A;"></i>
      <span id="speed">%SPEED%</span><sup class="units">km/h</sup>
    </span>
  </p>

  <p><i class="fas fa-running" style="color:#059E8A;"></i>
   <span id="pacemin">%PACEMIN%</span>:<span id="pacesec">%PACESEC%</span>
   <sup class="units">min/km</sup>
  </p>

  <p><i class="fas fa-shoe-prints" style="color:#2d0000;"></i>
   <span id="distance">%DISTANCE%</span>
   <sup class="units" id="distanceunit">%DISTANCE_UNIT%</sup>
  </p>

  <p class="duration-row">
    <span class="duration-left">
      <i class="fas fa-stopwatch" style="color:#059E8A;"></i>
      <span id="hour">%HOUR%</span>:<span id="minute">%MINUTE%</span>:<span id="second">%SECOND%</span>
    </span>
    <span class="duration-right">
      <i class="fas fa-clock" style="color:#059E8A;"></i>
      <span id="datetime">%DATETIME%</span>
    </span>
  </p>

  <div class="hr-row">
    <span class="metric hr">
      <i class="fas fa-heart"></i>
      <span id="heartrate">%HEARTRATE%</span><sup class="units">bpm</sup>
    </span>
    <span class="metric rr">
      <i class="fas fa-wave-square"></i>
      <span id="rr">%RR%</span><sup class="units">ms</sup>
    </span>
  </div>

  <div class="chart-container">
    <svg id="hrChart" viewBox="0 0 600 60" preserveAspectRatio="none"></svg>
  </div>
  <div class="chart-container">
    <svg id="rrChart" viewBox="0 0 600 60" preserveAspectRatio="none"></svg>
  </div>

  <p><i class="fas fa-arrows-alt-h" style="color:#059E8A;"></i>
     <span id="offset">%OFFSET%</span><sup class="units">km/h offset</sup><br>
     <a href="/offset/up"><button class="button button1">Up</button></a>
     <a href="/offset/down"><button class="button button2">Down</button></a></p>

  <p><i class="fas fa-tachometer-alt" style="color:#00ADD6;"></i>
     Band: <span id="rpm">%RPM%</span><sup class="units">rpm, </sup>
     Motor: <span id="motorrpm">%MOTORRPM%</span> <sup class="units">rpm</sup>
  </p>

  <p><i class="fas fa-microchip" style="color:#059E8A;"></i>
     CPU Usage: <span id="cpuusage">%CPUUSAGE%</span> <sup class="units">%%</sup>
  </p>

  <p><a href="/reset"><button class="button button3">Reset Workout</button></a></p>
  <p><a href="/testdata"><button class="button %TESTDATABUTTONCLASS%">%TESTDATABUTTONTEXT%</button></a></p>

<script>
const hrChartEl = document.getElementById('hrChart');
const rrChartEl = document.getElementById('rrChart');

function renderLineChart(svg, series, color, units) {
  if (!svg) return;
  let width = 600;
  let height = 240;
  const viewBoxAttr = svg.getAttribute('viewBox');
  if (viewBoxAttr) {
    const parts = viewBoxAttr.trim().split(/\s+/);
    if (parts.length === 4) {
      const parsedWidth = parseFloat(parts[2]);
      const parsedHeight = parseFloat(parts[3]);
      if (!Number.isNaN(parsedWidth) && parsedWidth > 0) width = parsedWidth;
      if (!Number.isNaN(parsedHeight) && parsedHeight > 0) height = parsedHeight;
    }
  }

  if (!Array.isArray(series) || !series.length) {
    svg.innerHTML = '<rect x="0" y="0" width="100%%" height="100%%" fill="white" stroke="#ddd" />'
      + '<text x="50%%" y="50%%" dominant-baseline="middle" text-anchor="middle" fill="#888" font-size="14">No data</text>';
    return;
  }

  const baseTime = series[0].t;
  const xValues = series.map(p => ((p.t - baseTime) / 1000));
  const maxX = xValues[xValues.length - 1] || 1;

  let minY = series[0].v;
  let maxY = series[0].v;
  for (let i = 1; i < series.length; i++) {
    const val = series[i].v;
    if (val < minY) minY = val;
    if (val > maxY) maxY = val;
  }
  if (minY === maxY) {
    minY -= 1;
    maxY += 1;
  }

  const pathPoints = [];
  for (let i = 0; i < series.length; i++) {
    const x = maxX === 0 ? 0 : (xValues[i] / maxX) * width;
    const normalized = (series[i].v - minY) / (maxY - minY);
    const y = height - (normalized * height);
    pathPoints.push(`${i === 0 ? 'M' : 'L'}${x.toFixed(2)} ${y.toFixed(2)}`);
  }

  const minLabel = `${minY.toFixed(0)} ${units}`;
  const maxLabel = `${maxY.toFixed(0)} ${units}`;
  const spanLabel = `${Math.max(0, Math.round(maxX))}s window`;

  svg.innerHTML = `
    <rect x="0" y="0" width="${width}" height="${height}" fill="white" stroke="#ddd" />
    <path d="${pathPoints.join(' ')}" stroke="${color}" stroke-width="2" fill="none" />
    <text x="4" y="${Math.min(height - 4, 16)}" fill="#555" font-size="12">${maxLabel}</text>
    <text x="4" y="${height - 4}" fill="#555" font-size="12">${minLabel}</text>
    <text x="${width - 4}" y="${height - 4}" fill="#888" font-size="12" text-anchor="end">${spanLabel}</text>
  `;
}

function updateValue(id, endpoint) {
  fetch('/' + endpoint + '?t=' + Date.now(), { cache: 'no-store' })
    .then(r => r.text())
    .then(t => { var el=document.getElementById(id); if(el) el.innerHTML=t; })
    .catch(() => {});
}

function updateSeries(svg, endpoint, color, units) {
  if (!svg) return;
  fetch('/' + endpoint + '?t=' + Date.now(), { cache: 'no-store' })
    .then(r => r.ok ? r.json() : [])
    .then(series => {
      if (!Array.isArray(series)) {
        svg.innerHTML = '';
        return;
      }
      renderLineChart(svg, series, color, units);
    })
    .catch(() => {
  svg.innerHTML = '<text x="50%%" y="50%%" dominant-baseline="middle" text-anchor="middle" fill="#c00" font-size="14">Unavailable</text>';
    });
}

function refreshMetrics() {
  // Only poll if page is visible
  if (document.hidden) return;
  
  updateValue('speed', 'api/speed');
  updateValue('pacemin', 'api/pacemin');
  updateValue('pacesec', 'api/pacesec');
  updateValue('cpuusage', 'api/cpu');
  updateValue('distance', 'api/distance');
  updateValue('distanceunit', 'api/distanceunit');
  updateValue('hour', 'api/hour');
  updateValue('minute', 'api/minute');
  updateValue('second', 'api/second');
  updateValue('offset', 'api/offset');
  updateValue('rpm', 'api/rpm');
  updateValue('motorrpm', 'api/motorrpm');
  updateValue('heartrate', 'api/heartrate');
  updateValue('rr', 'api/rr');
  updateValue('datetime', 'api/datetime');
  updateSeries(hrChartEl, 'api/hr-series', '#cc1f3a', 'bpm');
  updateSeries(rrChartEl, 'api/rr-series', '#0070c0', 'ms');
}

setInterval(refreshMetrics, 1000);
refreshMetrics();

// Resume polling when page becomes visible again
document.addEventListener('visibilitychange', function() {
  if (!document.hidden) refreshMetrics();
});
</script>
</body>
</html>
)rawliteral";

inline void addRoutes(AsyncWebServer& server) {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send(
      200,
      "text/html",
      MAIN_HTML,
      [](const String& v){ return processTemplate(v); }  // template processor
    );
  });
}

}} // namespace Ui::Main

#endif