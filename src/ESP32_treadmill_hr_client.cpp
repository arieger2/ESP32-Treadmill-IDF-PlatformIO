#include <Arduino.h>
#include "NimBLEDevice.h"
#include "NimBLEUtils.h"

#include "ESP32_treadmill_hr_client.h"
#include "ESP32_treadmill_tacho_config.h"
#include "ESP32_treadmill_tacho_wifi.h"  // For WiFiStatus

extern TreadmillMetrics metrics;
extern WiFiStatus wifi;  // WiFi status for coexistence check

namespace {
constexpr uint16_t HEART_RATE_SERVICE_UUID16     = 0x180D;
constexpr uint16_t HEART_RATE_MEASUREMENT_UUID16 = 0x2A37;
const NimBLEUUID HEART_RATE_SERVICE_UUID(HEART_RATE_SERVICE_UUID16);
const NimBLEUUID HEART_RATE_MEASUREMENT_UUID(HEART_RATE_MEASUREMENT_UUID16);

NimBLEScan*                 gScan   = nullptr;
NimBLEClient*               gClient = nullptr;
NimBLERemoteCharacteristic* gHrMeasurementChar = nullptr;

const NimBLEAdvertisedDevice* gPendingDevice    = nullptr;
bool                         gConnectRequested = false;

uint32_t gNextScanMs           = 0; // millis timestamp for next scan attempt
bool     gConnectionInProgress = false;
uint32_t gHRClientInitTime     = 0; // Track when HR client was initialized
constexpr uint32_t WIFI_STABILIZATION_DELAY_MS = 15000; // Wait 15s after init before starting BLE scan

void scheduleScan(uint32_t delayMs = 0);
void startScanIfDue();
void connectToHeartRateDevice(const NimBLEAdvertisedDevice* device);

// Optional: Flag-Defines
constexpr uint8_t HR_FLAG_16BIT_HR          = 0x01;
constexpr uint8_t HR_FLAG_SENSOR_CONTACT    = 0x06; // 0x02 + 0x04 (nur falls du es brauchst)
constexpr uint8_t HR_FLAG_ENERGY_EXPENDED   = 0x08;
constexpr uint8_t HR_FLAG_RR_PRESENT        = 0x10;

void handleHeartRateNotification(NimBLERemoteCharacteristic* /*chr*/,
                                 uint8_t* data, size_t len, bool /*isNotify*/) {
    if (!data || len < 2) return;

    const uint8_t flags = data[0];
    uint16_t bpm = 0;
    size_t idx = 1; // Start hinter flags

    // --- Herzfrequenz auslesen (8- oder 16-bit)
    if (flags & HR_FLAG_16BIT_HR) {
        if (len < 3) return;
        bpm = static_cast<uint16_t>(data[1] | (static_cast<uint16_t>(data[2]) << 8));
        idx = 3;
    } else {
        bpm = data[1];
        idx = 2;
    }

    if (bpm > 0) {
        metrics.heartRateBpm        = bpm;
        metrics.heartRateConnected  = true;
        metrics.heartRateLastUpdate = millis();
    metrics.hrBuffer.push(HRValue{millis(), bpm});
    }

    // --- Energy Expended (falls vorhanden) überspringen (2 Byte)
    if (flags & HR_FLAG_ENERGY_EXPENDED) {
        if (idx + 2 > len) return;
        metrics.heartRateEnergy = data[idx] | (data[idx+1] << 8); 
        //printf("Energy Expended: %u kJ\n", metrics.heartRateEnergy);
        idx += 2;
    }

    // --- RR-Intervalle einlesen (können mehrfach im Paket vorkommen)
    if (flags & HR_FLAG_RR_PRESENT) {
        while (idx + 1 < len) {
            uint16_t rr_raw = static_cast<uint16_t>(data[idx] | (static_cast<uint16_t>(data[idx+1]) << 8));
            idx += 2;

            // Einheit ist 1/1024 s -> in Millisekunden umrechnen
            const uint16_t rr_ms = (static_cast<uint16_t>(rr_raw) * 1000.0f) / 1024.0f;
            metrics.rrLastMs = rr_ms;
            //printf("RR Interval: %u ms\n", rr_ms);
            // Optional: Artefakt-Filter (z. B. 300–2000 ms)
            if (rr_ms >= 300.0f && rr_ms <= 2000.0f) {
                metrics.rrBuffer.push({millis(), rr_ms}); 
            }
        }
    }
}

class HeartRateClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* client) override {
    Serial.println("[HR] Connected to device");
    gConnectionInProgress = false;
    gConnectRequested     = false;
    gPendingDevice        = nullptr;

        if (!client) {
            scheduleScan(2000);
            return;
        }

        if (!client->discoverAttributes()) {
            Serial.println("[HR] Attribute discovery failed");
            client->disconnect();
            return;
        }

        NimBLERemoteService* service = client->getService(HEART_RATE_SERVICE_UUID);
        if (!service) {
            Serial.println("[HR] Heart Rate service not found");
            client->disconnect();
            return;
        }

        gHrMeasurementChar = service->getCharacteristic(HEART_RATE_MEASUREMENT_UUID);
        if (!gHrMeasurementChar) {
            Serial.println("[HR] Heart Rate Measurement characteristic missing");
            client->disconnect();
            return;
        }

        if (!gHrMeasurementChar->canNotify()) {
            Serial.println("[HR] Heart Rate characteristic does not support notify");
            client->disconnect();
            return;
        }

        if (!gHrMeasurementChar->subscribe(true, handleHeartRateNotification)) {
            Serial.println("[HR] Failed to enable Heart Rate notifications");
            client->disconnect();
            return;
        }

        metrics.heartRateConnected  = true;
        metrics.heartRateLastUpdate = millis();
        metrics.heartRateBpm        = 0; // wait for first notification
        Serial.println("[HR] Notifications enabled");
    }

    void onDisconnect(NimBLEClient* client, int reason) override {
        Serial.printf("[HR] Device disconnected (reason=%d/%s)\n", reason, NimBLEUtils::returnCodeToString(reason));
        gConnectionInProgress       = false;
        gConnectRequested           = false;
        metrics.heartRateConnected  = false;
        metrics.heartRateBpm        = 0;
        metrics.heartRateLastUpdate = 0;
        gHrMeasurementChar          = nullptr;
        gPendingDevice              = nullptr;

        if (client != nullptr) {
            client->deleteServices();
        }

        scheduleScan(3000); // retry after a short delay
    }

    void onConnectFail(NimBLEClient* client, int reason) override {
    Serial.printf("[HR] Connect attempt failed (reason=%d/%s)\n", reason, NimBLEUtils::returnCodeToString(reason));
        gConnectionInProgress = false;
        gConnectRequested     = false;
        gPendingDevice        = nullptr;
        if (client != nullptr) {
            client->deleteServices();
        }
        scheduleScan(3000);
    }
};

class HeartRateScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        if (!advertisedDevice) {
            return;
        }
        if (!advertisedDevice->isAdvertisingService(HEART_RATE_SERVICE_UUID)) {
            return;
        }
        if (gConnectionInProgress || gConnectRequested) {
            return;
        }

        Serial.printf("[HR] Found Heart Rate device: %s\n", advertisedDevice->getAddress().toString().c_str());
        NimBLEDevice::getScan()->stop();

        gPendingDevice    = advertisedDevice;
        gConnectRequested = true;
    }
};

HeartRateClientCallbacks gClientCallbacks;
HeartRateScanCallbacks   gScanCallbacks;

void scheduleScan(uint32_t delayMs) {
    gNextScanMs = millis() + delayMs;
}

void startScanIfDue() {
    if (!gScan || gScan->isScanning()) {
        return;
    }

    if (gClient && gClient->isConnected()) {
        return;
    }

    if (gConnectionInProgress || gConnectRequested) {
        return;
    }

    // **CRITICAL WiFi/BLE Coexistence Fix**:
    // Don't scan while WiFi is trying to connect!
    // ESP32 has shared 2.4GHz radio - BLE scanning blocks WiFi completely
    if (!wifi.connected && !wifi.apMode) {
        // WiFi still trying to connect - defer BLE scan
        scheduleScan(2000);  // Retry in 2s
        return;
    }

    const uint32_t now = millis();
    if (gNextScanMs > now) {
        return;
    }

    Serial.println("[HR] Starting BLE scan for Heart Rate service");
    if (!gScan->start(0, false)) {
        // Controller is still busy (e.g., cleaning up previous connection). Try again shortly.
        scheduleScan(1000);
    }
}

void connectToHeartRateDevice(const NimBLEAdvertisedDevice* device) {
    if (!device) {
        gConnectionInProgress = false;
        gPendingDevice        = nullptr;
        scheduleScan(1000);
        return;
    }

    NimBLEClient* client = gClient;

    if (!client) {
        client = NimBLEDevice::getClientByPeerAddress(device->getAddress());
        if (!client) {
            client = NimBLEDevice::getDisconnectedClient();
        }
        if (!client) {
            client = NimBLEDevice::createClient();
            if (!client) {
                Serial.println("[HR] Failed to create NimBLE client");
                gConnectionInProgress = false;
                gPendingDevice        = nullptr;
                scheduleScan(5000);
                return;
            }
            Serial.println("[HR] Created new NimBLE client");
            client->setClientCallbacks(&gClientCallbacks, false);
            client->setConnectionParams(12, 24, 0, 150); // recommended defaults
            client->setConnectTimeout(7000);
        } else {
            Serial.println("[HR] Reusing existing NimBLE client");
        }
        gClient = client;
    }

    if (client->isConnected()) {
        Serial.println("[HR] Already connected to Heart Rate device");
        gConnectionInProgress = false;
        gPendingDevice        = nullptr;
        return;
    }

    Serial.printf("[HR] Connecting to %s\n", device->getAddress().toString().c_str());
    gConnectionInProgress = true;

    if (!client->connect(device)) {
        Serial.println("[HR] Connection attempt failed to start");
        gConnectionInProgress = false;
        gPendingDevice        = nullptr;
        scheduleScan(3000);
        return;
    }

    gPendingDevice = nullptr;
}

} // namespace

void initHeartRateClient() {
    if (gScan) {
        // Already initialised
        return;
    }

    gHRClientInitTime = millis(); // Record init time for scan delay

    gScan = NimBLEDevice::getScan();
    if (!gScan) {
        Serial.println("[HR] NimBLE scan object unavailable");
        return;
    }

    gScan->setScanCallbacks(&gScanCallbacks, false);
    gScan->setActiveScan(true);
    gScan->setInterval(45);
    gScan->setWindow(30);
    gScan->setDuplicateFilter(true);

    metrics.heartRateBpm        = 0;
    metrics.heartRateConnected  = false;
    metrics.heartRateLastUpdate = 0;

    // Don't schedule scan immediately - startScanIfDue() will wait for WiFi stabilization
    Serial.println("[HR] Client initialized, scan will start after WiFi stabilization (15s)");
    scheduleScan(0); // Set scan ready, but startScanIfDue() will enforce the delay
}

void heartRateClientLoop() {
    if (gConnectRequested && !gConnectionInProgress) {
        const NimBLEAdvertisedDevice* target = gPendingDevice;
        gConnectRequested                    = false; // will be reset on failure/success
        connectToHeartRateDevice(target);
    }

    startScanIfDue();

    if (!metrics.heartRateConnected || metrics.heartRateLastUpdate == 0) {
        return;
    }

    const uint32_t now                        = millis();
    constexpr uint32_t HEART_RATE_TIMEOUT_MS = 10000; // 10s without data -> considered lost
    if (now - metrics.heartRateLastUpdate > HEART_RATE_TIMEOUT_MS) {
        Serial.println("[HR] Heart Rate data timed out");
        metrics.heartRateConnected  = false;
        metrics.heartRateBpm        = 0;
        metrics.heartRateLastUpdate = 0;
        if (gClient && gClient->isConnected()) {
            gClient->disconnect();
        } else {
            gConnectionInProgress = false;
            scheduleScan(1000);
        }
    }
}
