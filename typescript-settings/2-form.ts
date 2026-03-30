/**
 * @file 2-form.ts
 * @brief Form submission and settings management
 */

namespace SettingsApp {
  /**
   * Validation result with optional field ID to focus
   */
  interface ValidationResult {
    valid: boolean;
    error?: string;
    fieldId?: string;
  }

  /**
   * Validate form data before submission
   */
  function validateFormData(data: Record<string, string>): ValidationResult {
    // WiFi validation
    const ssid = data['wifiSSID'] || '';
    const pass = data['wifiPassword'] || '';
    if (ssid.length === 0 || ssid.length > 32) {
      return { valid: false, error: 'WiFi SSID must be 1-32 characters', fieldId: 'wifiSSID' };
    }
    if (pass.length > 64) {
      return { valid: false, error: 'WiFi password must be max 64 characters', fieldId: 'wifiPassword' };
    }

    // BLE Device Name validation
    const bleName = data['bleDeviceName'] || '';
    if (bleName.length === 0 || bleName.length > 20) {
      return { valid: false, error: 'BLE Device Name must be 1-20 characters', fieldId: 'bleDeviceName' };
    }

    // Pin validation - collect all pins
    const pinFields = ['interruptPin', 'motorInterruptPin', 'speedUpPin', 'speedDownPin', 'inclineUpPin', 'inclineDownPin'];
    const pins = pinFields.map(f => parseInt(data[f]));

    // Check pin ranges (0-39 for ESP32)
    for (let i = 0; i < pins.length; i++) {
      if (isNaN(pins[i]) || pins[i] < 0 || pins[i] > 39) {
        return { valid: false, error: `Pin ${pinFields[i]} must be between 0 and 39`, fieldId: pinFields[i] };
      }
    }

    // Check for duplicate pins
    const pinSet = new Set(pins);
    if (pinSet.size !== pins.length) {
      // Find the duplicate
      for (let i = 0; i < pins.length; i++) {
        for (let j = i + 1; j < pins.length; j++) {
          if (pins[i] === pins[j]) {
            return { valid: false, error: `Duplicate pin ${pins[i]} detected in ${pinFields[i]} and ${pinFields[j]}`, fieldId: pinFields[j] };
          }
        }
      }
    }

    // Frequency validation
    const testFreq = parseInt(data['testdataFreq']);
    if (isNaN(testFreq) || testFreq < 1 || testFreq > 1000) {
      return { valid: false, error: 'Test data frequency must be 1-1000 ms', fieldId: 'testdataFreq' };
    }

    // Belt & sensor validation
    const beltDist = parseInt(data['beltDistance']);
    if (isNaN(beltDist) || beltDist < 100 || beltDist > 500) {
      return { valid: false, error: 'Belt distance must be 100-500 mm', fieldId: 'beltDistance' };
    }

    const debounce = parseInt(data['debounceThreshold']);
    if (isNaN(debounce) || debounce < 1 || debounce > 13) {
      return { valid: false, error: 'Debounce threshold must be 1-13 µs (PCNT hardware limit)', fieldId: 'debounceThreshold' };
    }

    const maxRevTime = parseInt(data['maxRevolutionTime']);
    if (isNaN(maxRevTime) || maxRevTime < 100 || maxRevTime > 500000) {
      return { valid: false, error: 'Max revolution time must be 100-500000 ms', fieldId: 'maxRevolutionTime' };
    }

    const pulses = parseInt(data['pulsesPerRev']);
    if (isNaN(pulses) || pulses < 1 || pulses > 128) {
      return { valid: false, error: 'Pulses per revolution must be 1-128', fieldId: 'pulsesPerRev' };
    }

    const bandMult = parseInt(data['bandPulseMultiplier']);
    if (isNaN(bandMult) || bandMult < 1 || bandMult > 100) {
      return { valid: false, error: 'Band pulse multiplier must be 1-100', fieldId: 'bandPulseMultiplier' };
    }

    const motorPulses = parseInt(data['motorPulsesPerRev']);
    if (isNaN(motorPulses) || motorPulses < 1 || motorPulses > 256) {
      return { valid: false, error: 'Motor pulses per revolution must be 1-256', fieldId: 'motorPulsesPerRev' };
    }

    const motorMult = parseInt(data['motorPulseMultiplier']);
    if (isNaN(motorMult) || motorMult < 1 || motorMult > 100) {
      return { valid: false, error: 'Motor pulse multiplier must be 1-100', fieldId: 'motorPulseMultiplier' };
    }

    const ratio = parseFloat(data['motorToBeltRatio']);
    if (isNaN(ratio) || ratio <= 0.01 || ratio >= 10.0) {
      return { valid: false, error: 'Motor to belt ratio must be 0.01-10.0', fieldId: 'motorToBeltRatio' };
    }

    // PIDAJ controller validation
    const pidKp = parseFloat(data['pidKp']);
    if (isNaN(pidKp) || pidKp < 0.1 || pidKp > 10.0) {
      return { valid: false, error: 'PID Kp must be 0.1-10.0', fieldId: 'pidKp' };
    }
    const pidKi = parseFloat(data['pidKi']);
    if (isNaN(pidKi) || pidKi < 0.0 || pidKi > 2.0) {
      return { valid: false, error: 'PID Ki must be 0.0-2.0', fieldId: 'pidKi' };
    }
    const pidKa = parseFloat(data['pidKa']);
    if (isNaN(pidKa) || pidKa < 0.0 || pidKa > 10.0) {
      return { valid: false, error: 'PID Ka must be 0.0-10.0', fieldId: 'pidKa' };
    }
    const pidKj = parseFloat(data['pidKj']);
    if (isNaN(pidKj) || pidKj < 0.0 || pidKj > 5.0) {
      return { valid: false, error: 'PID Kj must be 0.0-5.0', fieldId: 'pidKj' };
    }
    const pidDZ = parseFloat(data['pidDeadZone']);
    if (isNaN(pidDZ) || pidDZ < 0.05 || pidDZ > 1.0) {
      return { valid: false, error: 'Dead zone must be 0.05-1.0 km/h', fieldId: 'pidDeadZone' };
    }
    const pidLP = parseFloat(data['pidLongPressThresh']);
    if (isNaN(pidLP) || pidLP < 0.3 || pidLP > 5.0) {
      return { valid: false, error: 'Long press threshold must be 0.3-5.0 km/h', fieldId: 'pidLongPressThresh' };
    }
    const pidIC = parseFloat(data['pidIClamp']);
    if (isNaN(pidIC) || pidIC < 0.5 || pidIC > 20.0) {
      return { valid: false, error: 'Integral clamp must be 0.5-20.0', fieldId: 'pidIClamp' };
    }
    const pidPC = parseInt(data['pidPulseCooldown']);
    if (isNaN(pidPC) || pidPC < 100 || pidPC > 2000) {
      return { valid: false, error: 'Pulse cooldown must be 100-2000 ms', fieldId: 'pidPulseCooldown' };
    }
    const pidLPM = parseInt(data['pidLongPressMax']);
    if (isNaN(pidLPM) || pidLPM < 3000 || pidLPM > 30000) {
      return { valid: false, error: 'Long press max must be 3000-30000 ms', fieldId: 'pidLongPressMax' };
    }
    const pidCT = parseFloat(data['pidCoastThreshold']);
    if (isNaN(pidCT) || pidCT < 0.01 || pidCT > 0.2) {
      return { valid: false, error: 'Coast threshold must be 0.01-0.2 m/s²', fieldId: 'pidCoastThreshold' };
    }

    return { valid: true }; // All validation passed
  }

  /**
   * Handle settings form submission
   */
  export function handleFormSubmit(e: Event): void {
    e.preventDefault();
    
    const form = e.target as HTMLFormElement;
    const data: Record<string, string> = {};
    const formData = new FormData(form);
    formData.forEach((v, k) => data[k] = v.toString());
    
    // Validate before sending
    const validation = validateFormData(data);
    if (!validation.valid) {
      showStatus('❌ ' + validation.error, true);
      
      // Focus the problematic field if specified
      if (validation.fieldId) {
        const field = document.getElementById(validation.fieldId) as HTMLInputElement;
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
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(data)
    })
    .then(r => r.json())
    .then(j => {
      if (j.success) {
        showStatus('✅ Settings saved successfully! Most changes are active immediately. Reboot required only for WiFi/Pin changes.');
      } else {
        showStatus('❌ Error: ' + j.message, true);
      }
    })
    .catch(err => showStatus('❌ Network Error: ' + err.message, true));
  }

  /**
   * Load default settings
   */
  export function loadDefaults(): void {
    if (!confirm('Load default settings? This will overwrite all current settings.')) return;
    
    fetch('/api/settings/defaults', {method: 'POST'})
    .then(r => r.json())
    .then(j => {
      if (j.success) {
        showStatus('Default settings loaded! Refreshing page...');
        setTimeout(() => location.reload(), 1500);
      } else {
        showStatus('Error loading defaults: ' + j.message, true);
      }
    })
    .catch(err => showStatus('Error: ' + err.message, true));
  }

  /**
   * Perform factory reset
   */
  export function factoryReset(): void {
    if (!confirm('FACTORY RESET: This will erase ALL settings and reboot the device. Are you sure?')) return;
    if (!confirm('This cannot be undone. Continue with factory reset?')) return;
    
    fetch('/api/factory-reset', {method: 'POST'})
    .then(r => r.json())
    .then(j => {
      if (j.success) {
        showStatus('Factory reset complete! Device will reboot...');
        setTimeout(() => location.href = '/', 5000);
      } else {
        showStatus('Error during factory reset: ' + j.message, true);
      }
    })
    .catch(err => showStatus('Error: ' + err.message, true));
  }

  /**
   * Reboot the device
   */
  export function rebootDevice(): void {
    if (!confirm('Reboot the device? This will restart the ESP32.')) return;
    
    fetch('/api/reboot', {method: 'POST'})
    .then(() => {
      showStatus('Device rebooting... Please wait 30 seconds before refreshing.');
      setTimeout(() => location.href = '/', CONFIG.REBOOT_WAIT_TIME_MS);
    })
    .catch(() => {
      showStatus('Reboot initiated. Please wait 30 seconds.');
      setTimeout(() => location.href = '/', CONFIG.REBOOT_WAIT_TIME_MS);
    });
  }
  
  /**
   * View boot log in modal popup
   */
  export function viewBootLog(): void {
    // Create modal overlay
    const modal = document.createElement('div');
    modal.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.7);z-index:9999;display:flex;align-items:center;justify-content:center;';
    
    // Create modal content
    const content = document.createElement('div');
    content.style.cssText = 'background:#fff;padding:20px;border-radius:8px;max-width:90%;max-height:80%;overflow:auto;box-shadow:0 4px 20px rgba(0,0,0,0.3);';
    
    // Add header
    const header = document.createElement('div');
    header.style.cssText = 'display:flex;justify-content:space-between;align-items:center;margin-bottom:15px;border-bottom:2px solid #ddd;padding-bottom:10px;';
    header.innerHTML = '<h2 style="margin:0;">Boot Log</h2><button id="closeLogModal" style="padding:5px 15px;cursor:pointer;">Close</button>';
    content.appendChild(header);
    
    // Add loading message
    const logArea = document.createElement('pre');
    logArea.style.cssText = 'background:#1e1e1e;color:#d4d4d4;padding:15px;border-radius:4px;overflow:auto;max-height:60vh;font-family:Consolas,Monaco,monospace;font-size:12px;line-height:1.4;';
    logArea.textContent = 'Loading boot log...';
    content.appendChild(logArea);
    
    modal.appendChild(content);
    document.body.appendChild(modal);
    
    // Close button handler
    document.getElementById('closeLogModal')!.onclick = () => {
      document.body.removeChild(modal);
    };
    
    // Click outside to close
    modal.onclick = (e) => {
      if (e.target === modal) {
        document.body.removeChild(modal);
      }
    };
    
    // Fetch boot log
    fetch('/api/bootlog')
      .then(r => r.text())
      .then(log => {
        logArea.textContent = log;
      })
      .catch(err => {
        logArea.textContent = 'Error loading boot log: ' + err.message;
        logArea.style.color = '#ff6b6b';
      });
  }
}
