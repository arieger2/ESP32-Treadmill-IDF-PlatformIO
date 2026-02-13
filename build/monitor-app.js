/**
 * @file 0-config.ts
 * @brief Monitor configuration and type definitions
 */
var MonitorApp;
(function (MonitorApp) {
    MonitorApp.CONFIG = {
        POLL_INTERVAL_MS: 1000,
        CHART_WIDTH: 280,
        CHART_HEIGHT: 60,
    };
})(MonitorApp || (MonitorApp = {}));
/**
 * @file 1-common.ts
 * @brief Common utilities for monitor page
 */
var MonitorApp;
(function (MonitorApp) {
    /**
     * Updates a single DOM element by ID with a text value
     */
    function setTextById(id, value) {
        var el = document.getElementById(id);
        if (el)
            el.textContent = value;
    }
    /**
     * Fetches series data from API endpoint and renders chart
     */
    function updateSeries(svg, endpoint, color, units) {
        if (!svg)
            return;
        fetch("/".concat(endpoint, "?t=").concat(Date.now()), { cache: 'no-store' })
            .then(function (r) { return r.ok ? r.json() : []; })
            .then(function (series) {
            if (!Array.isArray(series)) {
                MonitorApp.renderLineChart(svg, [], color, units);
                return;
            }
            MonitorApp.renderLineChart(svg, series, color, units);
        })
            .catch(function () {
            MonitorApp.renderLineChart(svg, [], color, units);
        });
    }
    MonitorApp.updateSeries = updateSeries;
    /**
     * Main refresh function - uses SINGLE combined API endpoint
     * Reduces from 15 HTTP requests to just 1 per refresh cycle!
     */
    function refreshMetrics() {
        // Only poll if page is visible
        if (document.hidden)
            return;
        // Single request for all scalar values
        fetch("/api/monitor?t=".concat(Date.now()), { cache: 'no-store' })
            .then(function (r) { return r.ok ? r.json() : null; })
            .then(function (data) {
            if (!data)
                return;
            setTextById('speed', data.speed);
            setTextById('pacemin', data.pacemin);
            setTextById('pacesec', data.pacesec);
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
            // Update signal quality
            setTextById('signal-quality', data.signalquality);
            setTextById('signal-cv', data.signalcv);
            setTextById('signal-freq', data.signalfreq);
            // Update signal quality color coding
            var qualityEl = document.getElementById('signal-quality');
            if (qualityEl && qualityEl.parentElement && qualityEl.parentElement.parentElement) {
                var metricDiv = qualityEl.parentElement.parentElement;
                metricDiv.className = 'metric ' + (data.signalclass || 'quality-good');
            }
            // Update testdata button appearance
            var testBtn = document.getElementById('testdata-btn');
            var testText = document.getElementById('testdata-text');
            if (testBtn && testText) {
                var isActive = data.testdata === true || data.testdata === 'true';
                testBtn.className = isActive ? 'button button-red' : 'button button-green';
                testText.textContent = isActive ? 'Test Mode ON' : 'Test Mode OFF';
            }
        })
            .catch(function () { });
        // Charts still need separate requests (different data format)
        var hrChart = document.getElementById('hr-chart');
        var rrChart = document.getElementById('rr-chart');
        updateSeries(hrChart, 'api/hr-series', '#cc1f3a', 'bpm');
        updateSeries(rrChart, 'api/rr-series', '#0070c0', 'ms');
    }
    MonitorApp.refreshMetrics = refreshMetrics;
    /**
     * Initialize polling interval and visibility change handling
     */
    function initPolling() {
        setInterval(refreshMetrics, MonitorApp.CONFIG.POLL_INTERVAL_MS);
        refreshMetrics();
        // Resume polling when page becomes visible again
        document.addEventListener('visibilitychange', function () {
            if (!document.hidden)
                refreshMetrics();
        });
    }
    MonitorApp.initPolling = initPolling;
    /**
     * Toggle test mode - called by button click
     */
    window.toggleTestMode = function () {
        fetch('/testdata', { method: 'GET' })
            .then(function () {
            // Force immediate refresh to update button state
            setTimeout(refreshMetrics, 100);
        })
            .catch(function (err) { return console.error('Failed to toggle test mode:', err); });
    };
})(MonitorApp || (MonitorApp = {}));
/**
 * @file 2-chart.ts
 * @brief Chart rendering for HR and RR series
 */
var MonitorApp;
(function (MonitorApp) {
    /**
     * Renders a line chart into an SVG element
     */
    function renderLineChart(svg, series, color, units) {
        // Get actual SVG dimensions from container
        var svgRect = svg.getBoundingClientRect();
        var width = svgRect.width || MonitorApp.CONFIG.CHART_WIDTH;
        var height = svgRect.height || MonitorApp.CONFIG.CHART_HEIGHT;
        if (!series || series.length === 0) {
            svg.setAttribute('viewBox', "0 0 ".concat(width, " ").concat(height));
            svg.setAttribute('preserveAspectRatio', 'none');
            svg.innerHTML = '<text x="50%" y="50%" dominant-baseline="middle" text-anchor="middle" fill="#888" font-size="14">No data</text>';
            return;
        }
        var baseTime = series[0].t;
        var xValues = series.map(function (p) { return ((p.t - baseTime) / 1000); });
        var maxX = xValues[xValues.length - 1] || 1;
        var minY = series[0].v;
        var maxY = series[0].v;
        for (var i = 1; i < series.length; i++) {
            var val = series[i].v;
            if (val < minY)
                minY = val;
            if (val > maxY)
                maxY = val;
        }
        if (minY === maxY) {
            minY -= 1;
            maxY += 1;
        }
        var pathPoints = [];
        for (var i = 0; i < series.length; i++) {
            var x = maxX === 0 ? 0 : (xValues[i] / maxX) * width;
            var normalized = (series[i].v - minY) / (maxY - minY);
            var y = height - (normalized * height);
            pathPoints.push("".concat(i === 0 ? 'M' : 'L').concat(x.toFixed(2), " ").concat(y.toFixed(2)));
        }
        var minLabel = "".concat(minY.toFixed(0), " ").concat(units);
        var maxLabel = "".concat(maxY.toFixed(0), " ").concat(units);
        var spanLabel = "".concat(Math.max(0, Math.round(maxX)), "s window");
        svg.setAttribute('viewBox', "0 0 ".concat(width, " ").concat(height));
        svg.setAttribute('preserveAspectRatio', 'none');
        svg.innerHTML = "\n      <rect x=\"0\" y=\"0\" width=\"".concat(width, "\" height=\"").concat(height, "\" fill=\"white\" stroke=\"#ddd\" />\n      <path d=\"").concat(pathPoints.join(' '), "\" stroke=\"").concat(color, "\" stroke-width=\"2\" fill=\"none\" />\n      <text x=\"4\" y=\"16\" fill=\"#555\" font-size=\"12\">").concat(maxLabel, "</text>\n      <text x=\"4\" y=\"").concat(height - 4, "\" fill=\"#555\" font-size=\"12\">").concat(minLabel, "</text>\n      <text x=\"").concat(width - 4, "\" y=\"").concat(height - 4, "\" fill=\"#888\" font-size=\"12\" text-anchor=\"end\">").concat(spanLabel, "</text>\n    ");
    }
    MonitorApp.renderLineChart = renderLineChart;
})(MonitorApp || (MonitorApp = {}));
/**
 * @file 3-main.ts
 * @brief Monitor page initialization
 */
var MonitorApp;
(function (MonitorApp) {
    document.addEventListener('DOMContentLoaded', function () {
        MonitorApp.initPolling();
    });
})(MonitorApp || (MonitorApp = {}));
