// ============================================================================
// File   : ESP32_treadmill_tacho_filters.h
// Purpose: Speed filtering algorithms - EMA, Kalman, Median
// ============================================================================
#ifndef ESP32_TREADMILL_TACHO_FILTERS_H
#define ESP32_TREADMILL_TACHO_FILTERS_H

#include <Arduino.h>

// Filter types
enum SpeedFilterType : uint8_t {
    FILTER_NONE = 0,      // No filtering (raw values)
    FILTER_EMA = 1,       // Exponential Moving Average (default)
    FILTER_KALMAN = 2,    // Kalman filter (optimal for noisy sensors)
    FILTER_MEDIAN = 3     // Median filter (best noise rejection)
};

// ============================================================================
// EXPONENTIAL MOVING AVERAGE FILTER
// ============================================================================
class EMAFilter {
private:
    float smoothed;
    bool initialized;
    
public:
    EMAFilter();
    float update(float newValue, bool isDecelerating);
    void reset();
};

// ============================================================================
// KALMAN FILTER
// ============================================================================
class KalmanFilter {
private:
    float x;          // State estimate (speed)
    float P;          // Error covariance
    float Q;          // Process noise covariance (how much we trust the model)
    float R;          // Measurement noise covariance (how much we trust the sensor)
    float K;          // Kalman gain
    bool initialized;
    
public:
    KalmanFilter();
    float update(float measurement);
    void reset();
    void setProcessNoise(float q);
    void setMeasurementNoise(float r);
};

// ============================================================================
// MEDIAN FILTER
// ============================================================================
class MedianFilter {
private:
    static const uint8_t WINDOW_SIZE = 5;  // Must be odd number
    float buffer[WINDOW_SIZE];
    uint8_t index;
    uint8_t count;
    
    float getMedian();
    
public:
    MedianFilter();
    float update(float newValue);
    void reset();
};

// ============================================================================
// FILTER MANAGER
// ============================================================================
class SpeedFilter {
private:
    EMAFilter ema;
    KalmanFilter kalman;
    MedianFilter median;
    SpeedFilterType currentType;
    
public:
    SpeedFilter();
    
    // Set the active filter type
    void setFilterType(SpeedFilterType type);
    
    // Update with new value
    float update(float newValue, bool isDecelerating = false);
    
    // Reset filter state
    void reset();
    
    // Get current filter type
    SpeedFilterType getFilterType() const { return currentType; }
};

#endif // ESP32_TREADMILL_TACHO_FILTERS_H
