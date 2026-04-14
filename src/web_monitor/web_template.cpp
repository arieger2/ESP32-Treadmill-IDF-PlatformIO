// ============================================================================
// Web Template Processor - Variable substitution for HTML pages
// ============================================================================

#include "ESP32_treadmill_tacho_web.h"
#include "ESP32_treadmill_tacho_config.h"

String processTemplate(const String& var) {
  if (var.length() == 0 || var.length() > 30) return String("Error");

  // Monitor placeholders
  if (var == "SPEED")           return getSpeed();
  if (var == "PACEMIN")         return getPaceMin();
  if (var == "PACESEC")         return getPaceSec();
  if (var == "MOTORRPM")        return getMotorRPM();
  if (var == "DISTANCE")        return getDistance();
  if (var == "DISTANCE_UNIT")   return getDistanceUnit();
  if (var == "HOUR")            return getHour();
  if (var == "MINUTE")          return getMinute();
  if (var == "SECOND")          return getSecond();
  if (var == "OFFSET")          return getOffset();
  if (var == "HEARTRATE")       return getHeartRate();
  if (var == "RR")              return getRR();
  if (var == "RPM")             return getRPM();
  if (var == "DATETIME")        return getDateTime();
  if (var == "TESTDATABUTTONTEXT") return getTestDataButtonText();

  // Settings placeholders
  if (var == "WIFI_SSID")          return storedGlobals.WIFI_SSID;
  if (var == "WIFI_PASSWORD")      return String(""); // Never send password to browser
  if (var == "BLE_DEVICE_NAME")    return storedGlobals.BLE_DEVICE_NAME;
  if (var == "INTERRUPT_PIN")      return String(storedGlobals.INTERRUPT_PIN);
  if (var == "MOTOR_INTERRUPT_PIN")return String(storedGlobals.MOTOR_INTERRUPT_PIN);

  if (var == "SPEED_UP_PIN")       return String(storedGlobals.SPEED_UP_PIN);
  if (var == "SPEED_DOWN_PIN")     return String(storedGlobals.SPEED_DOWN_PIN);
  if (var == "INCLINE_UP_PIN")     return String(storedGlobals.INCLINE_UP_PIN);
  if (var == "INCLINE_DOWN_PIN")   return String(storedGlobals.INCLINE_DOWN_PIN);
  if (var == "TESTDATA_FREQ")      return String(storedGlobals.TESTDATA_FREQ_MS);
  if (var == "BELT_DISTANCE")      return String(storedGlobals.BELT_DISTANCE_MM);
  if (var == "BELT_DISTANCE_MM")   return String(storedGlobals.BELT_DISTANCE_MM); // Legacy
  if (var == "DEBOUNCE_THRESHOLD") return String(storedGlobals.DEBOUNCE_THRESHOLD_US);
  if (var == "DEBOUNCE_THRESHOLD_US") return String(storedGlobals.DEBOUNCE_THRESHOLD_US); // Legacy
  if (var == "MAX_REVOLUTION_TIME")return String(storedGlobals.MAX_REVOLUTION_TIME_MS);
  if (var == "MAX_REVOLUTION_TIME_MS")return String(storedGlobals.MAX_REVOLUTION_TIME_MS); // Legacy
  if (var == "PULSES_PER_REV")     return String(storedGlobals.PULSES_PER_REV);
  if (var == "BAND_PULSE_MULTIPLIER") return String(storedGlobals.BAND_PULSE_MULTIPLIER);
  if (var == "MOTOR_PULSES_PER_REV")return String(storedGlobals.MOTOR_PULSES_PER_REV);
  if (var == "MOTOR_PULSE_MULTIPLIER") return String(storedGlobals.MOTOR_PULSE_MULTIPLIER);
  if (var == "MOTOR_TO_BELT_RATIO")return String(storedGlobals.MOTOR_TO_BELT_RATIO, 6);
  if (var == "SENSOR_SOURCE_MODE") return String((int)storedGlobals.SENSOR_SOURCE_MODE);
  if (var == "BAND_FILTER_TYPE")   return String((int)storedGlobals.BAND_FILTER_TYPE);
  if (var == "MOTOR_FILTER_TYPE")  return String((int)storedGlobals.MOTOR_FILTER_TYPE);

  if (var == "INERTIA_DELAY")      return String(storedGlobals.INERTIA_DELAY_MS);

  // PIDAJ controller parameters
  if (var == "PID_KP")                return String(storedGlobals.PID_Kp, 2);
  if (var == "PID_KI")                return String(storedGlobals.PID_Ki, 2);
  if (var == "PID_KA")                return String(storedGlobals.PID_Ka, 2);
  if (var == "PID_KJ")                return String(storedGlobals.PID_Kj, 2);
  if (var == "PID_DEAD_ZONE")         return String(storedGlobals.PID_DEAD_ZONE, 2);
  if (var == "PID_LONG_PRESS_THRESH") return String(storedGlobals.PID_LONG_PRESS_THRESH, 2);
  if (var == "PID_I_CLAMP")           return String(storedGlobals.PID_I_CLAMP, 1);
  if (var == "PID_PULSE_COOLDOWN")    return String(storedGlobals.PID_PULSE_COOLDOWN_MS);
  if (var == "PID_LONG_PRESS_MAX")    return String(storedGlobals.PID_LONG_PRESS_MAX_MS);
  if (var == "PID_COAST_THRESHOLD")   return String(storedGlobals.PID_COAST_THRESHOLD, 3);
  if (var == "PID_ERROR_BAND_ENTER")  return String(storedGlobals.PID_ERROR_BAND_ENTER_KMH, 2);
  if (var == "PID_ERROR_BAND_EXIT")   return String(storedGlobals.PID_ERROR_BAND_EXIT_KMH, 2);
  if (var == "CTRL_RESPONSE_DELAY")   return String(storedGlobals.CTRL_RESPONSE_DELAY_MS);
  if (var == "CTRL_BELT_RATE_UP")     return String(storedGlobals.CTRL_BELT_RATE_UP_KMHPS, 3);
  if (var == "CTRL_BELT_RATE_DOWN")   return String(storedGlobals.CTRL_BELT_RATE_DOWN_KMHPS, 3);
  if (var == "CTRL_INERTIA_UP")       return String(storedGlobals.CTRL_INERTIA_UP_KMH, 2);
  if (var == "CTRL_INERTIA_DOWN")     return String(storedGlobals.CTRL_INERTIA_DOWN_KMH, 2);

  if (var == "TESTDATABUTTONCLASS") return testdata ? "button-red" : "button-green";

  // Signal quality placeholders
  if (var == "SIGNAL_QUALITY")       return getSignalQuality();
  if (var == "SIGNAL_CV")            return getSignalCV();
  if (var == "SIGNAL_FREQUENCY")     return getSignalFrequency();
  if (var == "SIGNAL_QUALITY_CLASS") return getSignalQualityClass();

  return String("");
}
