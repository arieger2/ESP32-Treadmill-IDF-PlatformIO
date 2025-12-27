#pragma once

// Initializes the BLE Heart Rate client and begins scanning for HR belts.
void initHeartRateClient();

// Periodic housekeeping (call from loop) to manage scanning and stale data.
void heartRateClientLoop();
