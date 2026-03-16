/* ESP32-S3 Integration Layer Header for GBSP
 *
 * Copyright (C) 2024 Retro-Go Project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef ESP32S3_INTEGRATION_H
#define ESP32S3_INTEGRATION_H

#ifdef ESP32

#include <stdint.h>
#include <stddef.h>
#include "esp32s3_opt.h"

// Configuration flags
#define ENABLE_DUAL_CORE 1  // Set to 0 to disable dual-core processing

// Function declarations
void esp32s3_gbsp_init(void);
void esp32s3_gbsp_shutdown(void);

// Emulation wrappers
uint32_t esp32s3_update_gba_wrapper(uint32_t cycles);
void esp32s3_update_scanline_wrapper(void);
void esp32s3_wait_for_frame_wrapper(void);

// Performance monitoring
esp32s3_perf_stats_t esp32s3_get_performance_stats(void);
void esp32s3_print_performance_info(void);

// Memory management wrappers
void* esp32s3_gbsp_malloc(size_t size);
void esp32s3_gbsp_free(void* ptr);
void* esp32s3_gbsp_memcpy(void* dst, const void* src, size_t n);

// Macros for conditional compilation
#ifdef ESP32
#define GBSP_INIT() esp32s3_gbsp_init()
#define GBSP_SHUTDOWN() esp32s3_gbsp_shutdown()
#define GBSP_UPDATE_GBA(cycles) esp32s3_update_gba_wrapper(cycles)
#define GBSP_UPDATE_SCANLINE() esp32s3_update_scanline_wrapper()
#define GBSP_WAIT_FRAME() esp32s3_wait_for_frame_wrapper()
#define GBSP_MALLOC(size) esp32s3_gbsp_malloc(size)
#define GBSP_FREE(ptr) esp32s3_gbsp_free(ptr)
#define GBSP_MEMCPY(dst, src, n) esp32s3_gbsp_memcpy(dst, src, n)
#else
#define GBSP_INIT() do {} while(0)
#define GBSP_SHUTDOWN() do {} while(0)
#define GBSP_UPDATE_GBA(cycles) update_gba(cycles)
#define GBSP_UPDATE_SCANLINE() update_scanline()
#define GBSP_WAIT_FRAME() do {} while(0)
#define GBSP_MALLOC(size) malloc(size)
#define GBSP_FREE(ptr) free(ptr)
#define GBSP_MEMCPY(dst, src, n) memcpy(dst, src, n)
#endif

#endif // ESP32

#endif // ESP32S3_INTEGRATION_H
