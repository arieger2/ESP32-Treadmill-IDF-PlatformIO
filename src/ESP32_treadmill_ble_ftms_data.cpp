// ============================================================================
// BLE FTMS/RSC/HR Data Transmission - Sending metrics to clients
// ============================================================================

#include "ESP32_treadmill_tacho_ble.h"

// ============================================================================
// RSC DATA (Cadence)
// ============================================================================

void sendRSC_BLE_Data() {
  if (!pRSCMeasurement || !bleData.clientConnected) return;

  // cadence: 2 steps per belt rev
  float   stepsPerMinute = metrics.rpm * 2.0f;
  uint8_t cadence        = (stepsPerMinute > 255.0f) ? 255 : (uint8_t)stepsPerMinute;

  // speed for RSC: m/s * 256 (use mpsSmooth, same source as FTMS)
  float    mps       = metrics.mpsSmooth + metrics.mpsOffset;
  uint16_t instSpeed = (uint16_t)(mps * 256.0f);

  // flags: bit1=Total Distance present, bit2=Running (1=Running, 0=Walking)
  uint8_t  flags       = 0x06;
  uint32_t totalDistDm = (uint32_t)(metrics.workoutDistance * 10.0f); // 1/10 m (decimeters)

  uint8_t pkt[8] = {
    flags,
    (uint8_t)(instSpeed & 0xFF),
    (uint8_t)(instSpeed >> 8),
    cadence,
    (uint8_t)(totalDistDm & 0xFF),
    (uint8_t)((totalDistDm >> 8) & 0xFF),
    (uint8_t)((totalDistDm >> 16) & 0xFF),
    (uint8_t)((totalDistDm >> 24) & 0xFF)
  };

  pRSCMeasurement->setValue(pkt, sizeof(pkt));
  pRSCMeasurement->notify();

  static int dbg = 0;
  if (++dbg % 100 == 0) {
    Serial.printf("📊 RSC → Garmin: Speed=%.1f m/s, Cadence=%u spm, Distance=%.1f m\r\n",
                  mps, cadence, metrics.workoutDistance);
    Serial.printf("   RSC Packet: [%02X %02X %02X %02X %02X %02X %02X %02X]  Flags=0x%02X\r\n",
                  pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5], pkt[6], pkt[7], flags);
  }
}

// ============================================================================
// HEART RATE DATA (broadcast from client)
// ============================================================================

void sendHR_BLE_Data() {
  if (!pHRMeasurement || !bleData.clientConnected) return;
  if (!metrics.heartRateConnected || metrics.heartRateBpm == 0) return;

  // Heart Rate Measurement format:
  // Byte 0: Flags (0x00 = HR is uint8, no additional data)
  // Byte 1: Heart Rate Value (bpm)
  uint8_t hrData[2];
  hrData[0] = 0x00;  // Flags: 8-bit HR value
  hrData[1] = (uint8_t)(metrics.heartRateBpm > 255 ? 255 : metrics.heartRateBpm);

  pHRMeasurement->setValue(hrData, 2);
  pHRMeasurement->notify();

  static int dbg = 0;
  if (++dbg % 50 == 0) {
    Serial.printf("❤️  HR → Zwift: %u bpm (from belt)\r\n", hrData[1]);
  }
}

// ============================================================================
// FTMS DATA (Treadmill)
// ============================================================================

void sendFTMS_BLE_Data() {
  if (!bleData.clientConnected) return;

  // Always send RSC data too
  sendRSC_BLE_Data();

  if (pFTMSTreadmillData && metrics.controlRequested && isNotificationEnabled(pFTMSTreadmillData)) {
    uint8_t ftmsData[20] = {0};
    size_t idx = 0;

    float    speedKmh            = (metrics.mpsSmooth + metrics.mpsOffset) * 3.6f;
    uint16_t instantaneousSpeed  = (uint16_t)(speedKmh * 100.0f); // 0.01 km/h
    uint16_t averageSpeed        = instantaneousSpeed;
    uint32_t totalDistanceMeters = (uint32_t)metrics.workoutDistance;
    int16_t  inclinationDeciPct  = (int16_t)(metrics.currentInclination * 10.0f);
    uint16_t elapsedTimeSec      = (uint16_t)((millis() - metrics.sessionStartTime) / 1000);
    // Pace: min/km = 60 / speed_kmh, unit = 1 min/km, clamped to 253
    uint8_t  instPace = (speedKmh > 0.1f)
                          ? (uint8_t)fminf(253.0f, roundf(60.0f / speedKmh))
                          : 0;

    // Flags per FTMS spec:
    //   bit1=Average Speed, bit2=Total Distance, bit3=Inclination+Ramp Angle,
    //   bit5=Pace (inst+avg), bit9=Elapsed Time
    uint16_t flags = 0x022E;

    ftmsData[idx++] = flags & 0xFF;
    ftmsData[idx++] = (flags >> 8) & 0xFF;

    ftmsData[idx++] = instantaneousSpeed & 0xFF;
    ftmsData[idx++] = (instantaneousSpeed >> 8) & 0xFF;

    ftmsData[idx++] = averageSpeed & 0xFF;
    ftmsData[idx++] = (averageSpeed >> 8) & 0xFF;

    ftmsData[idx++] = (uint8_t)(totalDistanceMeters & 0xFF);
    ftmsData[idx++] = (uint8_t)((totalDistanceMeters >> 8) & 0xFF);
    ftmsData[idx++] = (uint8_t)((totalDistanceMeters >> 16) & 0xFF);

    // bit3: Inclination (0.1%) + Ramp Angle (0.1°) — Ramp Angle not available, send 0
    ftmsData[idx++] = (uint8_t)(inclinationDeciPct & 0xFF);
    ftmsData[idx++] = (uint8_t)((inclinationDeciPct >> 8) & 0xFF);
    ftmsData[idx++] = 0x00; // Ramp Angle low byte
    ftmsData[idx++] = 0x00; // Ramp Angle high byte

    // bit5: Instantaneous Pace + Average Pace (each uint8, min/km)
    ftmsData[idx++] = instPace;
    ftmsData[idx++] = instPace; // average = instantaneous (same source)

    // bit9: Elapsed Time (s)
    ftmsData[idx++] = (uint8_t)(elapsedTimeSec & 0xFF);
    ftmsData[idx++] = (uint8_t)((elapsedTimeSec >> 8) & 0xFF);

    pFTMSTreadmillData->setValue(ftmsData, idx);
    pFTMSTreadmillData->notify();

    static int dbg = 0;
    if (++dbg % 200 == 0) {
      Serial.printf("📈 FTMS: Speed=%.1f km/h, Pace=%u min/km, Distance=%u m, Incline=%.1f%%, Time=%us\r\n",
                    speedKmh, instPace, (unsigned)totalDistanceMeters,
                    metrics.currentInclination, (unsigned)elapsedTimeSec);
    }
  }
}

// ============================================================================
// DIAGNOSTIC
// ============================================================================

void FMTS_check() {
  if (!pFTMSControlPoint) {
    Serial.println("⚠️ FTMS Control Point characteristic not initialized");
    return;
  }

  uint32_t props = pFTMSControlPoint->getProperties();
  Serial.printf("FTMS Control Point properties: 0x%04X\n", props);
  Serial.printf("  - Write:     %s\n", (props & NIMBLE_PROPERTY::WRITE) ? "YES" : "NO");
  Serial.printf("  - Write NR:  %s\n", (props & NIMBLE_PROPERTY::WRITE_NR) ? "YES" : "NO");
  Serial.printf("  - Indicate:  %s\n", (props & NIMBLE_PROPERTY::INDICATE) ? "YES" : "NO");

  NimBLEDescriptor* cccd = pFTMSControlPoint->getDescriptorByUUID(NimBLEUUID((uint16_t)0x2902));
  if (cccd) {
    std::string val = cccd->getValue();
    uint8_t b0 = val.size() > 0 ? (uint8_t)val[0] : 0;
    uint8_t b1 = val.size() > 1 ? (uint8_t)val[1] : 0;

    Serial.printf("FTMS Control Point CCCD status:\n");
    Serial.printf("  - Value:       [0x%02X 0x%02X]\n", b0, b1);
    Serial.printf("  - Indications: %s\n", (b0 & 0x02) ? "Enabled" : "Disabled");
    Serial.printf("  - Notifications: %s\n", (b0 & 0x01) ? "Enabled" : "Disabled");
  }
  Serial.println();
}
