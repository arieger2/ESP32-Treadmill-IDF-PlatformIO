/**
 * Main Entry Point
 * Initializes all modules
 */

namespace WorkoutApp {
  
  document.addEventListener('DOMContentLoaded', () => {
    initView();
    initInput();
    initControl();
    initTest();
    initPolling();
  });
}
