/**
 * @file 4-main.ts
 * @brief Settings page initialization
 */

namespace SettingsApp {
  document.addEventListener('DOMContentLoaded', () => {
    // Initialize select dropdowns
    initializeSelects();
    
    // Attach form submit handler
    const form = document.getElementById('settingsForm');
    if (form) {
      form.addEventListener('submit', handleFormSubmit);
    }
    
    // Expose functions globally for onclick handlers in HTML
    (window as any).SettingsApp = SettingsApp;
  });
}
