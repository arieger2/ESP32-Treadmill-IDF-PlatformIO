/**
 * @file 0-config.ts
 * @brief Settings page configuration and types
 */

namespace SettingsApp {
  export interface CalibrationStatusResponse {
    state: 'idle' | 'stabilizing' | 'pressing_up' | 'up_inertia' |
           'wait_down' | 'pressing_down' | 'down_inertia' |
           'finalizing' | 'done' | 'error' | string;
    message?: string;
    responseDelay?: number;
    currentSpeed?: number;
    checkpoint?: number;
  }

  export const CONFIG = {
    STATUS_DISPLAY_TIME_MS: 5000,
    CALIBRATION_POLL_INTERVAL_MS: 1000,
    REBOOT_WAIT_TIME_MS: 30000,
  };
}
