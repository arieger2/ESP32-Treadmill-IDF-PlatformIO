/**
 * @file 1-common.ts
 * @brief Common utilities for settings page
 */

namespace SettingsApp {
  /**
   * Display status message with auto-hide
   */
  export function showStatus(msg: string, err: boolean = false): void {
    const s = document.getElementById('status');
    if (!s) return;
    
    s.textContent = msg;
    s.className = 'status ' + (err ? 'error' : 'success');
    s.style.display = 'block';
    setTimeout(() => s.style.display = 'none', CONFIG.STATUS_DISPLAY_TIME_MS);
  }

  /**
   * Navigate to OTA update page
   */
  export function goToOTA(): void {
    sessionStorage.setItem('fromSettings', '1');
    window.location.href = '/update';
  }

  /**
   * Initialize select dropdowns with data-value attributes
   * The values are set via data-value in HTML (from template processor)
   */
  export function initializeSelects(): void {
    const selects = document.querySelectorAll('select[data-value]');
    selects.forEach(sel => {
      const select = sel as HTMLSelectElement;
      const value = select.dataset.value;
      if (value && value.indexOf('%') === -1) { // Skip if placeholder not replaced
        select.value = value;
      }
    });
  }
}
