/**
 * @file 2-chart.ts
 * @brief Chart rendering for HR and RR series
 */

namespace MonitorApp {
  /**
   * Renders a line chart into an SVG element
   */
  export function renderLineChart(
    svg: SVGSVGElement,
    series: SeriesDataPoint[],
    color: string,
    units: string
  ): void {
    // Get actual SVG dimensions from container
    const svgRect = svg.getBoundingClientRect();
    const width = svgRect.width || CONFIG.CHART_WIDTH;
    const height = svgRect.height || CONFIG.CHART_HEIGHT;

    if (!series || series.length === 0) {
      svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
      svg.setAttribute('preserveAspectRatio', 'none');
      svg.innerHTML = '<text x="50%" y="50%" dominant-baseline="middle" text-anchor="middle" fill="#888" font-size="14">No data</text>';
      return;
    }

    const baseTime = series[0].t;
    const xValues = series.map(p => ((p.t - baseTime) / 1000));
    const maxX = xValues[xValues.length - 1] || 1;

    let minY = series[0].v;
    let maxY = series[0].v;
    for (let i = 1; i < series.length; i++) {
      const val = series[i].v;
      if (val < minY) minY = val;
      if (val > maxY) maxY = val;
    }
    if (minY === maxY) {
      minY -= 1;
      maxY += 1;
    }

    const pathPoints: string[] = [];
    for (let i = 0; i < series.length; i++) {
      const x = maxX === 0 ? 0 : (xValues[i] / maxX) * width;
      const normalized = (series[i].v - minY) / (maxY - minY);
      const y = height - (normalized * height);
      pathPoints.push(`${i === 0 ? 'M' : 'L'}${x.toFixed(2)} ${y.toFixed(2)}`);
    }

    const minLabel = `${minY.toFixed(0)} ${units}`;
    const maxLabel = `${maxY.toFixed(0)} ${units}`;
    const spanLabel = `${Math.max(0, Math.round(maxX))}s window`;

    svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
    svg.setAttribute('preserveAspectRatio', 'none');
    svg.innerHTML = `
      <rect x="0" y="0" width="${width}" height="${height}" fill="white" stroke="#ddd" />
      <path d="${pathPoints.join(' ')}" stroke="${color}" stroke-width="2" fill="none" />
      <text x="4" y="16" fill="#555" font-size="12">${maxLabel}</text>
      <text x="4" y="${height - 4}" fill="#555" font-size="12">${minLabel}</text>
      <text x="${width - 4}" y="${height - 4}" fill="#888" font-size="12" text-anchor="end">${spanLabel}</text>
    `;
  }
}
