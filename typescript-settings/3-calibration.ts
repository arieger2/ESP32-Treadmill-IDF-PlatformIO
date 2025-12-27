/**
 * @file 3-calibration.ts
 * @brief Auto-calibration functionality
 */

namespace SettingsApp {
  let calibrationInterval: number | null = null;

  /**
   * Start calibration process
   */
  export function startCalibration(): void {
    const statusEl = document.getElementById('calibrationStatus');
    if (!statusEl) return;
    
    statusEl.textContent = 'Starting calibration...';
    statusEl.className = 'status';
    statusEl.style.display = 'block';
    
    fetch('/api/calibrate/start', {method: 'POST'})
    .then(r => r.json())
    .then(j => {
      if (j.success) {
        statusEl.textContent = 'Calibration started! Monitoring progress...';
        // Poll status every second
        if (calibrationInterval) clearInterval(calibrationInterval);
        calibrationInterval = window.setInterval(updateCalibrationStatus, CONFIG.CALIBRATION_POLL_INTERVAL_MS);
      } else {
        statusEl.textContent = 'Error: ' + (j.message || 'Could not start calibration');
        statusEl.className = 'status error';
      }
    })
    .catch(err => {
      statusEl.textContent = 'Error: ' + err.message;
      statusEl.className = 'status error';
    });
  }

  /**
   * Update calibration status (called by interval)
   */
  function updateCalibrationStatus(): void {
    fetch('/api/calibrate/status')
    .then(r => r.json())
    .then((j: CalibrationStatusResponse) => {
      const statusEl = document.getElementById('calibrationStatus');
      if (!statusEl) return;
      
      if (j.state === 'done') {
        statusEl.textContent = `✓ Calibration complete! UP: ${j.speedUpRate?.toFixed(3)} km/h/s, DOWN: ${j.speedDownRate?.toFixed(3)} km/h/s`;
        statusEl.className = 'status success';
        if (calibrationInterval) {
          clearInterval(calibrationInterval);
          calibrationInterval = null;
        }
        
        // Update input fields with new values
        const speedUpInput = document.getElementById('speedUpRate') as HTMLInputElement | null;
        const speedDownInput = document.getElementById('speedDownRate') as HTMLInputElement | null;
        if (speedUpInput && j.speedUpRate !== undefined) {
          speedUpInput.value = j.speedUpRate.toFixed(3);
        }
        if (speedDownInput && j.speedDownRate !== undefined) {
          speedDownInput.value = j.speedDownRate.toFixed(3);
        }
      } else if (j.state === 'error') {
        statusEl.textContent = '✗ Calibration failed: ' + (j.message || 'Unknown error');
        statusEl.className = 'status error';
        if (calibrationInterval) {
          clearInterval(calibrationInterval);
          calibrationInterval = null;
        }
      } else {
        // Show detailed progress with speeds
        let progress = '';
        switch (j.state) {
          case 'starting_up':   progress = '⏳ Preparing UP calibration...'; break;
          case 'pressing_up':   progress = '⬆️ Pressing UP button...'; break;
          case 'waiting_up':    progress = `⏱️ Waiting for UP stabilization (${j.midSpeed?.toFixed(1) || '?'} km/h)...`; break;
          case 'starting_down': progress = '⏳ Preparing DOWN calibration...'; break;
          case 'pressing_down': progress = '⬇️ Pressing DOWN button...'; break;
          case 'waiting_down':  progress = `⏱️ Waiting for DOWN stabilization (${j.endSpeed?.toFixed(1) || '?'} km/h)...`; break;
          default:              progress = 'Status: ' + j.state;
        }
        if (j.message) progress += ' - ' + j.message;
        statusEl.textContent = progress;
        statusEl.className = 'status';
      }
    })
    .catch(err => {
      console.error('Status check failed:', err);
    });
  }
}
