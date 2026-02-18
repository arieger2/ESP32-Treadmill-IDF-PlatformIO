var __awaiter = (this && this.__awaiter) || function (thisArg, _arguments, P, generator) {
    function adopt(value) { return value instanceof P ? value : new P(function (resolve) { resolve(value); }); }
    return new (P || (P = Promise))(function (resolve, reject) {
        function fulfilled(value) { try { step(generator.next(value)); } catch (e) { reject(e); } }
        function rejected(value) { try { step(generator["throw"](value)); } catch (e) { reject(e); } }
        function step(result) { result.done ? resolve(result.value) : adopt(result.value).then(fulfilled, rejected); }
        step((generator = generator.apply(thisArg, _arguments || [])).next());
    });
};
var __generator = (this && this.__generator) || function (thisArg, body) {
    var _ = { label: 0, sent: function() { if (t[0] & 1) throw t[1]; return t[1]; }, trys: [], ops: [] }, f, y, t, g = Object.create((typeof Iterator === "function" ? Iterator : Object).prototype);
    return g.next = verb(0), g["throw"] = verb(1), g["return"] = verb(2), typeof Symbol === "function" && (g[Symbol.iterator] = function() { return this; }), g;
    function verb(n) { return function (v) { return step([n, v]); }; }
    function step(op) {
        if (f) throw new TypeError("Generator is already executing.");
        while (g && (g = 0, op[0] && (_ = 0)), _) try {
            if (f = 1, y && (t = op[0] & 2 ? y["return"] : op[0] ? y["throw"] || ((t = y["return"]) && t.call(y), 0) : y.next) && !(t = t.call(y, op[1])).done) return t;
            if (y = 0, t) op = [op[0] & 2, t.value];
            switch (op[0]) {
                case 0: case 1: t = op; break;
                case 4: _.label++; return { value: op[1], done: false };
                case 5: _.label++; y = op[1]; op = [0]; continue;
                case 7: op = _.ops.pop(); _.trys.pop(); continue;
                default:
                    if (!(t = _.trys, t = t.length > 0 && t[t.length - 1]) && (op[0] === 6 || op[0] === 2)) { _ = 0; continue; }
                    if (op[0] === 3 && (!t || (op[1] > t[0] && op[1] < t[3]))) { _.label = op[1]; break; }
                    if (op[0] === 6 && _.label < t[1]) { _.label = t[1]; t = op; break; }
                    if (t && _.label < t[2]) { _.label = t[2]; _.ops.push(op); break; }
                    if (t[2]) _.ops.pop();
                    _.trys.pop(); continue;
            }
            op = body.call(thisArg, _);
        } catch (e) { op = [6, e]; y = 0; } finally { f = t = 0; }
        if (op[0] & 5) throw op[1]; return { value: op[0] ? op[1] : void 0, done: true };
    }
};
var __spreadArray = (this && this.__spreadArray) || function (to, from, pack) {
    if (pack || arguments.length === 2) for (var i = 0, l = from.length, ar; i < l; i++) {
        if (ar || !(i in from)) {
            if (!ar) ar = Array.prototype.slice.call(from, 0, i);
            ar[i] = from[i];
        }
    }
    return to.concat(ar || Array.prototype.slice.call(from));
};
/**
 * Configuration and Type Definitions
 * File prefixed with 0- to ensure it's compiled first
 */
var WorkoutApp;
(function (WorkoutApp) {
    /** Configuration constants */
    WorkoutApp.CONFIG = {
        ACTION_DELAY_MS: 150,
        BUTTON_COOLDOWN_MS: 200,
        POLL_INTERVAL_MS: 500,
        CANVAS_WIDTH: 860,
        CANVAS_HEIGHT: 180,
        CANVAS_PADDING: 15,
    };
})(WorkoutApp || (WorkoutApp = {}));
/**
 * Common Utilities
 * Shared functions and state management
 */
var WorkoutApp;
(function (WorkoutApp) {
    var state = {
        timeline: [],
        total: 0,
        pollInterval: null,
    };
    var errorMsgElement = null;
    var drawFunction = null;
    function setErrorElement(elem) {
        errorMsgElement = elem;
    }
    WorkoutApp.setErrorElement = setErrorElement;
    function setDrawFunction(fn) {
        drawFunction = fn;
    }
    WorkoutApp.setDrawFunction = setDrawFunction;
    /**
     * Converts km/h to pace string (mm:ss)
     */
    function kmhToPaceStr(kmh) {
        if (!kmh || kmh <= 0)
            return '--:--';
        var paceMin = 60 / kmh;
        var mm = Math.floor(paceMin);
        var ss = Math.round((paceMin - mm) * 60);
        if (ss >= 60)
            ss = 59;
        return "".concat(mm, ":").concat(ss < 10 ? '0' + ss : ss);
    }
    WorkoutApp.kmhToPaceStr = kmhToPaceStr;
    /**
     * Performs an action (POST request)
     */
    function doAction(btn, url, body) {
        return __awaiter(this, void 0, void 0, function () {
            var res, text, e_1;
            return __generator(this, function (_a) {
                switch (_a.label) {
                    case 0:
                        _a.trys.push([0, 6, 7, 8]);
                        btn.disabled = true;
                        return [4 /*yield*/, fetch(url, {
                                method: 'POST',
                                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                                body: body,
                                cache: 'no-store',
                            })];
                    case 1:
                        res = _a.sent();
                        if (!!res.ok) return [3 /*break*/, 3];
                        return [4 /*yield*/, res.text()];
                    case 2:
                        text = _a.sent();
                        if (errorMsgElement) {
                            errorMsgElement.textContent = "\u26A0\uFE0F Error: ".concat(text);
                            errorMsgElement.style.display = 'block';
                        }
                        return [2 /*return*/];
                    case 3: return [4 /*yield*/, new Promise(function (resolve) { return setTimeout(resolve, WorkoutApp.CONFIG.ACTION_DELAY_MS); })];
                    case 4:
                        _a.sent();
                        return [4 /*yield*/, poll()];
                    case 5:
                        _a.sent();
                        return [3 /*break*/, 8];
                    case 6:
                        e_1 = _a.sent();
                        if (errorMsgElement) {
                            errorMsgElement.textContent = "\u26A0\uFE0F Network error: ".concat(e_1 instanceof Error ? e_1.message : String(e_1));
                            errorMsgElement.style.display = 'block';
                        }
                        return [3 /*break*/, 8];
                    case 7:
                        setTimeout(function () {
                            btn.disabled = false;
                            btn.blur();
                        }, WorkoutApp.CONFIG.BUTTON_COOLDOWN_MS);
                        return [7 /*endfinally*/];
                    case 8: return [2 /*return*/];
                }
            });
        });
    }
    WorkoutApp.doAction = doAction;
    /**
     * Polls server for workout state
     */
    function poll() {
        return __awaiter(this, void 0, void 0, function () {
            var res, workoutState, e_2;
            return __generator(this, function (_a) {
                switch (_a.label) {
                    case 0:
                        _a.trys.push([0, 3, , 4]);
                        return [4 /*yield*/, fetch('/api/workout/state?full=1', { cache: 'no-store' })];
                    case 1:
                        res = _a.sent();
                        if (!res.ok)
                            return [2 /*return*/];
                        return [4 /*yield*/, res.json()];
                    case 2:
                        workoutState = _a.sent();
                        if (drawFunction) {
                            drawFunction(workoutState);
                        }
                        return [3 /*break*/, 4];
                    case 3:
                        e_2 = _a.sent();
                        return [3 /*break*/, 4];
                    case 4: return [2 /*return*/];
                }
            });
        });
    }
    WorkoutApp.poll = poll;
    /**
     * Fetches full workout and redraws
     */
    function fetchAndUpdateFullWorkout() {
        return __awaiter(this, void 0, void 0, function () {
            var res, workoutState, e_3;
            return __generator(this, function (_a) {
                switch (_a.label) {
                    case 0:
                        _a.trys.push([0, 3, , 4]);
                        return [4 /*yield*/, fetch('/api/workout/state?full=1', { cache: 'no-store' })];
                    case 1:
                        res = _a.sent();
                        if (!res.ok)
                            return [2 /*return*/];
                        return [4 /*yield*/, res.json()];
                    case 2:
                        workoutState = _a.sent();
                        if (drawFunction) {
                            drawFunction(workoutState);
                        }
                        return [3 /*break*/, 4];
                    case 3:
                        e_3 = _a.sent();
                        return [3 /*break*/, 4];
                    case 4: return [2 /*return*/];
                }
            });
        });
    }
    WorkoutApp.fetchAndUpdateFullWorkout = fetchAndUpdateFullWorkout;
    /**
     * Initializes polling
     */
    function initPolling() {
        fetchAndUpdateFullWorkout();
        state.pollInterval = window.setInterval(poll, WorkoutApp.CONFIG.POLL_INTERVAL_MS);
        window.addEventListener('beforeunload', function () {
            if (state.pollInterval !== null) {
                clearInterval(state.pollInterval);
            }
        });
    }
    WorkoutApp.initPolling = initPolling;
})(WorkoutApp || (WorkoutApp = {}));
/**
 * Workout Visualization
 * Canvas rendering and status display
 */
var WorkoutApp;
(function (WorkoutApp) {
    var canvas = null;
    var ctx = null;
    var stateSpan = null;
    var errorMsg = null;
    var paceNow = null;
    var btnStart = null;
    var btnPause = null;
    var btnResume = null;
    var btnStop = null;
    var timeline = [];
    var total = 0;
    function initView() {
        var _a;
        canvas = document.getElementById('woCanvas');
        ctx = (_a = canvas === null || canvas === void 0 ? void 0 : canvas.getContext('2d')) !== null && _a !== void 0 ? _a : null;
        stateSpan = document.getElementById('woState');
        errorMsg = document.getElementById('errorMsg');
        paceNow = document.getElementById('paceNow');
        btnStart = document.getElementById('btnStart');
        btnPause = document.getElementById('btnPause');
        btnResume = document.getElementById('btnResume');
        btnStop = document.getElementById('btnStop');
        WorkoutApp.setErrorElement(errorMsg);
        WorkoutApp.setDrawFunction(draw);
    }
    WorkoutApp.initView = initView;
    function draw(state) {
        if (!ctx || !canvas)
            return;
        if (state.steps && JSON.stringify(state.steps) !== JSON.stringify(timeline)) {
            timeline = state.steps;
            total = timeline.reduce(function (acc, step) { return acc + (step.d || 0); }, 0);
        }
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        var pad = WorkoutApp.CONFIG.CANVAS_PADDING;
        var w = canvas.width - pad * 2;
        var h = canvas.height - pad * 2;
        // Single X-axis at bottom - speed bars grow upward from it
        var xAxisY = pad + h; // Bottom line is the X-axis
        // Draw axes - only Y-axis and X-axis (no middle divider)
        ctx.strokeStyle = '#999';
        ctx.beginPath();
        ctx.moveTo(pad, pad); // Y-axis top
        ctx.lineTo(pad, xAxisY); // Y-axis bottom
        ctx.moveTo(pad, xAxisY); // X-axis start
        ctx.lineTo(pad + w, xAxisY); // X-axis end
        ctx.stroke();
        if (!total) {
            if (stateSpan)
                stateSpan.textContent = "State: ".concat(state.state || 'Idle');
            if (paceNow)
                paceNow.textContent = '--:--';
            updateButtonStates(state, false);
            return;
        }
        var maxV = Math.max.apply(Math, __spreadArray([1], timeline.map(function (s) { return s.v || 0; }), false));
        var t0 = 0;
        for (var _i = 0, timeline_1 = timeline; _i < timeline_1.length; _i++) {
            var step = timeline_1[_i];
            var x0 = pad + (t0 / total) * w;
            var x1 = pad + ((t0 + step.d) / total) * w;
            var barW = x1 - x0 - 1;
            // Speed bar - grows UP from the X-axis (bottom), using full height
            var vh = maxV > 0 ? (step.v / maxV) * (h - 16) : 0; // Leave space for label at top
            ctx.fillStyle = '#4a90e2';
            ctx.fillRect(x0, xAxisY - vh, barW, vh);
            // Pace label - horizontal for wide bars, rotated for narrow bars
            if (barW >= 26) {
                var cx = x0 + barW / 2;
                var labelY = xAxisY - vh - 2;
                ctx.fillStyle = '#000';
                ctx.font = '11px sans-serif';
                ctx.textAlign = 'center';
                ctx.textBaseline = 'bottom';
                ctx.fillText(WorkoutApp.kmhToPaceStr(step.v || 0), cx, labelY);
            }
            else if (barW >= 8 && vh >= 20) {
                // Narrow bar: rotate text -90° and place inside/above bar
                var cx = x0 + barW / 2;
                var labelY = xAxisY - vh - 4;
                ctx.save();
                ctx.translate(cx, labelY);
                ctx.rotate(-Math.PI / 2);
                ctx.fillStyle = '#000';
                ctx.font = '11px sans-serif';
                ctx.textAlign = 'left';
                ctx.textBaseline = 'middle';
                ctx.fillText(WorkoutApp.kmhToPaceStr(step.v || 0), 0, 0);
                ctx.restore();
            }
            t0 += step.d;
        }
        // Current position marker
        if (state.elapsed_s) {
            var xn = pad + (state.elapsed_s / total) * w;
            ctx.strokeStyle = '#d0021b';
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.moveTo(xn, pad);
            ctx.lineTo(xn, pad + h);
            ctx.stroke();
            ctx.lineWidth = 1;
        }
        var vNow = Number(state.speed_kph || 0);
        if (paceNow)
            paceNow.textContent = WorkoutApp.kmhToPaceStr(vNow);
        if (stateSpan) {
            stateSpan.textContent =
                "State: ".concat(state.state, " | Step ").concat(state.step_index + 1, "/").concat(timeline.length, " ") +
                    "| Speed ".concat(vNow.toFixed(1), " km/h | Pace ").concat(WorkoutApp.kmhToPaceStr(vNow), " ") +
                    "| Incline ".concat(Number(state.incline_pct || 0).toFixed(1), " %");
        }
        if (errorMsg) {
            if (state.error && state.error.length > 0) {
                errorMsg.textContent = "\u26A0\uFE0F Error: ".concat(state.error);
                errorMsg.style.display = 'block';
            }
            else {
                errorMsg.style.display = 'none';
            }
        }
        updateButtonStates(state, timeline && timeline.length > 0);
    }
    function updateButtonStates(state, hasWorkout) {
        var isRunning = state.state === 'Running';
        var isPaused = state.state === 'Paused';
        if (btnStart)
            btnStart.disabled = !hasWorkout || (isRunning || isPaused);
        if (btnPause)
            btnPause.disabled = !isRunning;
        if (btnResume)
            btnResume.disabled = !isPaused;
        if (btnStop)
            btnStop.disabled = !(isRunning || isPaused);
    }
})(WorkoutApp || (WorkoutApp = {}));
/**
 * File Upload and Drag & Drop
 */
var WorkoutApp;
(function (WorkoutApp) {
    var fileInput = null;
    var uploadStatus = null;
    var canvas = null;
    function initInput() {
        fileInput = document.getElementById('zwoFile');
        uploadStatus = document.getElementById('uploadStatus');
        canvas = document.getElementById('woCanvas');
        setupFileInput();
        setupDragAndDrop();
    }
    WorkoutApp.initInput = initInput;
    function uploadZwo(file) {
        return __awaiter(this, void 0, void 0, function () {
            var fd, res, text, e_4;
            return __generator(this, function (_a) {
                switch (_a.label) {
                    case 0:
                        if (!file)
                            return [2 /*return*/];
                        _a.label = 1;
                    case 1:
                        _a.trys.push([1, 6, , 7]);
                        if (uploadStatus)
                            uploadStatus.textContent = 'Uploading...';
                        fd = new FormData();
                        fd.append('file', file, file.name);
                        return [4 /*yield*/, fetch("/api/workout/upload?t=".concat(Date.now()), {
                                method: 'POST',
                                body: fd,
                            })];
                    case 2:
                        res = _a.sent();
                        if (!!res.ok) return [3 /*break*/, 4];
                        return [4 /*yield*/, res.text()];
                    case 3:
                        text = _a.sent();
                        if (uploadStatus)
                            uploadStatus.textContent = "Upload failed: ".concat(text);
                        return [2 /*return*/];
                    case 4:
                        if (uploadStatus)
                            uploadStatus.textContent = 'Workout loaded ✔';
                        return [4 /*yield*/, WorkoutApp.fetchAndUpdateFullWorkout()];
                    case 5:
                        _a.sent();
                        return [3 /*break*/, 7];
                    case 6:
                        e_4 = _a.sent();
                        if (uploadStatus) {
                            uploadStatus.textContent = "Upload error: ".concat(e_4 instanceof Error ? e_4.message : String(e_4));
                        }
                        return [3 /*break*/, 7];
                    case 7: return [2 /*return*/];
                }
            });
        });
    }
    function setupFileInput() {
        if (!fileInput)
            return;
        fileInput.addEventListener('change', function () {
            var _a;
            var file = (_a = fileInput === null || fileInput === void 0 ? void 0 : fileInput.files) === null || _a === void 0 ? void 0 : _a[0];
            if (file)
                uploadZwo(file);
        });
    }
    function setupDragAndDrop() {
        var _a;
        if (!canvas)
            return;
        var dropOverlay = document.createElement('div');
        dropOverlay.style.cssText = "\n      position: absolute;\n      left: 0; top: 0; right: 0; bottom: 0;\n      display: none;\n      align-items: center;\n      justify-content: center;\n      background: rgba(0,0,0,0.05);\n      border: 2px dashed #999;\n      font: 14px/1.4 sans-serif;\n      color: #333;\n      pointer-events: none;\n    ";
        dropOverlay.textContent = 'Drop .zwo / .xml file here';
        var wrapper = document.createElement('div');
        wrapper.style.position = 'relative';
        (_a = canvas.parentNode) === null || _a === void 0 ? void 0 : _a.insertBefore(wrapper, canvas);
        wrapper.appendChild(canvas);
        wrapper.appendChild(dropOverlay);
        ['dragover', 'drop'].forEach(function (type) {
            window.addEventListener(type, function (e) { return e.preventDefault(); });
        });
        var dragDepth = 0;
        canvas.addEventListener('dragenter', function (e) {
            e.preventDefault();
            e.stopPropagation();
            dragDepth++;
            dropOverlay.style.display = 'flex';
        });
        canvas.addEventListener('dragover', function (e) {
            e.preventDefault();
            e.stopPropagation();
            if (e.dataTransfer)
                e.dataTransfer.dropEffect = 'copy';
            dropOverlay.style.display = 'flex';
        });
        canvas.addEventListener('dragleave', function (e) {
            e.preventDefault();
            e.stopPropagation();
            dragDepth = Math.max(0, dragDepth - 1);
            if (dragDepth === 0)
                dropOverlay.style.display = 'none';
        });
        canvas.addEventListener('drop', function (e) {
            var _a, _b;
            e.preventDefault();
            e.stopPropagation();
            dragDepth = 0;
            dropOverlay.style.display = 'none';
            var file = (_b = (_a = e.dataTransfer) === null || _a === void 0 ? void 0 : _a.files) === null || _b === void 0 ? void 0 : _b[0];
            if (file)
                uploadZwo(file);
        });
    }
})(WorkoutApp || (WorkoutApp = {}));
/**
 * Control Buttons
 * Start/Pause/Resume/Stop and Pace controls
 */
var WorkoutApp;
(function (WorkoutApp) {
    function initControl() {
        var thresholdInput = document.getElementById('thresholdInput');
        var btnThreshold = document.getElementById('btnThreshold');
        var thresholdHint = document.getElementById('thresholdHint');
        // Convert seconds/km to "m:ss" display string
        function secToMinKm(sec) {
            if (sec <= 0)
                return '';
            var m = Math.floor(sec / 60);
            var s = Math.round(sec % 60);
            return "".concat(m, ":").concat(s < 10 ? '0' + s : s);
        }
        // Parse "m:ss" to seconds, returns NaN on bad input
        function minKmToSec(txt) {
            var p = txt.trim().split(':');
            if (p.length !== 2)
                return NaN;
            var m = parseInt(p[0], 10), s = parseInt(p[1], 10);
            if (isNaN(m) || isNaN(s) || s < 0 || s > 59 || m < 0)
                return NaN;
            return m * 60 + s;
        }
        // Fetch current threshold from backend and update the field
        function refreshThreshold() {
            fetch('/api/workout/threshold')
                .then(function (r) { return r.text(); })
                .then(function (tv) {
                var sec = parseFloat(tv);
                if (isNaN(sec) || sec <= 0)
                    return;
                if (thresholdInput)
                    thresholdInput.value = secToMinKm(sec);
                if (thresholdHint)
                    thresholdHint.textContent =
                        "".concat(secToMinKm(sec), " min/km \u2248 ").concat((3600 / sec).toFixed(1), " km/h bei IF=1.0");
            }).catch(function () { });
        }
        // SET: send typed threshold to backend (backend rescales steps + stores)
        function applyThreshold() {
            var _a;
            var sec = minKmToSec((_a = thresholdInput === null || thresholdInput === void 0 ? void 0 : thresholdInput.value) !== null && _a !== void 0 ? _a : '');
            if (isNaN(sec) || sec <= 0) {
                if (thresholdHint)
                    thresholdHint.textContent = '\u26a0\ufe0f Format: mm:ss (z.B. 5:30)';
                return;
            }
            WorkoutApp.doAction(btnThreshold, '/api/workout/threshold', "val=".concat(sec))
                .then(function () { return refreshThreshold(); });
        }
        btnThreshold === null || btnThreshold === void 0 ? void 0 : btnThreshold.addEventListener('click', applyThreshold);
        thresholdInput === null || thresholdInput === void 0 ? void 0 : thresholdInput.addEventListener('keydown', function (e) {
            if (e.key === 'Enter')
                applyThreshold();
        });
        // -pace / +pace: backend scales steps AND threshold together
        var paceDown = document.getElementById('paceDown');
        var paceUp = document.getElementById('paceUp');
        paceDown === null || paceDown === void 0 ? void 0 : paceDown.addEventListener('click', function () {
            return WorkoutApp.doAction(paceDown, '/api/workout/scale/nudge', 'rel=-0.01').then(function () { return refreshThreshold(); });
        });
        paceUp === null || paceUp === void 0 ? void 0 : paceUp.addEventListener('click', function () {
            return WorkoutApp.doAction(paceUp, '/api/workout/scale/nudge', 'rel=+0.01').then(function () { return refreshThreshold(); });
        });
        var btnStart = document.getElementById('btnStart');
        var btnPause = document.getElementById('btnPause');
        var btnResume = document.getElementById('btnResume');
        var btnStop = document.getElementById('btnStop');
        btnStart === null || btnStart === void 0 ? void 0 : btnStart.addEventListener('click', function () { return WorkoutApp.doAction(btnStart, '/api/workout/control', 'action=start'); });
        btnPause === null || btnPause === void 0 ? void 0 : btnPause.addEventListener('click', function () { return WorkoutApp.doAction(btnPause, '/api/workout/control', 'action=pause'); });
        btnResume === null || btnResume === void 0 ? void 0 : btnResume.addEventListener('click', function () { return WorkoutApp.doAction(btnResume, '/api/workout/control', 'action=resume'); });
        btnStop === null || btnStop === void 0 ? void 0 : btnStop.addEventListener('click', function () { return WorkoutApp.doAction(btnStop, '/api/workout/control', 'action=stop'); });
        refreshThreshold();
    }
    WorkoutApp.initControl = initControl;
})(WorkoutApp || (WorkoutApp = {}));
/**
 * Manual Test Controls
 * GPIO control buttons
 */
var WorkoutApp;
(function (WorkoutApp) {
    function initTest() {
        var speedDown = document.getElementById('speedDown');
        var speedUp = document.getElementById('speedUp');
        var inclineDown = document.getElementById('inclineDown');
        var inclineUp = document.getElementById('inclineUp');
        speedDown === null || speedDown === void 0 ? void 0 : speedDown.addEventListener('click', function () {
            return WorkoutApp.doAction(speedDown, '/api/test/speed/down', '');
        });
        speedUp === null || speedUp === void 0 ? void 0 : speedUp.addEventListener('click', function () {
            return WorkoutApp.doAction(speedUp, '/api/test/speed/up', '');
        });
        inclineDown === null || inclineDown === void 0 ? void 0 : inclineDown.addEventListener('click', function () {
            return WorkoutApp.doAction(inclineDown, '/api/test/incline/down', '');
        });
        inclineUp === null || inclineUp === void 0 ? void 0 : inclineUp.addEventListener('click', function () {
            return WorkoutApp.doAction(inclineUp, '/api/test/incline/up', '');
        });
    }
    WorkoutApp.initTest = initTest;
})(WorkoutApp || (WorkoutApp = {}));
/**
 * Main Entry Point
 * Initializes all modules
 */
var WorkoutApp;
(function (WorkoutApp) {
    document.addEventListener('DOMContentLoaded', function () {
        WorkoutApp.initView();
        WorkoutApp.initInput();
        WorkoutApp.initControl();
        WorkoutApp.initTest();
        WorkoutApp.initPolling();
    });
})(WorkoutApp || (WorkoutApp = {}));
