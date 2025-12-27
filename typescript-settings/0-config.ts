/**
 * @file 0-config.ts
 * @brief Settings page configuration and types
 */

namespace SettingsApp {
  export interface CalibrationStatusResponse {
    state: 'idle' | 'starting_up' | 'pressing_up' | 'waiting_up' | 
           'starting_down' | 'pressing_down' | 'waiting_down' | 
           'done' | 'error' | string;
    message?: string;
    speedUpRate?: number;
    speedDownRate?: number;
    startSpeed?: number;
    midSpeed?: number;
    endSpeed?: number;
  }

  export const CONFIG = {
    STATUS_DISPLAY_TIME_MS: 5000,
    CALIBRATION_POLL_INTERVAL_MS: 1000,
    REBOOT_WAIT_TIME_MS: 30000,
  };
}
