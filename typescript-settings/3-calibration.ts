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
        statusEl.textContent = `Calibration complete! Inertia: ${j.responseDelay || '?'} ms`;
        statusEl.className = 'status success';
        if (calibrationInterval) {
          clearInterval(calibrationInterval);
          calibrationInterval = null;
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
          case 'stabilizing':   progress = 'Waiting for stable speed...'; break;
          case 'pressing_up':   progress = `Pressing UP... (${j.currentSpeed?.toFixed(1) || '?'} km/h)`; break;
          case 'up_inertia':    progress = 'Measuring UP motor lag...'; break;
          case 'wait_down':     progress = 'Preparing DOWN test...'; break;
          case 'pressing_down': progress = `Pressing DOWN... (${j.currentSpeed?.toFixed(1) || '?'} km/h)`; break;
          case 'down_inertia':  progress = 'Measuring DOWN motor lag...'; break;
          case 'finalizing':    progress = 'Computing results...'; break;
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
