/* ESP32-S3 Integration Layer for GBSP
 *
 * Copyright (C) 2024 Retro-Go Project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include "esp32s3_integration.h"
#include "esp32s3_opt.h"
#include "esp32s3_dualcore.h"
#include "common.h"
#include "main.h"

#ifdef ESP32

static bool dualcore_initialized = false;
static bool performance_monitoring = false;

void esp32s3_gbsp_init(void) {
    // Initialize performance monitoring
    esp32s3_init_performance_monitor();
    performance_monitoring = true;
    
    // Initialize dual-core processing
#ifdef ENABLE_DUAL_CORE
    esp32s3_dualcore_init();
    dualcore_initialized = true;
    
    printf("ESP32-S3 Dual-Core GBSP initialized\n");
    printf("CPU Core: %d, Video Core: %d\n", ESP32S3_CPU_CORE, ESP32S3_VIDEO_CORE);
#else
    printf("ESP32-S3 Single-Core GBSP initialized\n");
#endif
}

void esp32s3_gbsp_shutdown(void) {
    if (dualcore_initialized) {
        esp32s3_dualcore_shutdown();
        dualcore_initialized = false;
    }
    
    performance_monitoring = false;
}

u32 esp32s3_update_gba_wrapper(u32 cycles) {
#ifdef ENABLE_DUAL_CORE
    if (dualcore_initialized) {
        // In dual-core mode, the CPU task handles emulation
        // Just return cycles processed
        return cycles;
    }
#endif
    
    // Single-core fallback
    return update_gba(cycles);
}

void esp32s3_update_scanline_wrapper(void) {
#ifdef ENABLE_DUAL_CORE
    if (dualcore_initialized) {
        // In dual-core mode, scanline processing is handled by video task
        return;
    }
#endif
    
    // Single-core fallback
    update_scanline();
}

void esp32s3_wait_for_frame_wrapper(void) {
#ifdef ENABLE_DUAL_CORE
    if (dualcore_initialized) {
        esp32s3_dualcore_wait_for_frame();
        return;
    }
#endif
    
    // Single-core fallback - just yield
    vTaskDelay(pdMS_TO_TICKS(1));
}

esp32s3_perf_stats_t esp32s3_get_performance_stats(void) {
    esp32s3_perf_stats_t stats = {0};
    
    if (performance_monitoring) {
        // Get performance stats from ESP32-S3 optimization layer
        // This would need to be implemented in esp32s3_opt.c
        stats.frame_count = 0; // Would be filled from actual stats
        stats.avg_frame_time = 0;
        stats.dropped_frames = 0;
        stats.performance_level = 0;
    }
    
    return stats;
}

void esp32s3_print_performance_info(void) {
    esp32s3_perf_stats_t stats = esp32s3_get_performance_stats();
    
    printf("=== ESP32-S3 GBSP Performance ===\n");
    printf("Frames: %u\n", stats.frame_count);
    printf("Avg Frame Time: %u us\n", stats.avg_frame_time);
    printf("Dropped Frames: %u\n", stats.dropped_frames);
    printf("Performance Level: %u\n", stats.performance_level);
    
    if (dualcore_initialized) {
        printf("Dual-Core: Active\n");
        printf("CPU Core: %d, Video Core: %d\n", ESP32S3_CPU_CORE, ESP32S3_VIDEO_CORE);
    } else {
        printf("Dual-Core: Inactive\n");
    }
    
    printf("================================\n");
}

// Memory allocation wrapper for ESP32-S3
void* esp32s3_gbsp_malloc(size_t size) {
#ifdef ESP32
    return esp32s3_malloc_aligned(size, 16);
#else
    return malloc(size);
#endif
}

void esp32s3_gbsp_free(void* ptr) {
#ifdef ESP32
    esp32s3_free_aligned(ptr);
#else
    free(ptr);
#endif
}

// Fast memory copy wrapper
void* esp32s3_gbsp_memcpy(void* dst, const void* src, size_t n) {
#ifdef ESP32
    return esp32s3_memcpy_aligned(dst, src, n);
#else
    return memcpy(dst, src, n);
#endif
}

#endif // ESP32
