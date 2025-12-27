/**
 * Control Buttons
 * Start/Pause/Resume/Stop and Pace controls
 */

namespace WorkoutApp {
  
  export function initControl(): void {
    const btnStart = document.getElementById('btnStart') as HTMLButtonElement;
    const btnPause = document.getElementById('btnPause') as HTMLButtonElement;
    const btnResume = document.getElementById('btnResume') as HTMLButtonElement;
    const btnStop = document.getElementById('btnStop') as HTMLButtonElement;
    const paceDown = document.getElementById('paceDown') as HTMLButtonElement;
    const paceUp = document.getElementById('paceUp') as HTMLButtonElement;

    btnStart?.addEventListener('click', () => 
      doAction(btnStart, '/api/workout/control', 'action=start')
    );

    btnPause?.addEventListener('click', () => 
      doAction(btnPause, '/api/workout/control', 'action=pause')
    );

    btnResume?.addEventListener('click', () => 
      doAction(btnResume, '/api/workout/control', 'action=resume')
    );

    btnStop?.addEventListener('click', () => 
      doAction(btnStop, '/api/workout/control', 'action=stop')
    );

    paceDown?.addEventListener('click', () => 
      doAction(paceDown, '/api/workout/scale/nudge', 'rel=-0.01')
    );

    paceUp?.addEventListener('click', () => 
      doAction(paceUp, '/api/workout/scale/nudge', 'rel=+0.01')
    );
  }
}
