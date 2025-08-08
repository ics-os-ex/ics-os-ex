#ifndef __KLOG_H__
#define __KLOG_H__

/*
   Kernel Logging System for ICS-OS
   Provides structured logging with multiple output targets and log levels
   
   Copyright (C) 2025 ICS-OS Project
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#include "../dextypes.h"
#include <stdarg.h>

// Log levels (compatible with syslog levels)
typedef enum {
    KLOG_EMERG   = 0,  // System is unusable
    KLOG_ALERT   = 1,  // Action must be taken immediately
    KLOG_CRIT    = 2,  // Critical conditions
    KLOG_ERR     = 3,  // Error conditions
    KLOG_WARNING = 4,  // Warning conditions
    KLOG_NOTICE  = 5,  // Normal but significant condition
    KLOG_INFO    = 6,  // Informational messages
    KLOG_DEBUG   = 7   // Debug-level messages
} klog_level_t;

// Log output targets
typedef enum {
    KLOG_TARGET_NONE    = 0x00,
    KLOG_TARGET_CONSOLE = 0x01,
    KLOG_TARGET_SERIAL  = 0x02,
    KLOG_TARGET_BOTH    = 0x03
} klog_target_t;

// Log configuration structure
typedef struct {
    klog_level_t min_level;      // Minimum log level to output
    klog_target_t target;        // Output target(s)
    int timestamps_enabled;      // Whether to include timestamps
    int colors_enabled;          // Whether to use colors in console output
    int subsystem_tags;          // Whether to include subsystem tags
} klog_config_t;

// Subsystem tags for better log organization
#define KLOG_SUBSYS_KERNEL   "KERN"
#define KLOG_SUBSYS_MEMORY   "MEM"
#define KLOG_SUBSYS_PROCESS  "PROC"
#define KLOG_SUBSYS_VFS      "VFS"
#define KLOG_SUBSYS_USB      "USB"
#define KLOG_SUBSYS_NETWORK  "NET"
#define KLOG_SUBSYS_SERIAL   "SER"
#define KLOG_SUBSYS_CONSOLE  "CON"
#define KLOG_SUBSYS_DEVICE   "DEV"

// Color codes for console output
#define KLOG_COLOR_RESET     "\033[0m"
#define KLOG_COLOR_RED       "\033[31m"
#define KLOG_COLOR_GREEN     "\033[32m"
#define KLOG_COLOR_YELLOW    "\033[33m"
#define KLOG_COLOR_BLUE      "\033[34m"
#define KLOG_COLOR_MAGENTA   "\033[35m"
#define KLOG_COLOR_CYAN      "\033[36m"
#define KLOG_COLOR_WHITE     "\033[37m"
#define KLOG_COLOR_BRIGHT_RED "\033[91m"

// Maximum log message length
#define KLOG_MAX_MSG_LEN     512

// Function prototypes
int klog_init(void);
void klog_set_level(klog_level_t level);
void klog_set_target(klog_target_t target);
void klog_enable_timestamps(int enable);
void klog_enable_colors(int enable);
void klog_enable_subsystem_tags(int enable);

// Core logging functions
void klog_printf(klog_level_t level, const char* subsystem, const char* fmt, ...);
void klog_vprintf(klog_level_t level, const char* subsystem, const char* fmt, va_list args);

// Convenience macros for different log levels
#define klog_emerg(subsys, fmt, ...)    klog_printf(KLOG_EMERG, subsys, fmt, ##__VA_ARGS__)
#define klog_alert(subsys, fmt, ...)    klog_printf(KLOG_ALERT, subsys, fmt, ##__VA_ARGS__)
#define klog_crit(subsys, fmt, ...)     klog_printf(KLOG_CRIT, subsys, fmt, ##__VA_ARGS__)
#define klog_err(subsys, fmt, ...)      klog_printf(KLOG_ERR, subsys, fmt, ##__VA_ARGS__)
#define klog_warn(subsys, fmt, ...)     klog_printf(KLOG_WARNING, subsys, fmt, ##__VA_ARGS__)
#define klog_notice(subsys, fmt, ...)   klog_printf(KLOG_NOTICE, subsys, fmt, ##__VA_ARGS__)
#define klog_info(subsys, fmt, ...)     klog_printf(KLOG_INFO, subsys, fmt, ##__VA_ARGS__)
#define klog_debug(subsys, fmt, ...)    klog_printf(KLOG_DEBUG, subsys, fmt, ##__VA_ARGS__)

// Backward compatibility alias (legacy code may still call klog_error)
#ifndef klog_error
#define klog_error(subsys, fmt, ...)    klog_err(subsys, fmt, ##__VA_ARGS__)
#endif

// Simplified macros without subsystem (use KERNEL as default)
#define kemerg(fmt, ...)    klog_emerg(KLOG_SUBSYS_KERNEL, fmt, ##__VA_ARGS__)
#define kalert(fmt, ...)    klog_alert(KLOG_SUBSYS_KERNEL, fmt, ##__VA_ARGS__)
#define kcrit(fmt, ...)     klog_crit(KLOG_SUBSYS_KERNEL, fmt, ##__VA_ARGS__)
#define kerr(fmt, ...)      klog_err(KLOG_SUBSYS_KERNEL, fmt, ##__VA_ARGS__)
#define kwarn(fmt, ...)     klog_warn(KLOG_SUBSYS_KERNEL, fmt, ##__VA_ARGS__)
#define knotice(fmt, ...)   klog_notice(KLOG_SUBSYS_KERNEL, fmt, ##__VA_ARGS__)
#define kinfo(fmt, ...)     klog_info(KLOG_SUBSYS_KERNEL, fmt, ##__VA_ARGS__)
#define kdebug(fmt, ...)    klog_debug(KLOG_SUBSYS_KERNEL, fmt, ##__VA_ARGS__)

// USB logging macros
#define usb_debug(fmt, ...)    klog_debug(KLOG_SUBSYS_USB, fmt, ##__VA_ARGS__)
#define usb_info(fmt, ...)     klog_info(KLOG_SUBSYS_USB, fmt, ##__VA_ARGS__)
#define usb_warn(fmt, ...)     klog_warn(KLOG_SUBSYS_USB, fmt, ##__VA_ARGS__)
#define usb_err(fmt, ...)      klog_err(KLOG_SUBSYS_USB, fmt, ##__VA_ARGS__)

// Memory logging macros  
#define mem_debug(fmt, ...)    klog_debug(KLOG_SUBSYS_MEMORY, fmt, ##__VA_ARGS__)
#define mem_info(fmt, ...)     klog_info(KLOG_SUBSYS_MEMORY, fmt, ##__VA_ARGS__)
#define mem_warn(fmt, ...)     klog_warn(KLOG_SUBSYS_MEMORY, fmt, ##__VA_ARGS__)
#define mem_err(fmt, ...)      klog_err(KLOG_SUBSYS_MEMORY, fmt, ##__VA_ARGS__)

// Function to get current configuration
klog_config_t* klog_get_config(void);

// Function to dump current log configuration
void klog_show_config(void);

#endif /* __KLOG_H__ */
