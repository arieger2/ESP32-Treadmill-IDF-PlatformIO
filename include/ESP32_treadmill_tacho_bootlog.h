/**
 * @file ESP32_treadmill_tacho_bootlog.h
 * @brief Boot log capture to LittleFS
 */

#ifndef ESP32_TREADMILL_TACHO_BOOTLOG_H
#define ESP32_TREADMILL_TACHO_BOOTLOG_H

#include <Arduino.h>

/**
 * Initialize boot log capture to LittleFS
 * Call early in setup() to capture all boot messages
 * @return true if successful, false on error
 */
bool initBootLog();

/**
 * Stop boot log capture and close file
 * Call at end of setup() to finalize log
 */
void stopBootLog();

/**
 * Read boot log file content
 * @return Log content as String, or error message if unavailable
 */
String readBootLog();

/**
 * Get boot log file size in bytes
 * @return File size, or 0 if file doesn't exist
 */
size_t getBootLogSize();

/**
 * Write message to both Serial and boot log
 * Use during setup() to capture output
 */
void logPrint(const char* message);

/**
 * Write formatted message to both Serial and boot log
 * Use during setup() to capture output
 */
void logPrintf(const char* format, ...);

#endif // ESP32_TREADMILL_TACHO_BOOTLOG_H
