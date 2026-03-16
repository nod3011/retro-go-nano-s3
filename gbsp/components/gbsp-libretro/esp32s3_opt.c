/* ESP32-S3 Performance Optimization Implementation for GBSP
 *
 * Copyright (C) 2024 Retro-Go Project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include "esp32s3_opt.h"
#include <string.h>

#ifdef ESP32

// Global performance statistics
static esp32s3_perf_stats_t perf_stats = {0};
static uint32_t last_frame_time = 0;
static uint32_t frame_time_history[8] = {0};
static uint32_t history_index = 0;

void esp32s3_init_performance_monitor(void) {
    memset(&perf_stats, 0, sizeof(esp32s3_perf_stats_t));
    memset(frame_time_history, 0, sizeof(frame_time_history));
    last_frame_time = esp32s3_get_cycle_count();
    perf_stats.performance_level = 0; // Start with normal quality
}

void esp32s3_update_performance_stats(uint32_t frame_time) {
    perf_stats.frame_count++;
    perf_stats.total_render_time += frame_time;
    perf_stats.avg_frame_time = perf_stats.total_render_time / perf_stats.frame_count;
    
    // Track frame time history for trend analysis
    frame_time_history[history_index] = frame_time;
    history_index = (history_index + 1) % 8;
    
    // Calculate moving average
    uint32_t moving_avg = 0;
    for (int i = 0; i < 8; i++) {
        moving_avg += frame_time_history[i];
    }
    moving_avg /= 8;
    
    // Adjust performance level based on moving average
    uint32_t frame_time_us = moving_avg / (ESP32S3_CPU_FREQ / 1000000);
    
    if (frame_time_us > ESP32S3_FRAME_TIME_SLOW) {
        // Too slow, reduce quality
        if (perf_stats.performance_level < 2) {
            perf_stats.performance_level++;
        }
        perf_stats.dropped_frames++;
    } else if (frame_time_us < ESP32S3_FRAME_TIME_TARGET * 0.8) {
        // Running fast, can increase quality
        if (perf_stats.performance_level > 0) {
            perf_stats.performance_level--;
        }
    }
}

uint32_t esp32s3_get_optimal_cycles(bool hblank_free) {
    uint32_t base_cycles = hblank_free ? ESP32S3_MAX_SPRITE_CYCLES_REDUCED 
                                       : ESP32S3_MAX_SPRITE_CYCLES_NORMAL;
    
    // Apply performance level multiplier
    switch (perf_stats.performance_level) {
        case 2: return base_cycles * 0.5;  // 50% cycles for minimum quality
        case 1: return base_cycles * 0.75; // 75% cycles for reduced quality
        default: return base_cycles;        // 100% cycles for normal quality
    }
}

void* esp32s3_malloc_aligned(size_t size, size_t alignment) {
    return heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

void esp32s3_free_aligned(void* ptr) {
    heap_caps_free(ptr);
}

// Fast memory copy for aligned data
void* esp32s3_memcpy_aligned(void* dst, const void* src, size_t n) {
    if (n >= 16 && ((uintptr_t)dst % 16 == 0) && ((uintptr_t)src % 16 == 0)) {
        // Use 128-bit loads/stores for aligned data
        uint32_t* d32 = (uint32_t*)dst;
        const uint32_t* s32 = (const uint32_t*)src;
        size_t count = n / 16;
        
        while (count--) {
            uint32_t v0 = s32[0];
            uint32_t v1 = s32[1];
            uint32_t v2 = s32[2];
            uint32_t v3 = s32[3];
            d32[0] = v0;
            d32[1] = v1;
            d32[2] = v2;
            d32[3] = v3;
            d32 += 4;
            s32 += 4;
        }
        
        // Handle remaining bytes
        size_t remaining = n % 16;
        if (remaining) {
            memcpy(d32, s32, remaining);
        }
        
        return dst;
    } else {
        return memcpy(dst, src, n);
    }
}

#endif // ESP32
