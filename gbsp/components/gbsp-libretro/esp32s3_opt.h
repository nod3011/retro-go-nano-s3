/* ESP32-S3 Performance Optimization Header for GBSP
 *
 * Copyright (C) 2024 Retro-Go Project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef ESP32S3_OPT_H
#define ESP32S3_OPT_H

#ifdef ESP32

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "xtensa/core-macros.h"

// ESP32-S3 specific performance settings
#define ESP32S3_CPU_FREQ        240000000  // 240MHz
#define ESP32S3_CACHE_LINE_SIZE 32         // bytes

// Memory allocation preferences for ESP32-S3
#define ESP32S3_PREFER_IRAM     1
#define ESP32S3_PREFER_DRAM     0

// Video optimization settings
#define ESP32S3_MAX_SPRITE_CYCLES_NORMAL    550   // Reduced from 650 for better performance
#define ESP32S3_MAX_SPRITE_CYCLES_REDUCED   600   // Increased from 720 for hblank-free mode
#define ESP32S3_FRAME_TIME_TARGET           16666  // ~60 FPS in microseconds
#define ESP32S3_FRAME_TIME_FAST            20000  // ~50 FPS threshold
#define ESP32S3_FRAME_TIME_MEDIUM          25000  // ~40 FPS threshold
#define ESP32S3_FRAME_TIME_SLOW            33333  // ~30 FPS threshold

// Cache optimization
#define ESP32S3_ENABLE_DMA_CACHE    1
#define ESP32S3_CACHE_ALIGNMENT     16

// Dual-core task priorities
#define ESP32S3_CPU_TASK_PRIORITY    5
#define ESP32S3_VIDEO_TASK_PRIORITY  4

// Performance monitoring
typedef struct {
    uint32_t frame_count;
    uint32_t total_render_time;
    uint32_t avg_frame_time;
    uint32_t dropped_frames;
    uint32_t performance_level;
} esp32s3_perf_stats_t;

// Function declarations
void esp32s3_init_performance_monitor(void);
void esp32s3_update_performance_stats(uint32_t frame_time);
uint32_t esp32s3_get_optimal_cycles(bool hblank_free);
void* esp32s3_malloc_aligned(size_t size, size_t alignment);
void esp32s3_free_aligned(void* ptr);
void* esp32s3_memcpy_aligned(void* dst, const void* src, size_t n);

// Inline performance functions
static inline uint32_t esp32s3_get_cycle_count(void) {
    return xthal_get_ccount();
}

static inline void esp32s3_cache_writeback(void* addr, size_t size) {
    // Ensure data is written back to memory
    asm volatile("dsync");
}

static inline void esp32s3_cache_invalidate(void* addr, size_t size) {
    // Invalidate cache lines
    asm volatile("isync");
}

// Memory alignment macros
#define ESP32S3_ALIGN(n) __attribute__((aligned(n)))
#define ESP32S3_IRAM_ATTR __attribute__((section(".iram1")))
#define ESP32S3_DRAM_ATTR __attribute__((section(".dram1")))

// Critical performance functions should be in IRAM
#define ESP32S3_FAST_FUNC ESP32S3_IRAM_ATTR

#endif // ESP32

#endif // ESP32S3_OPT_H
