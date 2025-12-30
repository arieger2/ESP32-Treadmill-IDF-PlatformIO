/**
 * @file ESP32_treadmill_tacho_bootlog.cpp
 * @brief Boot log capture to LittleFS
 */

#include <Arduino.h>
#include "esp_littlefs.h"
#include "esp_log.h"
#include <cstring>
#include <stdio.h>

#define BOOT_LOG_FILE "/littlefs/boot.log"
#define MAX_LOG_SIZE 65536  // 64KB max log size

static FILE *bootLogFile = nullptr;
static bool logFileOpen = false;
static size_t logSize = 0;
static bool filesystemMounted = false;

/**
 * Custom vprintf that writes to both Serial and log file
 */
static int logVprintf(const char *fmt, va_list args) {
    // Write to Serial
    int ret = vprintf(fmt, args);
    
    // Also write to log file if open and under size limit
    if (logFileOpen && bootLogFile && logSize < MAX_LOG_SIZE) {
        va_list args_copy;
        va_copy(args_copy, args);
        
        char buffer[256];
        int len = vsnprintf(buffer, sizeof(buffer), fmt, args_copy);
        va_end(args_copy);
        
        if (len > 0) {
            size_t toWrite = len;
            if (logSize + len > MAX_LOG_SIZE) {
                toWrite = MAX_LOG_SIZE - logSize;
            }
            
            if (toWrite > 0) {
                size_t written = fwrite(buffer, 1, toWrite, bootLogFile);
                logSize += written;
                
                // Flush periodically
                if (logSize % 512 < toWrite) {
                    fflush(bootLogFile);
                }
            }
        }
    }
    
    return ret;
}

/**
 * Initialize boot log capture
 * Must be called early in setup() to capture all logs
 */
bool initBootLog() {
    // Mount LittleFS using ESP-IDF VFS
    Serial.println("Mounting LittleFS for boot log...");
    
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            Serial.println("⚠ LittleFS mount failed");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            Serial.println("⚠ LittleFS partition not found");
        } else {
            Serial.printf("⚠ LittleFS init failed: %s\n", esp_err_to_name(ret));
        }
        return false;
    }
    
    filesystemMounted = true;
    Serial.println("LittleFS mounted successfully");
    
    // Delete old log file
    remove(BOOT_LOG_FILE);
    
    // Open new log file for writing
    bootLogFile = fopen(BOOT_LOG_FILE, "w");
    if (!bootLogFile) {
        Serial.println("⚠ Failed to create boot log file");
        return false;
    }
    
    logFileOpen = true;
    logSize = 0;
    
    // Write header
    const char *header = "=== ESP32 Treadmill Boot Log ===\n";
    size_t written = fwrite(header, 1, strlen(header), bootLogFile);
    logSize += written;
    fflush(bootLogFile);
    
    Serial.println("✓ Boot log capture enabled → /littlefs/boot.log");
    
    // Redirect ESP-IDF logging to our custom vprintf
    esp_log_set_vprintf(logVprintf);
    
    return true;
}

/**
 * Stop capturing boot log (call after setup complete)
 */
void stopBootLog() {
    if (logFileOpen && bootLogFile) {
        fflush(bootLogFile);
        fclose(bootLogFile);
        bootLogFile = nullptr;
        logFileOpen = false;
        
        // Restore default vprintf
        esp_log_set_vprintf(vprintf);
        
        Serial.printf("✓ Boot log saved (%u bytes)\n", logSize);
    }
}

/**
 * Write message to both Serial and log file
 * Use this instead of Serial.println during setup
 */
void logPrint(const char* message) {
    Serial.print(message);
    
    if (logFileOpen && bootLogFile && logSize < MAX_LOG_SIZE) {
        size_t len = strlen(message);
        size_t toWrite = len;
        
        if (logSize + len > MAX_LOG_SIZE) {
            toWrite = MAX_LOG_SIZE - logSize;
        }
        
        if (toWrite > 0) {
            size_t written = fwrite(message, 1, toWrite, bootLogFile);
            logSize += written;
            fflush(bootLogFile);
        }
    }
}

/**
 * Write formatted message to both Serial and log file
 */
void logPrintf(const char* format, ...) {
    char buffer[256];
    
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    if (len > 0) {
        Serial.print(buffer);
        
        if (logFileOpen && bootLogFile && logSize < MAX_LOG_SIZE) {
            size_t toWrite = len;
            
            if (logSize + len > MAX_LOG_SIZE) {
                toWrite = MAX_LOG_SIZE - logSize;
            }
            
            if (toWrite > 0) {
                size_t written = fwrite(buffer, 1, toWrite, bootLogFile);
                logSize += written;
                fflush(bootLogFile);
            }
        }
    }
}

/**
 * Read boot log file content
 * Returns empty string if file doesn't exist
 */
String readBootLog() {
    // Ensure filesystem is mounted
    if (!filesystemMounted) {
        esp_vfs_littlefs_conf_t conf = {
            .base_path = "/littlefs",
            .partition_label = "littlefs",
            .format_if_mount_failed = false,
            .dont_mount = false,
        };
        
        esp_err_t ret = esp_vfs_littlefs_register(&conf);
        if (ret != ESP_OK) {
            return String("Error: LittleFS mount failed (") + esp_err_to_name(ret) + ")";
        }
        filesystemMounted = true;
    }
    
    // Open file
    FILE *file = fopen(BOOT_LOG_FILE, "r");
    if (!file) {
        return "No boot log available (file not found)";
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (size == 0) {
        fclose(file);
        return "Boot log is empty";
    }
    
    // Limit read size
    size_t readSize = (size > MAX_LOG_SIZE) ? MAX_LOG_SIZE : size;
    
    String content;
    content.reserve(readSize + 100);
    
    // Read in chunks
    char buffer[512];
    size_t remaining = readSize;
    
    while (remaining > 0) {
        size_t toRead = (remaining < sizeof(buffer)) ? remaining : sizeof(buffer);
        size_t bytesRead = fread(buffer, 1, toRead, file);
        
        if (bytesRead > 0) {
            content.concat(buffer, bytesRead);
            remaining -= bytesRead;
        } else {
            break;
        }
    }
    
    fclose(file);
    
    // Add footer
    char footer[128];
    snprintf(footer, sizeof(footer), "\n\n--- Log Size: %ld bytes, Read: %u bytes ---\n", 
             size, (unsigned)(readSize - remaining));
    content += footer;
    
    return content;
}

/**
 * Get boot log file size
 */
size_t getBootLogSize() {
    if (!filesystemMounted) {
        return 0;
    }
    
    FILE *file = fopen(BOOT_LOG_FILE, "r");
    if (!file) {
        return 0;
    }
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fclose(file);
    
    return (size_t)size;
}
