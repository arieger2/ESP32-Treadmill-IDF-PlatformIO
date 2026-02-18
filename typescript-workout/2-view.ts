/**
 * Workout Visualization
 * Canvas rendering and status display
 */

namespace WorkoutApp {
  
  let canvas: HTMLCanvasElement | null = null;
  let ctx: CanvasRenderingContext2D | null = null;
  let stateSpan: HTMLSpanElement | null = null;
  let errorMsg: HTMLDivElement | null = null;
  let paceNow: HTMLSpanElement | null = null;

  let btnStart: HTMLButtonElement | null = null;
  let btnPause: HTMLButtonElement | null = null;
  let btnResume: HTMLButtonElement | null = null;
  let btnStop: HTMLButtonElement | null = null;

  let timeline: WorkoutStep[] = [];
  let total = 0;

  export function initView(): void {
    canvas = document.getElementById('woCanvas') as HTMLCanvasElement;
    ctx = canvas?.getContext('2d') ?? null;
    stateSpan = document.getElementById('woState') as HTMLSpanElement;
    errorMsg = document.getElementById('errorMsg') as HTMLDivElement;
    paceNow = document.getElementById('paceNow') as HTMLSpanElement;

    btnStart = document.getElementById('btnStart') as HTMLButtonElement;
    btnPause = document.getElementById('btnPause') as HTMLButtonElement;
    btnResume = document.getElementById('btnResume') as HTMLButtonElement;
    btnStop = document.getElementById('btnStop') as HTMLButtonElement;

    setErrorElement(errorMsg);
    setDrawFunction(draw);
  }

  function draw(state: WorkoutStateResponse): void {
    if (!ctx || !canvas) return;

    if (state.steps && JSON.stringify(state.steps) !== JSON.stringify(timeline)) {
      timeline = state.steps;
      total = timeline.reduce((acc, step) => acc + (step.d || 0), 0);
    }

    ctx.clearRect(0, 0, canvas.width, canvas.height);

    const pad = CONFIG.CANVAS_PADDING;
    const w = canvas.width - pad * 2;
    const h = canvas.height - pad * 2;
    
    // Single X-axis at bottom - speed bars grow upward from it
    const xAxisY = pad + h;  // Bottom line is the X-axis

    // Draw axes - only Y-axis and X-axis (no middle divider)
    ctx.strokeStyle = '#999';
    ctx.beginPath();
    ctx.moveTo(pad, pad);              // Y-axis top
    ctx.lineTo(pad, xAxisY);           // Y-axis bottom
    ctx.moveTo(pad, xAxisY);           // X-axis start
    ctx.lineTo(pad + w, xAxisY);       // X-axis end
    ctx.stroke();

    if (!total) {
      if (stateSpan) stateSpan.textContent = `State: ${state.state || 'Idle'}`;
      if (paceNow) paceNow.textContent = '--:--';
      updateButtonStates(state, false);
      return;
    }

    const maxV = Math.max(1, ...timeline.map(s => s.v || 0));

    let t0 = 0;
    for (const step of timeline) {
      const x0 = pad + (t0 / total) * w;
      const x1 = pad + ((t0 + step.d) / total) * w;
      const barW = x1 - x0 - 1;

      // Speed bar - grows UP from the X-axis (bottom), using full height
      const vh = maxV > 0 ? (step.v / maxV) * (h - 16) : 0;  // Leave space for label at top
      ctx.fillStyle = '#4a90e2';
      ctx.fillRect(x0, xAxisY - vh, barW, vh);

      // Pace label - horizontal for wide bars, rotated for narrow bars
      if (barW >= 26) {
        const cx = x0 + barW / 2;
        const labelY = xAxisY - vh - 2;
        ctx.fillStyle = '#000';
        ctx.font = '11px sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'bottom';
        ctx.fillText(kmhToPaceStr(step.v || 0), cx, labelY);
      } else if (barW >= 8 && vh >= 20) {
        // Narrow bar: rotate text -90° and place inside/above bar
        const cx = x0 + barW / 2;
        const labelY = xAxisY - vh - 4;
        ctx.save();
        ctx.translate(cx, labelY);
        ctx.rotate(-Math.PI / 2);
        ctx.fillStyle = '#000';
        ctx.font = '11px sans-serif';
        ctx.textAlign = 'left';
        ctx.textBaseline = 'middle';
        ctx.fillText(kmhToPaceStr(step.v || 0), 0, 0);
        ctx.restore();
      }

      t0 += step.d;
    }

    // Current position marker
    if (state.elapsed_s) {
      const xn = pad + (state.elapsed_s / total) * w;
      ctx.strokeStyle = '#d0021b';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(xn, pad);
      ctx.lineTo(xn, pad + h);
      ctx.stroke();
      ctx.lineWidth = 1;
    }

    const vNow = Number(state.speed_kph || 0);
    if (paceNow) paceNow.textContent = kmhToPaceStr(vNow);

    if (stateSpan) {
      stateSpan.textContent = 
        `State: ${state.state} | Step ${state.step_index + 1}/${timeline.length} ` +
        `| Speed ${vNow.toFixed(1)} km/h | Pace ${kmhToPaceStr(vNow)} ` +
        `| Incline ${Number(state.incline_pct || 0).toFixed(1)} %`;
    }

    if (errorMsg) {
      if (state.error && state.error.length > 0) {
        errorMsg.textContent = `⚠️ Error: ${state.error}`;
        errorMsg.style.display = 'block';
      } else {
        errorMsg.style.display = 'none';
      }
    }

    updateButtonStates(state, timeline && timeline.length > 0);
  }

  function updateButtonStates(state: WorkoutStateResponse, hasWorkout: boolean): void {
    const isRunning = state.state === 'Running';
    const isPaused = state.state === 'Paused';

    if (btnStart) btnStart.disabled = !hasWorkout || (isRunning || isPaused);
    if (btnPause) btnPause.disabled = !isRunning;
    if (btnResume) btnResume.disabled = !isPaused;
    if (btnStop) btnStop.disabled = !(isRunning || isPaused);
  }
}
