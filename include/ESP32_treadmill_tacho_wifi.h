// ============================================================================
// WiFi Management - State Machine Based
// Handles WiFi Station and Access Point modes
// ============================================================================
#pragma once

#include <Arduino.h>

// Initialize WiFi system (call once in setup)
void wifiInit();

// WiFi loop handler (call regularly from main loop)
void wifiLoop();

// Get current WiFi state as string (for debugging)
const char* wifiGetStateName();

// Force switch to AP mode
void wifiForceAPMode();
