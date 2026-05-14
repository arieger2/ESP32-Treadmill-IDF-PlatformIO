#include <Arduino.h>
#include "ESP32_treadmill_tacho_workout.h"

static bool _extractAttr(const String& src, const char* key, String& outVal) {
  String k = String(key) + "=\"";
  int pos = src.indexOf(k);
  if (pos < 0) {
    String lower = src; lower.toLowerCase();
    String kl = String(key); kl.toLowerCase();
    String pat = kl + "=\"";
    int p = lower.indexOf(pat);
    if (p < 0) return false;
    int q1 = p + pat.length();
    int q2 = lower.indexOf('\"', q1);
    if (q2 < 0) return false;
    outVal = src.substring(q1, q2);
    return true;
  }
  int q1 = pos + k.length();
  int q2 = src.indexOf('\"', q1);
  if (q2 < 0) return false;
  outVal = src.substring(q1, q2);
  return true;
}

bool WorkoutExecutor::_getAttrF(const String& tag, const char* key, float& outVal) {
  String v; if (!_extractAttr(tag, key, v)) return false;
  outVal = v.toFloat(); return true;
}

bool WorkoutExecutor::_getAttrU32(const String& tag, const char* key, uint32_t& outVal) {
  String v; if (!_extractAttr(tag, key, v)) return false;
  long t = v.toInt(); if (t < 0) t = 0; outVal = (uint32_t)t; return true;
}

float WorkoutExecutor::paceToSpeedKph(float paceMin, int units) {
  if (paceMin <= 0) return 0.0f;
  if (units == 0) {
    const float km_per_mile = 1.609344f;
    return (60.0f / paceMin) * km_per_mile;
  }
  return 60.0f / paceMin;
}

float WorkoutExecutor::powerFracToSpeedKph(float frac) const {
  if (frac <= 0) return 0.0f;
  if (zwo_threshold_sec_per_km_ > 0.0f)
    return frac * (3600.0f / zwo_threshold_sec_per_km_);
  return frac * speedAtIF1_kph;
}

void WorkoutExecutor::scaleAllSpeeds(float factor) {
  if (factor <= 0.0f) return;
  for (auto& s : _steps)
    s.speed_kph = _clamp(s.speed_kph * factor, minSpeed_kph, maxSpeed_kph);
  if (_state == WorkoutState::Running && _currentIndex < _steps.size())
    _applyTargets(_steps[_currentIndex]);
}

bool WorkoutExecutor::_parseZwo(const String& xml) {
  _steps.reserve(32);

  auto push = [this](uint32_t dur, float spKph, float incl, const char* label) {
    if (!dur) return;
    WorkoutStep s;
    s.duration_value = dur; s.speed_kph = spKph; s.incline_pct = incl;
    s.label = label ? label : "";
    this->_steps.push_back(s);
  };
  auto pushRamp = [&](uint32_t dur, float v0, float v1, float incl, const char* label) {
    if (!dur) return;
    const uint32_t segs = (dur >= 240) ? 16 : (dur >= 120 ? 12 : (dur >= 60 ? 8 : 4));
    uint32_t base = dur / segs, rem = dur % segs;
    for (uint32_t i = 0; i < segs; ++i) {
      float t  = (segs == 1) ? 1.0f : (float)i / (float)(segs - 1);
      float sp = v0 + (v1 - v0) * t;
      push(base + (i < rem ? 1U : 0U), sp, incl, label);
    }
  };

  int pos = 0, iterCount = 0;
  while (true) {
    if (++iterCount % 100 == 0) yield();
    int lt = xml.indexOf('<', pos); if (lt < 0) break;
    int gt = xml.indexOf('>', lt + 1); if (gt < 0) break;

    String tagOpen = xml.substring(lt + 1, gt); tagOpen.trim();
    if (tagOpen.startsWith("/")) { pos = gt + 1; continue; }

    int sp = tagOpen.indexOf(' ');
    String name = (sp >= 0) ? tagOpen.substring(0, sp) : tagOpen;
    String nameLower = name; nameLower.toLowerCase();

    bool selfClosed = tagOpen.endsWith("/");
    if (!selfClosed) {
      String closeSeq = "</" + name + ">";
      int close = xml.indexOf(closeSeq, gt + 1);
      (void)close;
    }

    String attrs = "<" + tagOpen + ">";

    if (nameLower == "steadystate") {
      uint32_t dur = 0; float spd = 0, inc = 0, pwr = 0;
      bool hasDur = _getAttrU32(attrs, "Duration", dur);
      bool hasSp  = _getAttrF(attrs, "Speed",   spd);
      bool hasInc = _getAttrF(attrs, "Incline", inc);
      bool hasPwr = _getAttrF(attrs, "Power",   pwr);
      if (hasDur && dur) {
        float speedKph = hasSp ? spd : (hasPwr ? powerFracToSpeedKph(pwr) : minSpeed_kph);
        push(dur, speedKph, hasInc ? inc : 0.0f, "SteadyState");
      }
    }
    else if (nameLower == "intervalst") {
      uint32_t rep = 0, onDur = 0, offDur = 0;
      float onSp = 0, offSp = 0, onInc = 0, offInc = 0, onP = 0, offP = 0;
      bool hasOnSp   = _getAttrF(attrs,   "OnSpeed",    onSp);
      bool hasOffSp  = _getAttrF(attrs,   "OffSpeed",   offSp);
      bool hasOnInc  = _getAttrF(attrs,   "OnIncline",  onInc);
      bool hasOffInc = _getAttrF(attrs,   "OffIncline", offInc);
      bool hasOnP    = _getAttrF(attrs,   "OnPower",    onP);
      bool hasOffP   = _getAttrF(attrs,   "OffPower",   offP);
      _getAttrU32(attrs, "Repeat",      rep);
      _getAttrU32(attrs, "OnDuration",  onDur);
      _getAttrU32(attrs, "OffDuration", offDur);
      if (rep && (onDur || offDur)) {
        float onSpeed  = hasOnSp  ? onSp  : (hasOnP  ? powerFracToSpeedKph(onP)  : minSpeed_kph);
        float offSpeed = hasOffSp ? offSp : (hasOffP ? powerFracToSpeedKph(offP) : minSpeed_kph);
        for (uint32_t i = 0; i < rep; ++i) {
          if (onDur)  push(onDur,  onSpeed,  hasOnInc  ? onInc  : 0.0f, "IntervalsT-ON");
          if (offDur) push(offDur, offSpeed, hasOffInc ? offInc : 0.0f, "IntervalsT-OFF");
        }
      }
    }
    else if (nameLower == "warmup" || nameLower == "cooldown") {
      uint32_t dur = 0; float pl = 0, ph = 0, sl = 0, sh = 0, inc = 0;
      bool hasDur = _getAttrU32(attrs, "Duration",  dur);
      bool hasPL  = _getAttrF(attrs,   "PowerLow",  pl);
      bool hasPH  = _getAttrF(attrs,   "PowerHigh", ph);
      bool hasSL  = _getAttrF(attrs,   "SpeedLow",  sl);
      bool hasSH  = _getAttrF(attrs,   "SpeedHigh", sh);
      bool hasInc = _getAttrF(attrs,   "Incline",   inc);
      if (hasDur && dur) {
        float v0 = hasSL ? sl : (hasPL ? powerFracToSpeedKph(pl) : minSpeed_kph);
        float v1 = hasSH ? sh : (hasPH ? powerFracToSpeedKph(ph) : v0);
        pushRamp(dur, v0, v1, hasInc ? inc : 0.0f,
                 nameLower == "warmup" ? "Warmup" : "Cooldown");
      }
    }

    pos = gt + 1;
  }
  return true;
}
