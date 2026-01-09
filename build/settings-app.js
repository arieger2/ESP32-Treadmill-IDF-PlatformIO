/**
 * @file 0-config.ts
 * @brief Settings page configuration and types
 */
var SettingsApp;
(function (SettingsApp) {
    SettingsApp.CONFIG = {
        STATUS_DISPLAY_TIME_MS: 5000,
        CALIBRATION_POLL_INTERVAL_MS: 1000,
        REBOOT_WAIT_TIME_MS: 30000,
    };
})(SettingsApp || (SettingsApp = {}));
/**
 * @file 1-common.ts
 * @brief Common utilities for settings page
 */
var SettingsApp;
(function (SettingsApp) {
    /**
     * Display status message with auto-hide
     */
    function showStatus(msg, err) {
        if (err === void 0) { err = false; }
        var s = document.getElementById('status');
        if (!s)
            return;
        s.textContent = msg;
        s.className = 'status ' + (err ? 'error' : 'success');
        s.style.display = 'block';
        setTimeout(function () { return s.style.display = 'none'; }, SettingsApp.CONFIG.STATUS_DISPLAY_TIME_MS);
    }
    SettingsApp.showStatus = showStatus;
    /**
     * Navigate to OTA update page
     */
    function goToOTA() {
        sessionStorage.setItem('fromSettings', '1');
        window.location.href = '/update';
    }
    SettingsApp.goToOTA = goToOTA;
    /**
     * Initialize select dropdowns with data-value attributes
     * The values are set via data-value in HTML (from template processor)
     */
    function initializeSelects() {
        var selects = document.querySelectorAll('select[data-value]');
        selects.forEach(function (sel) {
            var select = sel;
            var value = select.dataset.value;
            if (value && value.indexOf('%') === -1) { // Skip if placeholder not replaced
                select.value = value;
            }
        });
    }
    SettingsApp.initializeSelects = initializeSelects;
})(SettingsApp || (SettingsApp = {}));
/**
 * @file 2-form.ts
 * @brief Form submission and settings management
 */
var SettingsApp;
(function (SettingsApp) {
    /**
     * Validate form data before submission
     */
    function validateFormData(data) {
        // WiFi validation
        var ssid = data['wifiSSID'] || '';
        var pass = data['wifiPassword'] || '';
        if (ssid.length === 0 || ssid.length > 32) {
            return { valid: false, error: 'WiFi SSID must be 1-32 characters', fieldId: 'wifiSSID' };
        }
        if (pass.length > 64) {
            return { valid: false, error: 'WiFi password must be max 64 characters', fieldId: 'wifiPassword' };
        }
        // BLE Device Name validation
        var bleName = data['bleDeviceName'] || '';
        if (bleName.length === 0 || bleName.length > 20) {
            return { valid: false, error: 'BLE Device Name must be 1-20 characters', fieldId: 'bleDeviceName' };
        }
        // Pin validation - collect all pins
        var pinFields = ['interruptPin', 'motorInterruptPin', 'speedUpPin', 'speedDownPin', 'inclineUpPin', 'inclineDownPin'];
        var pins = pinFields.map(function (f) { return parseInt(data[f]); });
        // Check pin ranges (0-39 for ESP32)
        for (var i = 0; i < pins.length; i++) {
            if (isNaN(pins[i]) || pins[i] < 0 || pins[i] > 39) {
                return { valid: false, error: "Pin ".concat(pinFields[i], " must be between 0 and 39"), fieldId: pinFields[i] };
            }
        }
        // Check for duplicate pins
        var pinSet = new Set(pins);
        if (pinSet.size !== pins.length) {
            // Find the duplicate
            for (var i = 0; i < pins.length; i++) {
                for (var j = i + 1; j < pins.length; j++) {
                    if (pins[i] === pins[j]) {
                        return { valid: false, error: "Duplicate pin ".concat(pins[i], " detected in ").concat(pinFields[i], " and ").concat(pinFields[j]), fieldId: pinFields[j] };
                    }
                }
            }
        }
        // Frequency validation
        var speedFreq = parseInt(data['speedIncDecFreq']);
        if (isNaN(speedFreq) || speedFreq < 10 || speedFreq > 1000) {
            return { valid: false, error: 'Speed Inc/Dec frequency must be 10-1000 ms', fieldId: 'speedIncDecFreq' };
        }
        var testFreq = parseInt(data['testdataFreq']);
        if (isNaN(testFreq) || testFreq < 1 || testFreq > 1000) {
            return { valid: false, error: 'Test data frequency must be 1-1000 ms', fieldId: 'testdataFreq' };
        }
        // Belt & sensor validation
        var beltDist = parseInt(data['beltDistance']);
        if (isNaN(beltDist) || beltDist < 100 || beltDist > 500) {
            return { valid: false, error: 'Belt distance must be 100-500 mm', fieldId: 'beltDistance' };
        }
        var debounce = parseInt(data['debounceThreshold']);
        if (isNaN(debounce) || debounce < 1 || debounce > 13) {
            return { valid: false, error: 'Debounce threshold must be 1-13 µs (PCNT hardware limit)', fieldId: 'debounceThreshold' };
        }
        var maxRevTime = parseInt(data['maxRevolutionTime']);
        if (isNaN(maxRevTime) || maxRevTime < 100 || maxRevTime > 500000) {
            return { valid: false, error: 'Max revolution time must be 100-500000 ms', fieldId: 'maxRevolutionTime' };
        }
        var pulses = parseInt(data['pulsesPerRev']);
        if (isNaN(pulses) || pulses < 1 || pulses > 128) {
            return { valid: false, error: 'Pulses per revolution must be 1-128', fieldId: 'pulsesPerRev' };
        }
        var bandMult = parseInt(data['bandPulseMultiplier']);
        if (isNaN(bandMult) || bandMult < 1 || bandMult > 100) {
            return { valid: false, error: 'Band pulse multiplier must be 1-100', fieldId: 'bandPulseMultiplier' };
        }
        var motorPulses = parseInt(data['motorPulsesPerRev']);
        if (isNaN(motorPulses) || motorPulses < 1 || motorPulses > 256) {
            return { valid: false, error: 'Motor pulses per revolution must be 1-256', fieldId: 'motorPulsesPerRev' };
        }
        var motorMult = parseInt(data['motorPulseMultiplier']);
        if (isNaN(motorMult) || motorMult < 1 || motorMult > 100) {
            return { valid: false, error: 'Motor pulse multiplier must be 1-100', fieldId: 'motorPulseMultiplier' };
        }
        var ratio = parseFloat(data['motorToBeltRatio']);
        if (isNaN(ratio) || ratio <= 0.01 || ratio >= 10.0) {
            return { valid: false, error: 'Motor to belt ratio must be 0.01-10.0', fieldId: 'motorToBeltRatio' };
        }
        return { valid: true }; // All validation passed
    }
    /**
     * Handle settings form submission
     */
    function handleFormSubmit(e) {
        e.preventDefault();
        var form = e.target;
        var data = {};
        var formData = new FormData(form);
        formData.forEach(function (v, k) { return data[k] = v.toString(); });
        // Validate before sending
        var validation = validateFormData(data);
        if (!validation.valid) {
            SettingsApp.showStatus('❌ ' + validation.error, true);
            // Focus the problematic field if specified
            if (validation.fieldId) {
                var field = document.getElementById(validation.fieldId);
                if (field) {
                    field.focus();
                    field.select();
                    field.scrollIntoView({ behavior: 'smooth', block: 'center' });
                }
            }
            return;
        }
        // Send to server
        fetch('/api/settings', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data)
        })
            .then(function (r) { return r.json(); })
            .then(function (j) {
            if (j.success) {
                SettingsApp.showStatus('✅ Settings saved successfully! Most changes are active immediately. Reboot required only for WiFi/Pin changes.');
            }
            else {
                SettingsApp.showStatus('❌ Error: ' + j.message, true);
            }
        })
            .catch(function (err) { return SettingsApp.showStatus('❌ Network Error: ' + err.message, true); });
    }
    SettingsApp.handleFormSubmit = handleFormSubmit;
    /**
     * Load default settings
     */
    function loadDefaults() {
        if (!confirm('Load default settings? This will overwrite all current settings.'))
            return;
        fetch('/api/settings/defaults', { method: 'POST' })
            .then(function (r) { return r.json(); })
            .then(function (j) {
            if (j.success) {
                SettingsApp.showStatus('Default settings loaded! Refreshing page...');
                setTimeout(function () { return location.reload(); }, 1500);
            }
            else {
                SettingsApp.showStatus('Error loading defaults: ' + j.message, true);
            }
        })
            .catch(function (err) { return SettingsApp.showStatus('Error: ' + err.message, true); });
    }
    SettingsApp.loadDefaults = loadDefaults;
    /**
     * Perform factory reset
     */
    function factoryReset() {
        if (!confirm('FACTORY RESET: This will erase ALL settings and reboot the device. Are you sure?'))
            return;
        if (!confirm('This cannot be undone. Continue with factory reset?'))
            return;
        fetch('/api/factory-reset', { method: 'POST' })
            .then(function (r) { return r.json(); })
            .then(function (j) {
            if (j.success) {
                SettingsApp.showStatus('Factory reset complete! Device will reboot...');
                setTimeout(function () { return location.href = '/'; }, 5000);
            }
            else {
                SettingsApp.showStatus('Error during factory reset: ' + j.message, true);
            }
        })
            .catch(function (err) { return SettingsApp.showStatus('Error: ' + err.message, true); });
    }
    SettingsApp.factoryReset = factoryReset;
    /**
     * Reboot the device
     */
    function rebootDevice() {
        if (!confirm('Reboot the device? This will restart the ESP32.'))
            return;
        fetch('/api/reboot', { method: 'POST' })
            .then(function () {
            SettingsApp.showStatus('Device rebooting... Please wait 30 seconds before refreshing.');
            setTimeout(function () { return location.href = '/'; }, SettingsApp.CONFIG.REBOOT_WAIT_TIME_MS);
        })
            .catch(function () {
            SettingsApp.showStatus('Reboot initiated. Please wait 30 seconds.');
            setTimeout(function () { return location.href = '/'; }, SettingsApp.CONFIG.REBOOT_WAIT_TIME_MS);
        });
    }
    SettingsApp.rebootDevice = rebootDevice;
    /**
     * View boot log in modal popup
     */
    function viewBootLog() {
        // Create modal overlay
        var modal = document.createElement('div');
        modal.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.7);z-index:9999;display:flex;align-items:center;justify-content:center;';
        // Create modal content
        var content = document.createElement('div');
        content.style.cssText = 'background:#fff;padding:20px;border-radius:8px;max-width:90%;max-height:80%;overflow:auto;box-shadow:0 4px 20px rgba(0,0,0,0.3);';
        // Add header
        var header = document.createElement('div');
        header.style.cssText = 'display:flex;justify-content:space-between;align-items:center;margin-bottom:15px;border-bottom:2px solid #ddd;padding-bottom:10px;';
        header.innerHTML = '<h2 style="margin:0;">Boot Log</h2><button id="closeLogModal" style="padding:5px 15px;cursor:pointer;">Close</button>';
        content.appendChild(header);
        // Add loading message
        var logArea = document.createElement('pre');
        logArea.style.cssText = 'background:#1e1e1e;color:#d4d4d4;padding:15px;border-radius:4px;overflow:auto;max-height:60vh;font-family:Consolas,Monaco,monospace;font-size:12px;line-height:1.4;';
        logArea.textContent = 'Loading boot log...';
        content.appendChild(logArea);
        modal.appendChild(content);
        document.body.appendChild(modal);
        // Close button handler
        document.getElementById('closeLogModal').onclick = function () {
            document.body.removeChild(modal);
        };
        // Click outside to close
        modal.onclick = function (e) {
            if (e.target === modal) {
                document.body.removeChild(modal);
            }
        };
        // Fetch boot log
        fetch('/api/bootlog')
            .then(function (r) { return r.text(); })
            .then(function (log) {
            logArea.textContent = log;
        })
            .catch(function (err) {
            logArea.textContent = 'Error loading boot log: ' + err.message;
            logArea.style.color = '#ff6b6b';
        });
    }
    SettingsApp.viewBootLog = viewBootLog;
})(SettingsApp || (SettingsApp = {}));
/**
 * @file 3-calibration.ts
 * @brief Auto-calibration functionality
 */
var SettingsApp;
(function (SettingsApp) {
    var calibrationInterval = null;
    /**
     * Start calibration process
     */
    function startCalibration() {
        var statusEl = document.getElementById('calibrationStatus');
        if (!statusEl)
            return;
        statusEl.textContent = 'Starting calibration...';
        statusEl.className = 'status';
        statusEl.style.display = 'block';
        fetch('/api/calibrate/start', { method: 'POST' })
            .then(function (r) { return r.json(); })
            .then(function (j) {
            if (j.success) {
                statusEl.textContent = 'Calibration started! Monitoring progress...';
                // Poll status every second
                if (calibrationInterval)
                    clearInterval(calibrationInterval);
                calibrationInterval = window.setInterval(updateCalibrationStatus, SettingsApp.CONFIG.CALIBRATION_POLL_INTERVAL_MS);
            }
            else {
                statusEl.textContent = 'Error: ' + (j.message || 'Could not start calibration');
                statusEl.className = 'status error';
            }
        })
            .catch(function (err) {
            statusEl.textContent = 'Error: ' + err.message;
            statusEl.className = 'status error';
        });
    }
    SettingsApp.startCalibration = startCalibration;
    /**
     * Update calibration status (called by interval)
     */
    function updateCalibrationStatus() {
        fetch('/api/calibrate/status')
            .then(function (r) { return r.json(); })
            .then(function (j) {
            var _a, _b, _c, _d;
            var statusEl = document.getElementById('calibrationStatus');
            if (!statusEl)
                return;
            if (j.state === 'done') {
                statusEl.textContent = "\u2713 Calibration complete! UP: ".concat((_a = j.speedUpRate) === null || _a === void 0 ? void 0 : _a.toFixed(3), " km/h/s, DOWN: ").concat((_b = j.speedDownRate) === null || _b === void 0 ? void 0 : _b.toFixed(3), " km/h/s");
                statusEl.className = 'status success';
                if (calibrationInterval) {
                    clearInterval(calibrationInterval);
                    calibrationInterval = null;
                }
                // Update input fields with new values
                var speedUpInput = document.getElementById('speedUpRate');
                var speedDownInput = document.getElementById('speedDownRate');
                if (speedUpInput && j.speedUpRate !== undefined) {
                    speedUpInput.value = j.speedUpRate.toFixed(3);
                }
                if (speedDownInput && j.speedDownRate !== undefined) {
                    speedDownInput.value = j.speedDownRate.toFixed(3);
                }
            }
            else if (j.state === 'error') {
                statusEl.textContent = '✗ Calibration failed: ' + (j.message || 'Unknown error');
                statusEl.className = 'status error';
                if (calibrationInterval) {
                    clearInterval(calibrationInterval);
                    calibrationInterval = null;
                }
            }
            else {
                // Show detailed progress with speeds
                var progress = '';
                switch (j.state) {
                    case 'starting_up':
                        progress = '⏳ Preparing UP calibration...';
                        break;
                    case 'pressing_up':
                        progress = '⬆️ Pressing UP button...';
                        break;
                    case 'waiting_up':
                        progress = "\u23F1\uFE0F Waiting for UP stabilization (".concat(((_c = j.midSpeed) === null || _c === void 0 ? void 0 : _c.toFixed(1)) || '?', " km/h)...");
                        break;
                    case 'starting_down':
                        progress = '⏳ Preparing DOWN calibration...';
                        break;
                    case 'pressing_down':
                        progress = '⬇️ Pressing DOWN button...';
                        break;
                    case 'waiting_down':
                        progress = "\u23F1\uFE0F Waiting for DOWN stabilization (".concat(((_d = j.endSpeed) === null || _d === void 0 ? void 0 : _d.toFixed(1)) || '?', " km/h)...");
                        break;
                    default: progress = 'Status: ' + j.state;
                }
                if (j.message)
                    progress += ' - ' + j.message;
                statusEl.textContent = progress;
                statusEl.className = 'status';
            }
        })
            .catch(function (err) {
            console.error('Status check failed:', err);
        });
    }
})(SettingsApp || (SettingsApp = {}));
/**
 * @file 4-main.ts
 * @brief Settings page initialization
 */
var SettingsApp;
(function (SettingsApp) {
    // Expose functions globally IMMEDIATELY for onclick handlers in HTML
    // This must happen before DOMContentLoaded to prevent race conditions
    window.SettingsApp = SettingsApp;
    document.addEventListener('DOMContentLoaded', function () {
        // Initialize select dropdowns
        SettingsApp.initializeSelects();
        // Attach form submit handler
        var form = document.getElementById('settingsForm');
        if (form) {
            form.addEventListener('submit', SettingsApp.handleFormSubmit);
        }
    });
})(SettingsApp || (SettingsApp = {}));
