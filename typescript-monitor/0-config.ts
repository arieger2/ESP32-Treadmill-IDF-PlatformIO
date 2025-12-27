/**
 * @file 0-config.ts
 * @brief Monitor configuration and type definitions
 */

namespace MonitorApp {
  export interface SeriesDataPoint {
    t: number;  // timestamp in ms
    v: number;  // value
  }

  export const CONFIG = {
    POLL_INTERVAL_MS: 1000,
    CHART_WIDTH: 280,
    CHART_HEIGHT: 60,
  };
}
