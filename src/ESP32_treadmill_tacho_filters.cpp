// ============================================================================
// File   : ESP32_treadmill_tacho_filters.cpp
// Purpose: Speed filtering algorithm implementations
// ============================================================================
#include "ESP32_treadmill_tacho_filters.h"
#include <algorithm>  // for std::sort

// ============================================================================
// EXPONENTIAL MOVING AVERAGE FILTER IMPLEMENTATION
// ============================================================================
EMAFilter::EMAFilter() : smoothed(0.0f), initialized(false) {}

float EMAFilter::update(float newValue, bool isDecelerating) {
    // Asymmetric alpha: faster response to deceleration (safety), slower for acceleration (stability)
    const float alpha_accel = 0.25f;   // Faster smoothing for startup (was 0.15f)
    const float alpha_decel = 0.35f;   // Faster response to decreases (was 0.30f)
    const float alpha_warmup = 0.50f;  // Even faster during initial warm-up
    
    // Reset if new value is near zero
    if (newValue < 0.05f) {
        smoothed = 0.0f;
        initialized = false;
        return 0.0f;
    }
    
    // First-time initialization: use faster convergence for first few samples
    if (!initialized || smoothed < 0.01f) {
        smoothed = newValue;
        initialized = true;
        return newValue;
    }
    
    // Use faster alpha during initial warm-up (when smoothed is very different from new value)
    float diff_ratio = fabsf(newValue - smoothed) / (smoothed + 0.1f);
    float alpha;
    
    if (diff_ratio > 0.3f) {
        // Large difference - likely still warming up, use faster convergence
        alpha = alpha_warmup;
    } else {
        // Normal operation - choose alpha based on whether speed is increasing or decreasing
        alpha = isDecelerating ? alpha_decel : alpha_accel;
    }
    
    smoothed = alpha * newValue + (1.0f - alpha) * smoothed;
    return smoothed;
}

void EMAFilter::reset() {
    smoothed = 0.0f;
    initialized = false;
}

// ============================================================================
// KALMAN FILTER IMPLEMENTATION
// ============================================================================
KalmanFilter::KalmanFilter() 
    : x(0.0f), P(1.0f), Q(0.01f), R(0.1f), K(0.0f), initialized(false) {
    // Q (process noise): Lower = trust the model more (smoother but slower response)
    // R (measurement noise): Lower = trust measurements more (faster but noisier)
    // Default values tuned for treadmill speed (1Hz updates, ~0.5 m/s^2 typical acceleration)
}

float KalmanFilter::update(float measurement) {
    // Reset if measurement is near zero
    if (measurement < 0.05f) {
        x = 0.0f;
        P = 1.0f;
        initialized = false;
        return 0.0f;
    }
    
    // Initialize with first measurement
    if (!initialized) {
        x = measurement;
        P = 1.0f;
        initialized = true;
        return measurement;
    }
    
    // Prediction step
    // x_pred = x (no model, assume constant speed)
    // P_pred = P + Q
    P = P + Q;
    
    // Update step
    // Kalman gain: K = P_pred / (P_pred + R)
    K = P / (P + R);
    
    // State update: x = x_pred + K * (measurement - x_pred)
    x = x + K * (measurement - x);
    
    // Error covariance update: P = (1 - K) * P_pred
    P = (1.0f - K) * P;
    
    return x;
}

void KalmanFilter::reset() {
    x = 0.0f;
    P = 1.0f;
    K = 0.0f;
    initialized = false;
}

void KalmanFilter::setProcessNoise(float q) {
    Q = q;
}

void KalmanFilter::setMeasurementNoise(float r) {
    R = r;
}

// ============================================================================
// MEDIAN FILTER IMPLEMENTATION
// ============================================================================
MedianFilter::MedianFilter() : index(0), count(0) {
    for (uint8_t i = 0; i < WINDOW_SIZE; i++) {
        buffer[i] = 0.0f;
    }
}

float MedianFilter::update(float newValue) {
    // Reset if new value is near zero
    if (newValue < 0.05f) {
        reset();
        return 0.0f;
    }
    
    // If this is first real value after zeros, fill buffer with it
    // This prevents median being influenced by initial zeros
    if (count > 0 && count < WINDOW_SIZE) {
        // Check if buffer has mostly zeros
        uint8_t zero_count = 0;
        for (uint8_t i = 0; i < count; i++) {
            if (buffer[i] < 0.05f) zero_count++;
        }
        // If more than half are zeros, pre-fill with current value
        if (zero_count > count / 2) {
            for (uint8_t i = 0; i < WINDOW_SIZE; i++) {
                buffer[i] = newValue;
            }
            index = 0;
            count = WINDOW_SIZE;
            return newValue;
        }
    }
    
    // Add new value to circular buffer
    buffer[index] = newValue;
    index = (index + 1) % WINDOW_SIZE;
    
    // Track how many values we have
    if (count < WINDOW_SIZE) {
        count++;
        // Return raw value until buffer is full
        return newValue;
    }
    
    // Buffer is full, return median
    return getMedian();
}

float MedianFilter::getMedian() {
    // Create temporary array and sort it
    float sorted[WINDOW_SIZE];
    for (uint8_t i = 0; i < WINDOW_SIZE; i++) {
        sorted[i] = buffer[i];
    }
    
    // Simple bubble sort (efficient for small arrays)
    for (uint8_t i = 0; i < WINDOW_SIZE - 1; i++) {
        for (uint8_t j = 0; j < WINDOW_SIZE - i - 1; j++) {
            if (sorted[j] > sorted[j + 1]) {
                float temp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = temp;
            }
        }
    }
    
    // Return middle value (median)
    return sorted[WINDOW_SIZE / 2];
}

void MedianFilter::reset() {
    index = 0;
    count = 0;
    for (uint8_t i = 0; i < WINDOW_SIZE; i++) {
        buffer[i] = 0.0f;
    }
}

// ============================================================================
// FILTER MANAGER IMPLEMENTATION
// ============================================================================
SpeedFilter::SpeedFilter() : currentType(FILTER_EMA) {}

void SpeedFilter::setFilterType(SpeedFilterType type) {
    if (type != currentType) {
        // Only reset filters when switching between actual filters (not to/from NONE)
        if (type != FILTER_NONE && currentType != FILTER_NONE) {
            // Switching between different filters - reset all
            ema.reset();
            kalman.reset();
            median.reset();
        } else if (type != FILTER_NONE) {
            // Switching from NONE to a filter - only reset the target filter
            switch (type) {
                case FILTER_EMA:    ema.reset();    break;
                case FILTER_KALMAN: kalman.reset(); break;
                case FILTER_MEDIAN: median.reset(); break;
                default: break;
            }
        }
        // If switching to FILTER_NONE, no reset needed (filters won't be used)
        currentType = type;
    }
}

float SpeedFilter::update(float newValue, bool isDecelerating) {
    switch (currentType) {
        case FILTER_NONE:
            return newValue;
            
        case FILTER_EMA:
            return ema.update(newValue, isDecelerating);
            
        case FILTER_KALMAN:
            return kalman.update(newValue);
            
        case FILTER_MEDIAN:
            return median.update(newValue);
            
        default:
            return newValue;
    }
}

void SpeedFilter::reset() {
    ema.reset();
    kalman.reset();
    median.reset();
}
