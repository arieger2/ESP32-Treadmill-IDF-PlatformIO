#pragma once
#ifndef ESP32_TREADMILL_TACHO_UI_ASSETS_H
#define ESP32_TREADMILL_TACHO_UI_ASSETS_H

#include <Arduino.h>
#include "ESP32_treadmill_tacho_web.h"

// If you later want to serve static assets (CSS/JS) locally, add them here.
// For now, keep it as a no-op to satisfy Ui::Assets::addRoutes(server).

namespace Ui { namespace Assets {

inline void addRoutes(AsyncWebServer& server) {
  // no-op (reserved for /assets routes if needed later)
  (void)server;
}

}} // namespace Ui::Assets

#endif