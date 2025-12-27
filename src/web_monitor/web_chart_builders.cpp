// ============================================================================
// Web API - Chart builders for heart rate and RR data
// ============================================================================

#include "ESP32_treadmill_tacho_web.h"
#include "ESP32_treadmill_tacho_config.h"

static constexpr size_t kChartPointLimit = 180; // ~3 minutes @ 1Hz

// Template function for building chart series JSON
template<typename T, typename std::enable_if<std::is_same<T, HRValue>::value, int>::type = 0>
static String buildSeriesJson(const CircularBuffer<T, 5000>& buffer) {
  const size_t count = buffer.size();
  if (count == 0) return String("[]");
  
  const size_t start = (count > kChartPointLimit) ? (count - kChartPointLimit) : 0;
  const size_t pointCount = count - start;

  String out;
  out.reserve(pointCount * 28 + 2);
  out += '[';
  for (size_t i = start; i < count; ++i) {
    if (i > start) out += ',';
    T sample = buffer[(uint16_t)i];
    out += "{\"t\":";
    out += sample.timestamp;
    out += ",\"v\":";
    out += sample.hrValue;
    out += '}';
  }
  out += ']';
  return out;
}

template<typename T, typename std::enable_if<std::is_same<T, RRValue>::value, int>::type = 0>
static String buildSeriesJson(const CircularBuffer<T, 5000>& buffer) {
  const size_t count = buffer.size();
  if (count == 0) return String("[]");
  
  const size_t start = (count > kChartPointLimit) ? (count - kChartPointLimit) : 0;
  const size_t pointCount = count - start;

  String out;
  out.reserve(pointCount * 28 + 2);
  out += '[';
  for (size_t i = start; i < count; ++i) {
    if (i > start) out += ',';
    T sample = buffer[(uint16_t)i];
    out += "{\"t\":";
    out += sample.timestamp;
    out += ",\"v\":";
    out += sample.rrValue;
    out += '}';
  }
  out += ']';
  return out;
}

String buildHeartRateSeriesJson() {
  return buildSeriesJson(metrics.hrBuffer);
}

String buildRRSeriesJson() {
  return buildSeriesJson(metrics.rrBuffer);
}
