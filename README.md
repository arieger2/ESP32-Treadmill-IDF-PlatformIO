# ESP32 Smart Treadmill Retrofit

[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Framework](https://img.shields.io/badge/framework-ESP--IDF%205.5%20%2B%20Arduino-green.svg)](https://docs.espressif.com/projects/esp-idf/)
[![License](https://img.shields.io/badge/license-MIT-lightgrey.svg)](LICENSE)

## 📖 Projekt-Übersicht

Dieses Projekt verwandelt ein älteres, nicht-smartes Laufband in ein **vollständig vernetztes Fitness-Gerät** mit Bluetooth Low Energy (BLE) Konnektivität, Web-Interface und präziser Geschwindigkeitserfassung. Der ESP32-S3 fungiert als intelligente Steuereinheit, die sowohl die Sensordaten erfasst als auch die Kommunikation mit Fitness-Apps (wie Zwift, TrainerRoad, etc.) ermöglicht.

### 🎯 Hauptfunktionen

- **Dual-Sensor Geschwindigkeitsmessung**: Hochpräzise Erfassung über Band- und Motor-Sensoren mit automatischer Auswahl
- **FTMS & RSC BLE Profile**: Vollständige Kompatibilität mit Fitness-Apps über standardisierte Bluetooth-Profile
- **Herzfrequenz-Integration**: BLE Heart Rate Client für externe HR-Monitore (Brustgurte, Smartwatches)
- **Workout-Steuerung**: Automatische Geschwindigkeits- und Neigungssteuerung basierend auf .ZWO-Workouts (Zwift-Format)
- **Web-Interface**: Vollständiges Monitoring, Konfiguration und Workout-Management über WiFi
- **Erweiterte Filterung**: EMA, Kalman und Median-Filter für stabile Geschwindigkeitswerte
- **OTA Updates**: Over-the-Air Firmware-Updates via ElegantOTA

---

## 🏗️ Technologischer Ansatz

### Hardware-Plattform

- **MCU**: ESP32-S3 (Dual-Core Xtensa LX7, 240 MHz)
- **Flash**: 16 MB (3 MB App + 13 MB LittleFS für Web-Assets)
- **RAM**: PSRAM-Unterstützung aktiviert
- **Framework**: ESP-IDF 5.5.x mit Arduino als Component

### Architektur-Prinzipien

Das Projekt folgt einem **modularen, ereignisgesteuerten Design**:

1. **Hardware-Abstraktionsschicht**: Verwendung nativer ESP-IDF Treiber (MCPWM, PCNT, GPTimer) für maximale Performance
2. **Dual-Stack Netzwerk**: Simultaner WiFi + BLE Betrieb mit ESP-IDF WiFi-Coexistence
3. **Asynchrone Webserver**: ESPAsyncWebServer für nicht-blockierende HTTP-Kommunikation
4. **ISR-sichere Datenstrukturen**: FreeRTOS-Spinlocks für kritische Abschnitte
5. **TypeScript Web-Frontend**: Typ-sichere UI-Entwicklung mit Minifizierung und Embedding

---

## 🔧 Technologische Komponenten

### 1. Geschwindigkeitssensorik (PCNT + MCPWM)

**Hardware-Ressourcen:**
- **PCNT (Pulse Counter)**: Hardware-basiertes Zählen von Pulsen ohne CPU-Last
- **MCPWM (Motor Control PWM)**: Präzise Zeitstempel-Erfassung (10 MHz Auflösung)
- **GPTimer**: Timeout-Überwachung für Stillstandserkennung (200 ms)

**Dual-Sensor System:**
```
Sensor 1 (Band):  GPIO 18 → MCPWM Group 0 + PCNT Unit 0
Sensor 2 (Motor): GPIO 19 → MCPWM Group 1 + PCNT Unit 1
```

**Betriebsmodi:**
- `SENSOR_AUTO`: Automatische Auswahl des stabileren Sensors (kleinstes RPM-Delta)
- `SENSOR_BAND`: Fest auf Band-Sensor (direktere Belt-Messung)
- `SENSOR_MOTOR`: Fest auf Motor-Sensor (höhere Pulsfrequenz = bessere Auflösung)

**Messablauf:**
1. ISR bei erstem Puls → Messfenster starten
2. PCNT zählt N Pulse (konfigurierbar, z.B. 100)
3. MCPWM erfasst präzise Zeitstempel bei jedem Puls
4. Bei Erreichen von N → Berechnung: `RPM = (Pulses / Time) * 60`
5. Konvertierung: `Speed [km/h] = RPM × Belt_Distance × 60 / 1,000,000`

**Entprellmechanismus:**
- Hardware-Debouncing via konfigurierbarer Schwellwert (Standard: 13 µs)
- Software-Validation: Max. Revolutionszeit 2000 ms

### 2. Signalfilterung

Drei Filter-Implementierungen für saubere Geschwindigkeitswerte:

#### EMA (Exponential Moving Average)
```cpp
Asymmetrische Alpha-Werte:
- Beschleunigung: α = 0.25 (moderate Glättung)
- Verzögerung:    α = 0.35 (schnellere Reaktion)
- Warm-Up:        α = 0.50 (Schnellstart)
```
**Vorteile**: Geringer Speicherbedarf, schnelle Reaktion bei Stops

#### Kalman Filter
```cpp
Parameter:
- Q (Prozessrauschen):  0.01 (Vertrauen in Modell)
- R (Messrauschen):     0.10 (Vertrauen in Sensor)
```
**Vorteile**: Optimale Fusion von Prädiktions- und Messwerten

#### Median Filter
```cpp
Fenster: 5 Samples
Algorithmus: Sortierung + Median-Extraktion
```
**Vorteile**: Robustheit gegen einzelne Ausreißer

### 3. Bluetooth Low Energy (NimBLE)

**BLE-Stack**: NimBLE (lightweight, optimiert für ESP32)

#### Implementierte GATT Services:

**FTMS (Fitness Machine Service - 0x1826)**
- **Treadmill Data (0x2ACD)**: Geschwindigkeit, Distanz, Neigung, Herzfrequenz
- **Control Point (0x2AD9)**: Steuerkommandos von Apps (Start/Pause/Speed/Incline)
- **Status (0x2ADA)**: Gerätestatus und Fehler
- **Feature (0x2ACC)**: Fähigkeiten-Deklaration (Unterstützte Features)

**RSC (Running Speed and Cadence - 0x1814)**
- **Measurement (0x2A53)**: Instant Speed, Cadence, Stride Length
- **Feature (0x2A54)**: RSC-Fähigkeiten

**Heart Rate Service (0x180D)**
- **Client-Modus**: Scannen und Verbinden zu externen HR-Monitoren
- **Server-Modus**: Weiterleitung der HR-Daten an Fitness-Apps
- **RR-Intervall Support**: Vollständige HRV-Daten (Heart Rate Variability)

**Features:**
- MTU: 247 Bytes (optimiert für größere Datenpakete)
- Simultaner Server + Client Betrieb
- Connection-Callbacks für Zustandsmanagement
- Coexistence mit WiFi (ESP-IDF WiFi-BT-Coexistence Manager)

### 4. WiFi-Netzwerk (ESP-IDF WiFi Stack)

**State Machine Implementation:**
```
INIT → STA_CONNECTING → STA_CONNECTED → [RUNNING]
  ↓                                            ↓
AP_MODE ←───────────── (Timeout) ────────────┘
```

**Reconnect-Strategie (nach Espressif Best Practices):**
- Exponential Backoff: 1s → 30s max
- Soft Reset nach 3 Fehlversuchen (Neustart WiFi-Stack)
- Hard Reset nach 10 Fehlversuchen (vollständiger Radio-Reset)
- User-Disconnect-Flag verhindert ungewollte Reconnects

**Fallback AP-Modus:**
```
SSID: ESP32-Treadmill-Setup
IP:   192.168.4.1
```

**mDNS (Multicast DNS):**
```
Hostname: treadmill.local
Service:  _http._tcp (Port 80)
```

**SNTP Time Sync:**
- NTP-Server: `pool.ntp.org`
- Automatische Synchronisation bei WiFi-Verbindung
- Zeitstempel für Workout-Logs und Boot-Logs

### 5. Web-Interface (Async WebServer)

**Architektur:**
- **Backend**: ESPAsyncWebServer (nicht-blockierend)
- **Frontend**: TypeScript → Transpiliert & Minifiziert → Embedded als C-Header
- **Build-Pipeline**: `npm run build` → `pre_build.py` → `.h` Files

**Drei UI-Seiten:**

#### Monitor (`/`)
- Echtzeit-Geschwindigkeit, Pace, Distanz, RPM
- Live HR + RR-Intervall Diagramme (Chart.js)
- System-Status (WiFi, BLE, Sensor-Auswahl)
- Speed Offset Kontrolle

#### Settings (`/settings`)
- WiFi-Konfiguration (SSID/Passwort)
- Sensor-Pins und Kalibrierung
- Filter-Auswahl (EMA/Kalman/Median)
- Sensor-Mode (Auto/Band/Motor)
- Geschwindigkeits-Steuerungs-Parameter
- Boot-Log Anzeige

#### Workout (`/workout`)
- .ZWO File Upload (Zwift Workout Format)
- Workout-Visualisierung (Segmente mit Power/Speed)
- Play/Pause/Stop Kontrolle
- Automatische Laufband-Steuerung während Workout

**API-Endpunkte:**
```
GET  /api/monitor        - Alle Monitoring-Werte (JSON)
GET  /api/hr-series      - Herzfrequenz-Zeitreihe
GET  /api/rr-series      - RR-Intervall-Zeitreihe
GET  /api/bootlog        - System-Boot-Log
POST /upload-workout     - ZWO-File Upload
POST /api/settings/save  - Konfiguration speichern
```

### 6. Workout-Management

**ZWO-Parser (Zwift Workout Format):**
```xml
<workout_file>
  <thresholdSecPerKm>240</thresholdSecPerKm>
  <workout>
    <SteadyState Duration="600" Power="0.75" />
    <IntervalsT Repeat="5" OnDuration="120" OffDuration="60" 
                OnPower="1.2" OffPower="0.6" />
  </workout>
</workout_file>
```

**Unterstützte Workout-Typen:**
- `SteadyState`: Konstante Intensität
- `IntervalsT`: Zeitbasierte Intervalle
- `Warmup/Cooldown`: Rampen (nicht vollständig implementiert für Laufband)
- `FreeRide`: Freies Tempo

**Automatische Steuerung:**
1. Power → Speed Konvertierung (basierend auf `thresholdSecPerKm`)
2. GPIO-Pulssimulation für Speed Up/Down Buttons
3. Inertia-Kompensation (Überschwingen berücksichtigen)
4. State Machine: Idle → Running → Paused → Completed/Error

**Steuerungs-Algorithmus:**
```cpp
ΔSpeed = Target - Current
Button_Press_Duration = ΔSpeed / Speed_Rate
+ Overshoot_Compensation
+ Inertia_Delay
```

### 7. Persistente Konfiguration (NVS)

**NVS (Non-Volatile Storage) Namespace**: `TreadmillTc`

**Gespeicherte Parameter:**
```cpp
- WiFi-Credentials (SSID, Password)
- BLE Device Name
- GPIO-Pin-Assignments
- Sensor-Kalibrierung (Belt Distance, Pulses per Rev)
- Filter-Einstellungen
- Speed Control Parameters (Rates, Delays)
- Sensor Source Mode
```

**API-Funktionen:**
- `loadConfig()` - Startup Load mit Defaults
- `saveConfig()` - Atomic Write mit Validation
- `resetToDefaults()` - Factory Reset

### 8. OTA Updates (ElegantOTA)

**Features:**
- Web-basiertes Firmware-Upload
- MD5-Checksummen-Validierung
- Partition-Table-Aware (app0 Partition)
- Rollback bei fehlgeschlagenem Update

**Zugriff**: `http://treadmill.local/update`

---

## 📋 Funktionsübersicht

| Funktion | Implementiert | Technologie |
|----------|---------------|-------------|
| **Geschwindigkeitsmessung** | ✅ | PCNT + MCPWM + GPTimer |
| **Dual-Sensor System** | ✅ | Hardware-Multiplexing |
| **Signal-Filterung** | ✅ | EMA, Kalman, Median |
| **BLE FTMS Server** | ✅ | NimBLE GATT |
| **BLE RSC Server** | ✅ | NimBLE GATT |
| **BLE HR Client** | ✅ | NimBLE Central |
| **WiFi STA + AP** | ✅ | ESP-IDF WiFi Stack |
| **Async WebServer** | ✅ | ESPAsyncWebServer |
| **TypeScript UI** | ✅ | TSC + Terser |
| **ZWO Workout Parser** | ✅ | Custom XML Parser |
| **Auto Speed Control** | ✅ | GPIO Pulse Simulation |
| **HR/RR Visualization** | ✅ | Chart.js |
| **OTA Updates** | ✅ | ElegantOTA |
| **Boot Log System** | ✅ | LittleFS Persistence |
| **NVS Config** | ✅ | ESP-IDF NVS API |
| **mDNS** | ✅ | ESP-IDF mDNS |
| **SNTP Time Sync** | ✅ | ESP-IDF SNTP |

---

## 🚀 Installation & Setup

### Voraussetzungen

**Hardware:**
- ESP32-S3 Development Board (min. 8MB Flash, PSRAM empfohlen)
- Hall-Sensor oder optischer Sensor für Band-Messung
- Optionaler Motor-Sensor (höhere Auflösung)
- Laufband mit GPIO-zugänglichen Steuerungstasten

**Software:**
- [PlatformIO](https://platformio.org/) (empfohlen) oder ESP-IDF 5.5+
- Node.js & npm (für Web-UI Build)
- Git

### Build-Anleitung

1. **Repository klonen:**
   ```bash
   git clone <repository-url>
   cd ESP32-Treadmill-IDF-PlatformIO
   ```

2. **Dependencies installieren:**
   ```bash
   # ESP-IDF Components werden automatisch von idf_component.yml geladen
   # Arduino Libraries via platformio.ini
   
   # Web-UI Dependencies
   npm install
   ```

3. **Web-UI bauen:**
   ```bash
   npm run build
   # Generiert minifizierte JS-Files in build/
   # pre_build.py konvertiert diese automatisch zu C-Headers
   ```

4. **Firmware kompilieren:**
   ```bash
   # PlatformIO
   pio run -e esp32-s3-16mb
   
   # ESP-IDF
   idf.py build
   ```

5. **Flash & Monitor:**
   ```bash
   # PlatformIO
   pio run -e esp32-s3-16mb -t upload -t monitor
   
   # ESP-IDF
   idf.py flash monitor
   ```

### Erste Konfiguration

1. **WiFi Setup (Fallback AP):**
   - Bei erstem Start oder fehlgeschlagenem WiFi → AP-Modus
   - Verbinde mit `ESP32-Treadmill-Setup`
   - Öffne `http://192.168.4.1/settings`
   - Setze SSID/Passwort und speichere
   - ESP32 startet neu und verbindet

2. **Sensor-Kalibrierung:**
   - Öffne `http://treadmill.local/settings`
   - Setze `Belt Distance` (mm pro Umdrehung)
   - Setze `Pulses per Revolution` (Band/Motor)
   - Optional: Justiere Debounce-Schwellwert
   - Teste im Monitor-Tab

3. **BLE-Name anpassen:**
   - Settings → `BLE Device Name`
   - Speichern → Neustart erforderlich

---

## 🔌 Hardware-Verdrahtung

### Minimale Konfiguration

```
ESP32-S3              Treadmill
-------              ---------
GPIO 18  ────────►   Band-Sensor (Hall/Optical)
GPIO 19  ────────►   Motor-Sensor (optional)
GND      ────────►   Common Ground
```

### Erweiterte Konfiguration (mit Steuerung)

```
ESP32-S3              Treadmill
-------              ---------
GPIO 18  ────────►   Band-Sensor
GPIO 19  ────────►   Motor-Sensor
GPIO 10  ────────►   Speed Up Button (parallel)
GPIO 11  ────────►   Speed Down Button (parallel)
GPIO 12  ────────►   Incline Up Button (optional)
GPIO 13  ────────►   Incline Down Button (optional)
GPIO 2   ────────►   Status LED
GND      ────────►   Common Ground
```

**⚠️ Wichtig:** GPIO-Pins sind konfigurierbar via Web-Interface!

---

## 📊 Datenfluss-Diagramm

```
┌─────────────┐
│   Sensoren  │ (Band + Motor)
└──────┬──────┘
       │ Pulses
       ▼
┌──────────────────┐
│  PCNT + MCPWM    │ (Hardware Counting + Timestamps)
│  + GPTimer       │ (Timeout Watchdog)
└──────┬───────────┘
       │ Raw RPM
       ▼
┌──────────────────┐
│  Filter (EMA/    │ (Signal Processing)
│  Kalman/Median)  │
└──────┬───────────┘
       │ Filtered Speed
       ├─────────────────┐
       ▼                 ▼
┌─────────────┐   ┌──────────────┐
│  BLE FTMS   │   │  Web API     │
│  + RSC      │   │  (HTTP JSON) │
└─────────────┘   └──────────────┘
       │                 │
       ▼                 ▼
┌─────────────┐   ┌──────────────┐
│ Fitness App │   │  Web Browser │
│ (Zwift etc.)│   │  (Monitor UI)│
└─────────────┘   └──────────────┘
```

---

## 🧪 Test-Modus

**Funktion**: Simuliert Laufband-Bewegung ohne physische Sensoren

**Aktivierung**: Web-Interface → Monitor → "Enable Test Data"

**Verhalten:**
- Generiert synthetische RPM-Werte (100 RPM baseline)
- Simuliert Beschleunigung/Verzögerung
- Nützlich für BLE-App-Testing ohne Laufband

---

## 🐛 Debugging & Logging

### Serial-Monitor

```cpp
Boot Sequence:
=== Init Status ===
  NVS:           ✓
  Config:        ✓
  BLE FTMS:      ✓
  BLE RSC:       ✓
  BLE HR Server: ✓
  BLE Adv:       ✓
  BLE HR Client: ✓
  WiFi:          ✓
  Web Server:    ✓
  Tachometer:    ✓
===================
```

### Boot-Log (Web-UI)

`/settings` → "Boot Log" zeigt:
- Initialisierungs-Schritte
- Error Messages
- WiFi-Verbindungs-Historie
- System Restarts

### ESP-IDF Logs

```bash
# Erhöhe Log-Level für spezifische Komponenten
idf.py menuconfig
→ Component config → Log output → Default log verbosity
```

---

## 📝 Bekannte Einschränkungen

1. **Neigungssteuerung**: Noch nicht vollständig implementiert (GPIO-Pins reserviert)
2. **Kalibrations-Workflow**: Manuelle Einstellung der Belt Distance erforderlich
3. **Workout-Rampen**: Linear Ramps (Warmup/Cooldown) für Laufband vereinfacht
4. **WiFi-BLE Coexistence**: Kann bei hoher Last zu Latenz führen (ESP-IDF Coexistence Manager aktiv)

---

## 🛠️ Anpassung & Erweiterung

### Neue Web-Seite hinzufügen

1. **TypeScript entwickeln**: `typescript-mypage/`
2. **Build-Skript**: `package.json` → neues Target
3. **Header-Generierung**: `create_js_headers.py` erweitern
4. **Routes registrieren**: `web_interface.cpp` → `initWebServer()`

### Neuer BLE Service

1. **UUIDs definieren**: `tacho_config.h`
2. **Service erstellen**: `ble_ftms_init.cpp`
3. **Callbacks**: Separate Callback-Datei (Naming: `ble_<service>_callbacks.cpp`)

### Custom Filter

1. **Interface**: `tacho_filters.h` → neue Klasse
2. **Implementierung**: `tacho_filters.cpp`
3. **Integration**: `sensor_speed_distance.cpp` → Filter-Switch

---

## 📚 Verwendete Bibliotheken

| Bibliothek | Version | Zweck |
|------------|---------|-------|
| ESP-IDF | 5.5+ | Core Framework |
| Arduino-ESP32 | latest | Arduino Compatibility Layer |
| NimBLE-Arduino | 2.3.7 | BLE Stack |
| ArduinoJson | 7.4.2 | JSON Parser/Generator |
| ESPAsyncWebServer | 3.9.3 | Async HTTP Server |
| AsyncTCP | 3.4.9 | Async TCP Library |
| ElegantOTA | 3.1.7 | OTA Updates |
| LittleFS | * | Filesystem (via idf_component.yml) |

---

## 🤝 Beiträge

Contributions sind willkommen! 

**Prioritäten:**
- Neigungssteuerungs-Implementierung
- Auto-Kalibrierungs-Wizard
- Mobile-Responsive Web-UI
- Docker-basierte Build-Pipeline

**Entwicklungs-Workflow:**
1. Feature-Branch erstellen
2. Code + Tests implementieren
3. Pull Request mit Beschreibung

---

## 📄 Lizenz

Dieses Projekt ist lizenziert unter der MIT License - siehe [LICENSE](LICENSE) Datei.

---

## 🙏 Danksagungen

- Espressif Systems für ESP-IDF und umfassende Dokumentation
- NimBLE-Arduino Contributors
- OpenAI ChatGPT für Code-Reviews und Optimierungsvorschläge
- Zwift für das offene .ZWO-Format

---

## 📞 Support & Kontakt

- **Issues**: GitHub Issues für Bug-Reports und Feature-Requests
- **Diskussionen**: GitHub Discussions für allgemeine Fragen

---

**⚙️ Made with ❤️ and ESP32-S3**
