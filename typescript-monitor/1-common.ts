/**
 * @file 1-common.ts
 * @brief Common utilities for monitor page
 */

namespace MonitorApp {
  /**
   * Updates a single DOM element by ID with a text value
   */
  function setTextById(id: string, value: string): void {
    const el = document.getElementById(id);
    if (el) el.textContent = value;
  }

  /**
   * Fetches series data from API endpoint and renders chart
   */
  export function updateSeries(
    svg: SVGSVGElement | null,
    endpoint: string,
    color: string,
    units: string
  ): void {
    if (!svg) return;

    fetch(`/${endpoint}?t=${Date.now()}`, { cache: 'no-store' })
      .then(r => r.ok ? r.json() : [])
      .then((series: SeriesDataPoint[] | unknown) => {
        if (!Array.isArray(series)) {
          renderLineChart(svg, [], color, units);
          return;
        }
        renderLineChart(svg, series, color, units);
      })
      .catch(() => {
        renderLineChart(svg, [], color, units);
      });
  }

  /**
   * Main refresh function - uses SINGLE combined API endpoint
   * Reduces from 15 HTTP requests to just 1 per refresh cycle!
   */
  export function refreshMetrics(): void {
    // Only poll if page is visible
    if (document.hidden) return;

    // Single request for all scalar values
    fetch(`/api/monitor?t=${Date.now()}`, { cache: 'no-store' })
      .then(r => r.ok ? r.json() : null)
      .then((data: Record<string, string> | null) => {
        if (!data) return;
        
        setTextById('speed', data.speed);
        setTextById('pacemin', data.pacemin);
        setTextById('pacesec', data.pacesec);
        setTextById('cpuusage', data.cpu);
        setTextById('distance', data.distance);
        setTextById('distanceunit', data.distanceunit);
        setTextById('hour', data.hour);
        setTextById('minute', data.minute);
        setTextById('second', data.second);
        setTextById('offset', data.offset);
        setTextById('rpm', data.rpm);
        setTextById('motorrpm', data.motorrpm);
        setTextById('heartrate', data.heartrate);
        setTextById('rr', data.rr);
        setTextById('datetime', data.datetime);
      })
      .catch(() => {});

    // Charts still need separate requests (different data format)
    const hrChart = document.getElementById('hr-chart') as unknown as SVGSVGElement | null;
    const rrChart = document.getElementById('rr-chart') as unknown as SVGSVGElement | null;
    
    updateSeries(hrChart, 'api/hr-series', '#cc1f3a', 'bpm');
    updateSeries(rrChart, 'api/rr-series', '#0070c0', 'ms');
  }

  /**
   * Initialize polling interval and visibility change handling
   */
  export function initPolling(): void {
    setInterval(refreshMetrics, CONFIG.POLL_INTERVAL_MS);
    refreshMetrics();

    // Resume polling when page becomes visible again
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) refreshMetrics();
    });
  }
}
