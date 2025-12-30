/**
 * @file 4-main.ts
 * @brief Settings page initialization
 */

namespace SettingsApp {
  // Expose functions globally IMMEDIATELY for onclick handlers in HTML
  // This must happen before DOMContentLoaded to prevent race conditions
  (window as any).SettingsApp = SettingsApp;
  
  document.addEventListener('DOMContentLoaded', () => {
    // Initialize select dropdowns
    initializeSelects();
    
    // Attach form submit handler
    const form = document.getElementById('settingsForm');
    if (form) {
      form.addEventListener('submit', handleFormSubmit);
    }
  });
}
