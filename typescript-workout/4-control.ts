/**
 * Control Buttons
 * Start/Pause/Resume/Stop and Pace controls
 */

namespace WorkoutApp {

  export function initControl(): void {

    const thresholdInput = document.getElementById('thresholdInput') as HTMLInputElement;
    const btnThreshold   = document.getElementById('btnThreshold')   as HTMLButtonElement;
    const thresholdHint  = document.getElementById('thresholdHint')  as HTMLSpanElement;

    // Convert seconds/km to "m:ss" display string
    function secToMinKm(sec: number): string {
      if (sec <= 0) return '';
      const m = Math.floor(sec / 60);
      const s = Math.round(sec % 60);
      return `${m}:${s < 10 ? '0' + s : s}`;
    }

    // Parse "m:ss" to seconds, returns NaN on bad input
    function minKmToSec(txt: string): number {
      const p = txt.trim().split(':');
      if (p.length !== 2) return NaN;
      const m = parseInt(p[0], 10), s = parseInt(p[1], 10);
      if (isNaN(m) || isNaN(s) || s < 0 || s > 59 || m < 0) return NaN;
      return m * 60 + s;
    }

    // Fetch current threshold from backend and update the field
    function refreshThreshold(): void {
      fetch('/api/workout/threshold')
        .then(r => r.text())
        .then(tv => {
          const sec = parseFloat(tv);
          if (isNaN(sec) || sec <= 0) return;
          if (thresholdInput) thresholdInput.value = secToMinKm(sec);
          if (thresholdHint)  thresholdHint.textContent =
            `${secToMinKm(sec)} min/km \u2248 ${(3600 / sec).toFixed(1)} km/h bei IF=1.0`;
        }).catch(() => {});
    }

    // SET: send typed threshold to backend (backend rescales steps + stores)
    function applyThreshold(): void {
      const sec = minKmToSec(thresholdInput?.value ?? '');
      if (isNaN(sec) || sec <= 0) {
        if (thresholdHint) thresholdHint.textContent = '\u26a0\ufe0f Format: mm:ss (z.B. 5:30)';
        return;
      }
      doAction(btnThreshold, '/api/workout/threshold', `val=${sec}`)
        .then(() => refreshThreshold());
    }

    btnThreshold?.addEventListener('click', applyThreshold);
    thresholdInput?.addEventListener('keydown', (e: KeyboardEvent) => {
      if (e.key === 'Enter') applyThreshold();
    });

    // -pace / +pace: backend scales steps AND threshold together
    const paceDown = document.getElementById('paceDown') as HTMLButtonElement;
    const paceUp   = document.getElementById('paceUp')   as HTMLButtonElement;
    paceDown?.addEventListener('click', () =>
      doAction(paceDown, '/api/workout/scale/nudge', 'rel=-0.01').then(() => refreshThreshold())
    );
    paceUp?.addEventListener('click', () =>
      doAction(paceUp, '/api/workout/scale/nudge', 'rel=+0.01').then(() => refreshThreshold())
    );

    const btnStart  = document.getElementById('btnStart')  as HTMLButtonElement;
    const btnPause  = document.getElementById('btnPause')  as HTMLButtonElement;
    const btnResume = document.getElementById('btnResume') as HTMLButtonElement;
    const btnStop   = document.getElementById('btnStop')   as HTMLButtonElement;
    btnStart?.addEventListener('click',  () => doAction(btnStart,  '/api/workout/control', 'action=start'));
    btnPause?.addEventListener('click',  () => doAction(btnPause,  '/api/workout/control', 'action=pause'));
    btnResume?.addEventListener('click', () => doAction(btnResume, '/api/workout/control', 'action=resume'));
    btnStop?.addEventListener('click',   () => doAction(btnStop,   '/api/workout/control', 'action=stop'));

    refreshThreshold();
  }
}
