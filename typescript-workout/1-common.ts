/**
 * Common Utilities
 * Shared functions and state management
 */

namespace WorkoutApp {
  
  const state: GlobalState = {
    timeline: [],
    total: 0,
    pollInterval: null,
  };

  let errorMsgElement: HTMLDivElement | null = null;
  let drawFunction: ((state: WorkoutStateResponse) => void) | null = null;

  export function setErrorElement(elem: HTMLDivElement | null): void {
    errorMsgElement = elem;
  }

  export function setDrawFunction(fn: (state: WorkoutStateResponse) => void): void {
    drawFunction = fn;
  }

  /**
   * Converts km/h to pace string (mm:ss)
   */
  export function kmhToPaceStr(kmh: number): string {
    if (!kmh || kmh <= 0) return '--:--';
    
    const paceMin = 60 / kmh;
    const mm = Math.floor(paceMin);
    let ss = Math.round((paceMin - mm) * 60);
    
    if (ss >= 60) ss = 59;
    
    return `${mm}:${ss < 10 ? '0' + ss : ss}`;
  }

  /**
   * Performs an action (POST request)
   */
  export async function doAction(
    btn: HTMLButtonElement,
    url: string,
    body: string
  ): Promise<void> {
    try {
      btn.disabled = true;
      
      const res = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body,
        cache: 'no-store',
      });

      if (!res.ok) {
        const text = await res.text();
        if (errorMsgElement) {
          errorMsgElement.textContent = `⚠️ Error: ${text}`;
          errorMsgElement.style.display = 'block';
        }
        return;
      }

      await new Promise(resolve => setTimeout(resolve, CONFIG.ACTION_DELAY_MS));
      await poll();
    } catch (e) {
      if (errorMsgElement) {
        errorMsgElement.textContent = `⚠️ Network error: ${e instanceof Error ? e.message : String(e)}`;
        errorMsgElement.style.display = 'block';
      }
    } finally {
      setTimeout(() => {
        btn.disabled = false;
        btn.blur();
      }, CONFIG.BUTTON_COOLDOWN_MS);
    }
  }

  /**
   * Polls server for workout state
   */
  export async function poll(): Promise<void> {
    try {
      const res = await fetch('/api/workout/state?full=1', { cache: 'no-store' });
      if (!res.ok) return;
      
      const workoutState: WorkoutStateResponse = await res.json();
      if (drawFunction) {
        drawFunction(workoutState);
      }
    } catch (e) {
      // Silent fail - will retry
    }
  }

  /**
   * Fetches full workout and redraws
   */
  export async function fetchAndUpdateFullWorkout(): Promise<void> {
    try {
      const res = await fetch('/api/workout/state?full=1', { cache: 'no-store' });
      if (!res.ok) return;
      
      const workoutState: WorkoutStateResponse = await res.json();
      if (drawFunction) {
        drawFunction(workoutState);
      }
    } catch (e) {
      // Silent fail
    }
  }

  /**
   * Initializes polling
   */
  export function initPolling(): void {
    fetchAndUpdateFullWorkout();
    state.pollInterval = window.setInterval(poll, CONFIG.POLL_INTERVAL_MS);

    window.addEventListener('beforeunload', () => {
      if (state.pollInterval !== null) {
        clearInterval(state.pollInterval);
      }
    });
  }
}
