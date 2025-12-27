/**
 * File Upload and Drag & Drop
 */

namespace WorkoutApp {
  
  let fileInput: HTMLInputElement | null = null;
  let uploadStatus: HTMLSpanElement | null = null;
  let canvas: HTMLCanvasElement | null = null;

  export function initInput(): void {
    fileInput = document.getElementById('zwoFile') as HTMLInputElement;
    uploadStatus = document.getElementById('uploadStatus') as HTMLSpanElement;
    canvas = document.getElementById('woCanvas') as HTMLCanvasElement;

    setupFileInput();
    setupDragAndDrop();
  }

  async function uploadZwo(file: File): Promise<void> {
    if (!file) return;

    try {
      if (uploadStatus) uploadStatus.textContent = 'Uploading...';

      const fd = new FormData();
      fd.append('file', file, file.name);

      const res = await fetch(`/api/workout/upload?t=${Date.now()}`, {
        method: 'POST',
        body: fd,
      });

      if (!res.ok) {
        const text = await res.text();
        if (uploadStatus) uploadStatus.textContent = `Upload failed: ${text}`;
        return;
      }

      if (uploadStatus) uploadStatus.textContent = 'Workout loaded ✔';
      await fetchAndUpdateFullWorkout();
    } catch (e) {
      if (uploadStatus) {
        uploadStatus.textContent = `Upload error: ${e instanceof Error ? e.message : String(e)}`;
      }
    }
  }

  function setupFileInput(): void {
    if (!fileInput) return;

    fileInput.addEventListener('change', () => {
      const file = fileInput?.files?.[0];
      if (file) uploadZwo(file);
    });
  }

  function setupDragAndDrop(): void {
    if (!canvas) return;

    const dropOverlay = document.createElement('div');
    dropOverlay.style.cssText = `
      position: absolute;
      left: 0; top: 0; right: 0; bottom: 0;
      display: none;
      align-items: center;
      justify-content: center;
      background: rgba(0,0,0,0.05);
      border: 2px dashed #999;
      font: 14px/1.4 sans-serif;
      color: #333;
      pointer-events: none;
    `;
    dropOverlay.textContent = 'Drop .zwo / .xml file here';

    const wrapper = document.createElement('div');
    wrapper.style.position = 'relative';
    canvas.parentNode?.insertBefore(wrapper, canvas);
    wrapper.appendChild(canvas);
    wrapper.appendChild(dropOverlay);

    ['dragover', 'drop'].forEach(type => {
      window.addEventListener(type, (e: Event) => e.preventDefault());
    });

    let dragDepth = 0;

    canvas.addEventListener('dragenter', (e: DragEvent) => {
      e.preventDefault();
      e.stopPropagation();
      dragDepth++;
      dropOverlay.style.display = 'flex';
    });

    canvas.addEventListener('dragover', (e: DragEvent) => {
      e.preventDefault();
      e.stopPropagation();
      if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy';
      dropOverlay.style.display = 'flex';
    });

    canvas.addEventListener('dragleave', (e: DragEvent) => {
      e.preventDefault();
      e.stopPropagation();
      dragDepth = Math.max(0, dragDepth - 1);
      if (dragDepth === 0) dropOverlay.style.display = 'none';
    });

    canvas.addEventListener('drop', (e: DragEvent) => {
      e.preventDefault();
      e.stopPropagation();
      dragDepth = 0;
      dropOverlay.style.display = 'none';
      
      const file = e.dataTransfer?.files?.[0];
      if (file) uploadZwo(file);
    });
  }
}
