// ============================================================================
// WiFi Management - State Machine Implementation
// Based on Espressif ESP-IDF WiFi Programming Guide best practices:
// - Active reconnect on disconnect (not relying on auto-reconnect)
// - User-initiated disconnect flag to prevent unwanted reconnects
// - Soft recovery before hard recovery (escalation)
// - IP_EVENT_STA_LOST_IP handling
// ============================================================================
#include "ESP32_treadmill_tacho_wifi.h"
#include "ESP32_treadmill_tacho_config.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_coexist.h>
#include <esp_task_wdt.h>
#include <ESPmDNS.h>
#include <TimeLib.h>
#include <time.h>
#include <esp_sntp.h>

// External references
extern TreadmillMetrics metrics;
extern TreadmillStoredGlobals storedGlobals;
extern WiFiStatus wifi;

// Time sync globals
static bool gTimeSynced = false;
static uint32_t gLastTimeSyncAttemptMs = 0;

// User-initiated disconnect flag (per Espressif recommendation)
// Set this BEFORE calling esp_wifi_disconnect() or esp_wifi_stop()
static volatile bool gUserDisconnect = false;

// Reconnect backoff parameters
static const uint32_t RECONNECT_BASE_DELAY_MS = 1000;    // Start with 1 second
static const uint32_t RECONNECT_MAX_DELAY_MS = 30000;    // Max 30 seconds between attempts
static const uint8_t  SOFT_RESET_THRESHOLD = 3;          // After 3 fails: soft reset
static const uint8_t  HARD_RESET_THRESHOLD = 10;         // After 10 fails: hard reset

// WiFi State Machine Types
typedef enum {
    WIFI_STATE_INIT,
    WIFI_STATE_STA_CONNECTING,
    WIFI_STATE_STA_CONNECTED,
    WIFI_STATE_AP_MODE
} WiFiState_t;

typedef struct {
    WiFiState_t state;
    WiFiState_t previousState;
    uint32_t stateEnterTime;
    uint8_t connectionAttempts;
    uint8_t reconnectAttempts;        // Consecutive reconnect failures
    bool mdnsStarted;
    esp_netif_t* netif_sta;
    esp_netif_t* netif_ap;
    bool eventHandlersRegistered;
    uint32_t lastDisconnectTime;
    uint32_t lastReconnectAttempt;
    uint32_t reconnectBackoff;        // Current backoff delay
    bool wasConnected;
    bool hasIP;                       // Track L3 (IP) connectivity separately
    bool timeoutMessageShown;
    uint32_t initialConnectStartTime;
    bool stateEntered;                // Clean entry-action tracking
} WiFiManager_t;

static WiFiManager_t wifiMgr = {
    .state = WIFI_STATE_INIT,
    .previousState = WIFI_STATE_INIT,
    .stateEnterTime = 0,
    .connectionAttempts = 0,
    .reconnectAttempts = 0,
    .mdnsStarted = false,
    .netif_sta = nullptr,
    .netif_ap = nullptr,
    .eventHandlersRegistered = false,
    .lastDisconnectTime = 0,
    .lastReconnectAttempt = 0,
    .reconnectBackoff = RECONNECT_BASE_DELAY_MS,
    .wasConnected = false,
    .hasIP = false,
    .timeoutMessageShown = false,
    .initialConnectStartTime = 0,
    .stateEntered = false
};

// Forward declarations
static void wifiChangeState(WiFiState_t newState);
static void wifiStateMachine();
static void wifiInitSTA();
static void wifiInitAP();
static void wifiSoftReset();
static void wifiHardReset();
static void wifiTriggerReconnect(uint8_t reason);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void startMDNS();
static void syncTimeFromNetwork(bool force);

// ============================================================================
// State Management (clean entry-action pattern)
// ============================================================================
static void wifiChangeState(WiFiState_t newState) {
    if (wifiMgr.state == newState) return;
    
    const char* stateNames[] = {"INIT", "STA_CONNECTING", "STA_CONNECTED", "AP_MODE"};
    Serial.printf("📶 WiFi State: %s → %s\n", 
        stateNames[wifiMgr.state], stateNames[newState]);
    
    wifiMgr.previousState = wifiMgr.state;
    wifiMgr.state = newState;
    wifiMgr.stateEnterTime = millis();
    wifiMgr.stateEntered = false;  // Mark: entry-action not yet done
}

const char* wifiGetStateName() {
    const char* stateNames[] = {"INIT", "STA_CONNECTING", "STA_CONNECTED", "AP_MODE"};
    return stateNames[wifiMgr.state];
}

// ============================================================================
// Reconnect Helper - Per Espressif: Call esp_wifi_connect() on disconnect!
// ============================================================================
static void wifiTriggerReconnect(uint8_t reason) {
    // Skip if user-initiated disconnect (mode switch, etc.)
    if (gUserDisconnect) {
        Serial.println("  ↳ User-initiated disconnect, not reconnecting");
        return;
    }
    
    wifiMgr.reconnectAttempts++;
    
    // Calculate exponential backoff
    wifiMgr.reconnectBackoff = RECONNECT_BASE_DELAY_MS * (1 << min(wifiMgr.reconnectAttempts, (uint8_t)5));
    if (wifiMgr.reconnectBackoff > RECONNECT_MAX_DELAY_MS) {
        wifiMgr.reconnectBackoff = RECONNECT_MAX_DELAY_MS;
    }
    
    // Determine recovery strategy based on failure count
    if (wifiMgr.reconnectAttempts >= HARD_RESET_THRESHOLD) {
        Serial.printf("  ↳ Hard reset after %d failures\n", wifiMgr.reconnectAttempts);
        wifiHardReset();
    } else if (wifiMgr.reconnectAttempts >= SOFT_RESET_THRESHOLD) {
        Serial.printf("  ↳ Soft reset after %d failures\n", wifiMgr.reconnectAttempts);
        wifiSoftReset();
    } else {
        // Simple reconnect - per Espressif recommendation
        Serial.printf("  ↳ Reconnecting (attempt %d, backoff %lums)...\n", 
                      wifiMgr.reconnectAttempts, wifiMgr.reconnectBackoff);
        
        // Small delay for backoff (non-blocking would be better, but keep it simple)
        delay(min(wifiMgr.reconnectBackoff, (uint32_t)500));  // Max 500ms blocking
        
        esp_err_t ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            Serial.printf("  ⚠️ esp_wifi_connect() failed: %s\n", esp_err_to_name(ret));
        }
    }
    
    wifiMgr.lastReconnectAttempt = millis();
}

// ============================================================================
// Soft Reset: stop + start (no deinit) - Per Espressif escalation pattern
// ============================================================================
static void wifiSoftReset() {
    Serial.println("  🔄 Soft reset: stop → start → connect");
    
    gUserDisconnect = true;  // Prevent event handler from reconnecting during reset
    
    esp_wifi_disconnect();
    delay(100);
    esp_wifi_stop();
    delay(200);
    esp_wifi_start();
    delay(100);
    
    gUserDisconnect = false;
    
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        Serial.printf("  ⚠️ Soft reset connect failed: %s\n", esp_err_to_name(ret));
    }
}

// ============================================================================
// Hard Reset: Full re-init (deinit + init) - Last resort
// ============================================================================
static void wifiHardReset() {
    Serial.println("  🔄 Hard reset: full WiFi re-initialization");
    
    gUserDisconnect = true;
    
    // Deregister event handlers to avoid stale callbacks
    if (wifiMgr.eventHandlersRegistered) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler);
        wifiMgr.eventHandlersRegistered = false;
    }
    
    esp_wifi_disconnect();
    delay(100);
    esp_wifi_stop();
    delay(100);
    esp_wifi_deinit();
    delay(200);
    
    // Reset reconnect counter after hard reset
    wifiMgr.reconnectAttempts = 0;
    wifiMgr.reconnectBackoff = RECONNECT_BASE_DELAY_MS;
    
    gUserDisconnect = false;
    
    // Re-init via state machine
    wifiChangeState(WIFI_STATE_STA_CONNECTING);
}

// ============================================================================
// Event Handler - Per Espressif: actively reconnect on disconnect!
// ============================================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        Serial.println("  WiFi STA started, connecting...");
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi.connected = false;
        wifiMgr.hasIP = false;
        gTimeSynced = false;
        wifiMgr.lastDisconnectTime = millis();
        
        wifi_event_sta_disconnected_t* disconn_event = (wifi_event_sta_disconnected_t*) event_data;
        uint8_t reason = disconn_event->reason;
        
        // Get disconnect reason string and RSSI for debugging
        const char* reason_str = "Unknown";
        switch (reason) {
            case WIFI_REASON_UNSPECIFIED: reason_str = "UNSPECIFIED"; break;
            case WIFI_REASON_AUTH_EXPIRE: reason_str = "AUTH_EXPIRE"; break;
            case WIFI_REASON_AUTH_LEAVE: reason_str = "AUTH_LEAVE"; break;
            case WIFI_REASON_ASSOC_EXPIRE: reason_str = "ASSOC_EXPIRE"; break;
            case WIFI_REASON_ASSOC_TOOMANY: reason_str = "ASSOC_TOOMANY"; break;
            case WIFI_REASON_NOT_AUTHED: reason_str = "NOT_AUTHED"; break;
            case WIFI_REASON_NOT_ASSOCED: reason_str = "NOT_ASSOCED"; break;
            case WIFI_REASON_ASSOC_LEAVE: reason_str = "ASSOC_LEAVE"; break;
            case WIFI_REASON_ASSOC_NOT_AUTHED: reason_str = "ASSOC_NOT_AUTHED"; break;
            case WIFI_REASON_DISASSOC_PWRCAP_BAD: reason_str = "DISASSOC_PWRCAP_BAD"; break;
            case WIFI_REASON_DISASSOC_SUPCHAN_BAD: reason_str = "DISASSOC_SUPCHAN_BAD"; break;
            case WIFI_REASON_IE_INVALID: reason_str = "IE_INVALID"; break;
            case WIFI_REASON_MIC_FAILURE: reason_str = "MIC_FAILURE"; break;
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: reason_str = "4WAY_HANDSHAKE_TIMEOUT"; break;
            case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: reason_str = "GROUP_KEY_UPDATE_TIMEOUT"; break;
            case WIFI_REASON_IE_IN_4WAY_DIFFERS: reason_str = "IE_IN_4WAY_DIFFERS"; break;
            case WIFI_REASON_GROUP_CIPHER_INVALID: reason_str = "GROUP_CIPHER_INVALID"; break;
            case WIFI_REASON_PAIRWISE_CIPHER_INVALID: reason_str = "PAIRWISE_CIPHER_INVALID"; break;
            case WIFI_REASON_AKMP_INVALID: reason_str = "AKMP_INVALID"; break;
            case WIFI_REASON_UNSUPP_RSN_IE_VERSION: reason_str = "UNSUPP_RSN_IE_VERSION"; break;
            case WIFI_REASON_INVALID_RSN_IE_CAP: reason_str = "INVALID_RSN_IE_CAP"; break;
            case WIFI_REASON_802_1X_AUTH_FAILED: reason_str = "802_1X_AUTH_FAILED"; break;
            case WIFI_REASON_CIPHER_SUITE_REJECTED: reason_str = "CIPHER_SUITE_REJECTED"; break;
            case WIFI_REASON_BEACON_TIMEOUT: reason_str = "BEACON_TIMEOUT"; break;
            case WIFI_REASON_NO_AP_FOUND: reason_str = "NO_AP_FOUND"; break;
            case WIFI_REASON_AUTH_FAIL: reason_str = "AUTH_FAIL"; break;
            case WIFI_REASON_ASSOC_FAIL: reason_str = "ASSOC_FAIL"; break;
            case WIFI_REASON_HANDSHAKE_TIMEOUT: reason_str = "HANDSHAKE_TIMEOUT"; break;
            case WIFI_REASON_CONNECTION_FAIL: reason_str = "CONNECTION_FAIL"; break;
            default: break;
        }
        
        // Log with additional debug info
        wifi_ap_record_t ap_info;
        int8_t rssi = -100;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }
        Serial.printf("  ⚠️ WiFi disconnected (reason: %d=%s, RSSI: %d)\n", reason, reason_str, rssi);
        
        // Per Espressif: ALWAYS call esp_wifi_connect() unless user-initiated!
        // This is the KEY FIX - don't rely on auto-reconnect!
        wifiTriggerReconnect(reason);
        
        // Update state if we were connected
        if (wifiMgr.state == WIFI_STATE_STA_CONNECTED) {
            wifiChangeState(WIFI_STATE_STA_CONNECTING);
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        wifi.connected = true;
        wifiMgr.hasIP = true;
        wifiMgr.connectionAttempts = 0;
        wifiMgr.reconnectAttempts = 0;  // Reset reconnect counter on success
        wifiMgr.reconnectBackoff = RECONNECT_BASE_DELAY_MS;
        wifiMgr.wasConnected = true;
        wifiMgr.mdnsStarted = false;
        wifiMgr.initialConnectStartTime = 0;
        Serial.printf("  ✅ Got IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        wifiChangeState(WIFI_STATE_STA_CONNECTED);
    }
    // NEW: Handle IP loss (per Espressif recommendation)
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        Serial.println("  ⚠️ Lost IP address (L3 down)");
        wifiMgr.hasIP = false;
        wifi.connected = false;  // Mark as not connected (no L3)
        // WiFi L2 might still be associated, but we can't communicate
        // Trigger DHCP renewal by disconnecting and reconnecting
        if (!gUserDisconnect) {
            Serial.println("  ↳ Triggering reconnect to renew DHCP...");
            esp_wifi_disconnect();
            delay(100);
            esp_wifi_connect();
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        Serial.println("  ✅ Access Point started");
        wifi.apMode = true;
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        Serial.printf("  📱 Client connected: " MACSTR "\n", MAC2STR(event->mac));
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        Serial.printf("  📱 Client disconnected: " MACSTR "\n", MAC2STR(event->mac));
    }
}

// ============================================================================
// mDNS Management
// ============================================================================
static void startMDNS() {
    MDNS.end();
    delay(100);
    
    if (MDNS.begin("treadmill")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("✅ mDNS started: http://treadmill.local/");
    } else {
        Serial.println("❌ mDNS failed");
    }
}

// ============================================================================
// Time Synchronization
// ============================================================================
static void syncTimeFromNetwork(bool force) {
    const uint32_t now = millis();
    if (!force && (now - gLastTimeSyncAttemptMs < 60000)) {
        return;
    }
    gLastTimeSyncAttemptMs = now;

    Serial.println("Synchronizing time from NTP...");
    // Set timezone FIRST for Europe/Berlin (CET/CEST)
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    
    // Sync with NTP - get UTC time and apply timezone offset manually for TimeLib
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    const uint32_t pollDelayMs = 100;  // Shorter delay to feed watchdog more often
    const uint32_t maxAttempts = 50;   // 50 * 100ms = 5 seconds max
    for (uint32_t i = 0; i < maxAttempts; i++) {
        yield();  // Feed watchdog during NTP sync (sufficient for Arduino framework)
        time_t now_t = time(nullptr);
        if (now_t > 8 * 3600 * 2) {
            // Get local time using the timezone
            struct tm timeinfo;
            localtime_r(&now_t, &timeinfo);
            // Convert struct tm to time_t in local time for TimeLib
            time_t local_time = now_t + 3600;  // Add 1 hour for CET (will be 2 in summer)
            // Check if DST is active
            if (timeinfo.tm_isdst > 0) {
                local_time += 3600;  // Add another hour for CEST
            }
            setTime(local_time);
            gTimeSynced = true;
            Serial.printf("Time synchronized (UTC): %s", asctime(gmtime(&now_t)));
            Serial.printf("Local time (CET/CEST): %s", asctime(&timeinfo));
            return;
        }
        delay(pollDelayMs);
    }
    Serial.println("Time sync timeout");
}

// ============================================================================
// WiFi Initialization Functions
// Per Espressif: Avoid repeated deinit/init! Use soft reset first.
// ============================================================================
static void wifiInitSTA() {
    Serial.println("🔄 Initializing WiFi in Station mode...");
    
    gUserDisconnect = true;  // Prevent event handler interference during init
    
    esp_err_t ret;
    
    // Only do full cleanup if netif doesn't exist yet
    if (!wifiMgr.netif_sta) {
        // First time init or after hard reset
        esp_wifi_stop();
        delay(50);
        esp_wifi_deinit();
        delay(50);
        
        if (wifiMgr.netif_ap) {
            esp_netif_destroy(wifiMgr.netif_ap);
            wifiMgr.netif_ap = nullptr;
        }
        
        wifiMgr.netif_sta = esp_netif_create_default_wifi_sta();
        
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ret = esp_wifi_init(&cfg);
        if (ret != ESP_OK) {
            Serial.printf("❌ WiFi init failed: %s\n", esp_err_to_name(ret));
            gUserDisconnect = false;
            wifiChangeState(WIFI_STATE_AP_MODE);
            return;
        }
    }
    
    // Register event handlers (including LOST_IP!)
    if (!wifiMgr.eventHandlersRegistered) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler, NULL);
        wifiMgr.eventHandlersRegistered = true;
    }
    
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        Serial.printf("❌ Set mode failed: %s\n", esp_err_to_name(ret));
        gUserDisconnect = false;
        wifiChangeState(WIFI_STATE_AP_MODE);
        return;
    }
    
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, storedGlobals.WIFI_SSID.c_str(), 
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, storedGlobals.WIFI_PASSWORD.c_str(), 
            sizeof(wifi_config.sta.password) - 1);
    
    // WPA2/WPA3 with PMF for AUTH_EXPIRE fix
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    
    // Default listen_interval (let ESP-IDF decide based on DTIM)
    // Setting to 1 can cause MORE issues with some APs
    wifi_config.sta.listen_interval = 0;  // 0 = use default
    
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        Serial.printf("❌ Config failed: %s\n", esp_err_to_name(ret));
        gUserDisconnect = false;
        wifiChangeState(WIFI_STATE_AP_MODE);
        return;
    }
    
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        Serial.printf("❌ Start failed: %s\n", esp_err_to_name(ret));
        gUserDisconnect = false;
        wifiChangeState(WIFI_STATE_AP_MODE);
        return;
    }
    
    // Configure BLE/WiFi coexistence
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    
    // Power save: Use MIN_MODEM for BLE coexistence
    // Testing shows this is more stable than PS_NONE with BLE
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    
    gUserDisconnect = false;  // Now allow event handler to work
    
    Serial.printf("  Connecting to '%s'...\n", storedGlobals.WIFI_SSID.c_str());
    wifiMgr.connectionAttempts++;
}

static void wifiInitAP() {
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("🔄 Initializing Access Point mode");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    gUserDisconnect = true;  // Prevent reconnect attempts during mode switch
    gTimeSynced = false;
    esp_err_t ret;
    
    esp_wifi_disconnect();
    delay(50);
    esp_wifi_stop();
    delay(50);
    esp_wifi_deinit();
    delay(50);
    
    if (wifiMgr.netif_sta) {
        esp_netif_destroy(wifiMgr.netif_sta);
        wifiMgr.netif_sta = nullptr;
    }
    
    if (!wifiMgr.netif_ap) {
        wifiMgr.netif_ap = esp_netif_create_default_wifi_ap();
        if (!wifiMgr.netif_ap) {
            Serial.println("❌ Failed to create AP netif");
            return;
        }
    }
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        Serial.printf("❌ WiFi init failed: %s\n", esp_err_to_name(ret));
        return;
    }
    
    if (!wifiMgr.eventHandlersRegistered) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
        wifiMgr.eventHandlersRegistered = true;
    }
    
    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        Serial.printf("❌ Set mode failed: %s\n", esp_err_to_name(ret));
        return;
    }
    
    wifi_config_t ap_config = {};
    const char* ssid = "treadmill_rieger";
    strncpy((char*)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.ssid_hidden = 0;
    ap_config.ap.beacon_interval = 100;
    
    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) {
        Serial.printf("❌ Config failed: %s\n", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        Serial.printf("❌ Start failed: %s\n", esp_err_to_name(ret));
        return;
    }
    
    wifi.connected = false;
    wifi.apMode = true;
    wifiMgr.connectionAttempts = 0;
    wifiMgr.reconnectAttempts = 0;
    
    gUserDisconnect = false;  // Allow normal operation
    
    delay(300);
    
    Serial.println();
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("✅ ACCESS POINT ACTIVE");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("  SSID: treadmill_rieger");
    Serial.println("  IP: 192.168.4.1");
    Serial.println("  Security: Open");
    Serial.println();
    Serial.println("📱 Connect to 'treadmill_rieger' and");
    Serial.println("   browse to http://192.168.4.1");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
}

// ============================================================================
// State Machine - Clean entry-action pattern (no manual previousState setting)
// ============================================================================
static void wifiStateMachine() {
    const uint32_t now = millis();
    const uint32_t stateTime = now - wifiMgr.stateEnterTime;
    
    switch (wifiMgr.state) {
        case WIFI_STATE_INIT:
            // Entry action
            if (!wifiMgr.stateEntered) {
                wifiMgr.stateEntered = true;
                if (storedGlobals.WIFI_SSID.isEmpty()) {
                    Serial.println("📶 No WiFi credentials, starting AP mode");
                    wifiChangeState(WIFI_STATE_AP_MODE);
                } else {
                    Serial.println("📶 WiFi credentials found, connecting...");
                    wifiChangeState(WIFI_STATE_STA_CONNECTING);
                }
            }
            break;
            
        case WIFI_STATE_STA_CONNECTING:
            // Entry action
            if (!wifiMgr.stateEntered) {
                wifiMgr.stateEntered = true;
                if (wifiMgr.initialConnectStartTime == 0) {
                    wifiMgr.initialConnectStartTime = now;
                }
                wifiMgr.timeoutMessageShown = false;
                wifiInitSTA();
                wifiMgr.lastReconnectAttempt = now;
            }
            
            // Monitor for timeout (only for INITIAL connection, not reconnects)
            // Reconnects are handled by the event handler with backoff
            if (!wifiMgr.wasConnected) {
                const uint32_t totalConnectTime = now - wifiMgr.initialConnectStartTime;
                
                // Give up after 60 seconds on initial connection
                if (totalConnectTime >= 60000) {
                    if (!wifiMgr.timeoutMessageShown) {
                        Serial.printf("  ⏳ Initial connection failed after %lu seconds, switching to AP mode\n", totalConnectTime / 1000);
                        wifiMgr.timeoutMessageShown = true;
                    }
                    wifiChangeState(WIFI_STATE_AP_MODE);
                }
            }
            // For reconnects (wasConnected=true): event handler handles it with backoff
            // No timeout - keep trying indefinitely
            break;
            
        case WIFI_STATE_STA_CONNECTED:
            // Entry action
            if (!wifiMgr.stateEntered) {
                wifiMgr.stateEntered = true;
            }
            
            if (!wifiMgr.mdnsStarted) {
                startMDNS();
                wifiMgr.mdnsStarted = true;
            }
            if (!gTimeSynced && stateTime > 2000) {
                syncTimeFromNetwork(false);
            }
            break;
            
        case WIFI_STATE_AP_MODE:
            // Entry action
            if (!wifiMgr.stateEntered) {
                wifiMgr.stateEntered = true;
                wifiInitAP();
                wifiMgr.lastReconnectAttempt = now;
            }
            
            // Periodically try to reconnect to known network (every 5 minutes)
            if (wifiMgr.wasConnected && !storedGlobals.WIFI_SSID.isEmpty()) {
                const uint32_t timeSinceLastAttempt = now - wifiMgr.lastReconnectAttempt;
                if (timeSinceLastAttempt >= 300000) {
                    Serial.println("  📶 Attempting to reconnect to known network...");
                    wifiMgr.connectionAttempts = 0;
                    wifiMgr.reconnectAttempts = 0;
                    wifiChangeState(WIFI_STATE_STA_CONNECTING);
                }
            }
            break;
    }
}

// ============================================================================
// Public API
// ============================================================================
void wifiInit() {
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("📶 WiFi Manager Initializing");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    
    wifi.attempts = 0;
    wifi.connected = false;
    wifi.apMode = false;
    wifiChangeState(WIFI_STATE_INIT);
    
    Serial.println("✅ WiFi Manager ready");
}

void wifiLoop() {
    static uint32_t lastRun = 0;
    const uint32_t now = millis();
    
    if (now - lastRun < 500) return;
    lastRun = now;
    
    digitalWrite(storedGlobals.LED_PIN, wifi.connected ? HIGH : LOW);
    wifiStateMachine();
}

void wifiForceAPMode() {
    Serial.println("⚠️  Forcing AP mode...");
    gUserDisconnect = true;  // Prevent reconnect attempts
    wifiChangeState(WIFI_STATE_AP_MODE);
}
