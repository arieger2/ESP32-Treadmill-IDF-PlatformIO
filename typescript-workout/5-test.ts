/**
 * Manual Test Controls
 * GPIO control buttons
 */

namespace WorkoutApp {
  
  export function initTest(): void {
    const speedDown = document.getElementById('speedDown') as HTMLButtonElement;
    const speedUp = document.getElementById('speedUp') as HTMLButtonElement;
    const inclineDown = document.getElementById('inclineDown') as HTMLButtonElement;
    const inclineUp = document.getElementById('inclineUp') as HTMLButtonElement;

    speedDown?.addEventListener('click', () => 
      doAction(speedDown, '/api/test/speed/down', '')
    );

    speedUp?.addEventListener('click', () => 
      doAction(speedUp, '/api/test/speed/up', '')
    );

    inclineDown?.addEventListener('click', () => 
      doAction(inclineDown, '/api/test/incline/down', '')
    );

    inclineUp?.addEventListener('click', () => 
      doAction(inclineUp, '/api/test/incline/up', '')
    );
  }
}
