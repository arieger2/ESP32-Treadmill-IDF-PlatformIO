/**
 * Configuration and Type Definitions
 * File prefixed with 0- to ensure it's compiled first
 */

namespace WorkoutApp {
  
  /** Configuration constants */
  export const CONFIG = {
    ACTION_DELAY_MS: 150,
    BUTTON_COOLDOWN_MS: 200,
    POLL_INTERVAL_MS: 500,
    CANVAS_WIDTH: 860,
    CANVAS_HEIGHT: 180,
    CANVAS_PADDING: 15,
  } as const;

  /** Workout execution states */
  export type WorkoutState = 'Idle' | 'Running' | 'Paused' | 'Finished';

  /** Single workout step/interval */
  export interface WorkoutStep {
    d: number;  // Duration in seconds
    v: number;  // Speed in km/h
    i: number;  // Incline in percent
    l?: string; // Label (optional)
  }

  /** Complete workout state from server */
  export interface WorkoutStateResponse {
    state: WorkoutState;
    steps: WorkoutStep[];
    step_index: number;
    elapsed_s: number;
    speed_kph: number;
    incline_pct: number;
    error?: string;
  }

  /** Global state */
  export interface GlobalState {
    timeline: WorkoutStep[];
    total: number;
    pollInterval: number | null;
  }
}
