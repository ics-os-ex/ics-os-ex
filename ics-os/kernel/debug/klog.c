/*
   Kernel Logging System Implementation for ICS-OS
   Provides structured logging with multiple output targets and log levels
   
   Copyright (C) 2025 ICS-OS Project
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#include "klog.h"
#include "../hardware/serial/serial.h"
#include "../console/dexio.h"
#include "../stdlib/dexstdlib.h"

// Global log configuration
static klog_config_t klog_config = {
    .min_level = KLOG_INFO,           // Default to INFO level and above
    .target = KLOG_TARGET_BOTH,       // Output to both console and serial by default  
    .timestamps_enabled = 1,          // Enable timestamps
    .colors_enabled = 0,              // Disable colors by default (console might not support them)
    .subsystem_tags = 1               // Enable subsystem tags
};

// Log level names
static const char* klog_level_names[] = {
    "EMERG", "ALERT", "CRIT", "ERR", "WARN", "NOTICE", "INFO", "DEBUG"
};

// Log level colors (for console output)
static const char* klog_level_colors[] = {
    KLOG_COLOR_BRIGHT_RED,  // EMERG
    KLOG_COLOR_RED,         // ALERT  
    KLOG_COLOR_RED,         // CRIT
    KLOG_COLOR_RED,         // ERR
    KLOG_COLOR_YELLOW,      // WARN
    KLOG_COLOR_GREEN,       // NOTICE
    KLOG_COLOR_CYAN,        // INFO
    KLOG_COLOR_WHITE        // DEBUG
};

// Simple uptime counter (incremented by timer interrupts)
static DWORD klog_uptime_ticks = 0;

// Buffer for formatting log messages
static char klog_buffer[KLOG_MAX_MSG_LEN];

/*
 * Initialize the kernel logging system
 */
int klog_init(void) {
    int serial_result = 0;
    
    // Initialize serial logging if target includes serial
    if (klog_config.target & KLOG_TARGET_SERIAL) {
        serial_result = serial_log_init();
        if (serial_result != 0) {
            // If serial init fails, fall back to console only
            klog_config.target = KLOG_TARGET_CONSOLE;
        }
    }
    
    // Log initialization message
    klog_info(KLOG_SUBSYS_KERNEL, "Kernel logging system initialized (target=0x%02X, level=%s)\n", 
              klog_config.target, klog_level_names[klog_config.min_level]);
    
    if (serial_result != 0) {
        klog_warn(KLOG_SUBSYS_SERIAL, "Serial port initialization failed (code=%d), using console only\n", serial_result);
    }
    
    return 0;
}

/*
 * Set minimum log level
 */
void klog_set_level(klog_level_t level) {
    if (level <= KLOG_DEBUG) {
        klog_config.min_level = level;
        klog_info(KLOG_SUBSYS_KERNEL, "Log level changed to %s\n", klog_level_names[level]);
    }
}

/*
 * Set log output target
 */
void klog_set_target(klog_target_t target) {
    klog_config.target = target;
    klog_info(KLOG_SUBSYS_KERNEL, "Log target changed to 0x%02X\n", target);
}

/*
 * Enable/disable timestamps
 */
void klog_enable_timestamps(int enable) {
    klog_config.timestamps_enabled = enable ? 1 : 0;
}

/*
 * Enable/disable colors
 */
void klog_enable_colors(int enable) {
    klog_config.colors_enabled = enable ? 1 : 0;
}

/*
 * Enable/disable subsystem tags
 */
void klog_enable_subsystem_tags(int enable) {
    klog_config.subsystem_tags = enable ? 1 : 0;
}

/*
 * Get current configuration
 */
klog_config_t* klog_get_config(void) {
    return &klog_config;
}

/*
 * Update uptime counter (should be called by timer interrupt)
 */
void klog_update_uptime(void) {
    klog_uptime_ticks++;
}

/*
 * Format timestamp string
 */
static void format_timestamp(char* buffer, int max_len) {
    if (!klog_config.timestamps_enabled) {
        buffer[0] = '\0';
        return;
    }
    
    // Simple timestamp format: [TICKS.MSEC]
    DWORD seconds = klog_uptime_ticks / 100;  // Assuming 100 ticks per second
    DWORD msec = (klog_uptime_ticks % 100) * 10;
    
    sprintf(buffer, "[%5lu.%03lu] ", seconds, msec);
}

/*
 * Output formatted message to console
 */
static void klog_output_console(const char* message) {
    if (klog_config.target & KLOG_TARGET_CONSOLE) {
        printf("%s", message);
    }
}

/*
 * Output formatted message to serial port
 */
static void klog_output_serial(const char* message) {
    if (klog_config.target & KLOG_TARGET_SERIAL) {
        serial_log_puts(message);
    }
}

/*
 * Core logging function with printf-style formatting
 */
void klog_vprintf(klog_level_t level, const char* subsystem, const char* fmt, va_list args) {
    char timestamp[32];
    char prefix[64];
    char* msg_start;
    int prefix_len = 0;
    
    // Check if this message should be logged
    if (level > klog_config.min_level) {
        return;
    }
    
    // Format timestamp
    format_timestamp(timestamp, sizeof(timestamp));
    
    // Build prefix
    prefix[0] = '\0';
    
    if (klog_config.timestamps_enabled) {
        strcat(prefix, timestamp);
        prefix_len += strlen(timestamp);
    }
    
    if (klog_config.colors_enabled && (klog_config.target & KLOG_TARGET_CONSOLE)) {
        strcat(prefix, klog_level_colors[level]);
        prefix_len += strlen(klog_level_colors[level]);
    }
    
    if (klog_config.subsystem_tags && subsystem) {
        char temp[32];
        sprintf(temp, "[%s:%s] ", subsystem, klog_level_names[level]);
        strcat(prefix, temp);
        prefix_len += strlen(temp);
    } else {
        char temp[16];
        sprintf(temp, "[%s] ", klog_level_names[level]);
        strcat(prefix, temp);
        prefix_len += strlen(temp);
    }
    
    if (klog_config.colors_enabled && (klog_config.target & KLOG_TARGET_CONSOLE)) {
        strcat(prefix, KLOG_COLOR_RESET);
        prefix_len += strlen(KLOG_COLOR_RESET);
    }
    
    // Format the message
    strcpy(klog_buffer, prefix);
    msg_start = klog_buffer + strlen(prefix);
    
    // Use vsprintf to format the message
    vsprintf(msg_start, fmt, args);
    
    // Output to console
    if (klog_config.target & KLOG_TARGET_CONSOLE) {
        klog_output_console(klog_buffer);
    }
    
    // Output to serial (strip colors for serial output)
    if (klog_config.target & KLOG_TARGET_SERIAL) {
        if (klog_config.colors_enabled) {
            // For serial output, rebuild without colors
            char serial_buffer[KLOG_MAX_MSG_LEN];
            char serial_prefix[64];
            
            serial_prefix[0] = '\0';
            
            if (klog_config.timestamps_enabled) {
                strcat(serial_prefix, timestamp);
            }
            
            if (klog_config.subsystem_tags && subsystem) {
                char temp[32];
                sprintf(temp, "[%s:%s] ", subsystem, klog_level_names[level]);
                strcat(serial_prefix, temp);
            } else {
                char temp[16];
                sprintf(temp, "[%s] ", klog_level_names[level]);
                strcat(serial_prefix, temp);
            }
            
            strcpy(serial_buffer, serial_prefix);
            strcat(serial_buffer, msg_start);
            klog_output_serial(serial_buffer);
        } else {
            klog_output_serial(klog_buffer);
        }
    }
}

/*
 * Main logging function with printf-style formatting
 */
void klog_printf(klog_level_t level, const char* subsystem, const char* fmt, ...) {
    va_list args;
    
    va_start(args, fmt);
    klog_vprintf(level, subsystem, fmt, args);
    va_end(args);
}

// Backward compatibility: provide function symbol klog_error even if macro alias exists
#ifdef klog_error
#undef klog_error
#endif
void klog_error(const char* subsystem, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    klog_vprintf(KLOG_ERR, subsystem, fmt, args);
    va_end(args);
}

/*
 * Show current log configuration
 */
void klog_show_config(void) {
    klog_info(KLOG_SUBSYS_KERNEL, "Kernel logging configuration:\n");
    klog_info(KLOG_SUBSYS_KERNEL, "  Min Level: %s (%d)\n", klog_level_names[klog_config.min_level], klog_config.min_level);
    klog_info(KLOG_SUBSYS_KERNEL, "  Target: 0x%02X (Console:%s, Serial:%s)\n", 
              klog_config.target,
              (klog_config.target & KLOG_TARGET_CONSOLE) ? "ON" : "OFF",
              (klog_config.target & KLOG_TARGET_SERIAL) ? "ON" : "OFF");
    klog_info(KLOG_SUBSYS_KERNEL, "  Timestamps: %s\n", klog_config.timestamps_enabled ? "ON" : "OFF");
    klog_info(KLOG_SUBSYS_KERNEL, "  Colors: %s\n", klog_config.colors_enabled ? "ON" : "OFF");
    klog_info(KLOG_SUBSYS_KERNEL, "  Subsystem Tags: %s\n", klog_config.subsystem_tags ? "ON" : "OFF");
}
